#pragma once

#include "arch/common.h"
#include "software_interrupt.h"
#include "software_interrupt_enable.h"

#define ARCH_SIG_JMP_OFF 8

/**
 * Initializes a context, zeros out registers, and sets the Instruction and
 * Stack pointers. Sets variant to unused if ip and sp are 0, fast otherwise.
 * @param actx arch_context to init
 * @param ip value to set instruction pointer to
 * @param sp value to set stack pointer to
 */
static void __attribute__((noinline)) arch_context_init(struct arch_context *actx, reg_t ip, reg_t sp)
{
	assert(actx != NULL);
	assert(software_interrupt_is_disabled);

	if (ip == 0 && sp == 0) {
		actx->variant = ARCH_CONTEXT_VARIANT_UNUSED;
	} else {
		actx->variant = ARCH_CONTEXT_VARIANT_FAST;
	}

	actx->preemptable = false;

	if (sp) {
		/*
		 * context_switch conventions: bp is expected to be on top of the stack
		 * when co-op context switching..
		 *
		 * Temporarily switches the active stack to the stack pointer stored in sp
		 * to push the stack pointer sp to the top of its own stack.
		 * This acts as the base pointer
		 */
		asm volatile("movq %%rsp, %%rbx\n\t" /* Temporarily save pointer of active stack to B */
		             "movq %%rax, %%rsp\n\t" /* Set active stack to stack pointer in A(C variable sp) */
		             "pushq %%rax\n\t"       /* Push A register (C variable sp) onto the stack at sp */
		             "movq %%rsp, %%rax\n\t" /* Write the incremented stack pointer to A(C variable sp) */
		             "movq %%rbx, %%rsp\n\t" /* Restore original stack saved in B */
		             : "=a"(sp)
		             : "a"(sp)
		             : "memory", "cc", "rbx");
	}
	// FIXME: Is the klobber list correct? Issue #129
	//   : "memory", "cc", "rbx", "rsi", "rdi");

	actx->regs[UREG_SP] = sp;
	actx->regs[UREG_IP] = ip;
}

/**
 * Restore a sandbox saved using a fastpath switch, restoring only the
 * instruction pointer and stack pointer registers rather than
 * a full mcontext, so it is less expensive than arch_mcontext_restore.
 * @param active_context - the context of the current worker thread
 * @param sandbox_context - the context that we want to restore
 */
static void
arch_context_restore(mcontext_t *active_context, struct arch_context *sandbox_context)
{
	assert(active_context != NULL);
	assert(sandbox_context != NULL);

	/* Assumption: Base Context is only ever used by arch_context_switch */
	assert(sandbox_context != &worker_thread_base_context);

	assert(sandbox_context->regs[UREG_SP]);
	assert(sandbox_context->regs[UREG_IP]);

	/* Transitioning from Fast -> Running */
	assert(sandbox_context->variant == ARCH_CONTEXT_VARIANT_FAST);
	sandbox_context->variant = ARCH_CONTEXT_VARIANT_RUNNING;

	active_context->gregs[REG_RSP] = sandbox_context->regs[UREG_SP];
	active_context->gregs[REG_RIP] = sandbox_context->regs[UREG_IP] + ARCH_SIG_JMP_OFF;
}

/**
 * Load a new sandbox that preempted an existing sandbox, restoring only the
 * instruction pointer and stack pointer registers.
 * I am unclear about setting the BP. Issue #131
 * @param active_context - the context of the current worker thread
 * @param sandbox_context - the context that we want to restore
 */
static inline void
arch_context_restore_new(mcontext_t *active_context, struct arch_context *sandbox_context)
{
	assert(active_context != NULL);
	assert(sandbox_context != NULL);

	/* Assumption: Base Context is only ever used by arch_context_switch */
	assert(sandbox_context != &worker_thread_base_context);

	assert(sandbox_context->regs[UREG_SP]);
	assert(sandbox_context->regs[UREG_IP]);

	/* Transitioning from Fast -> Running */
	assert(sandbox_context->variant == ARCH_CONTEXT_VARIANT_FAST);
	sandbox_context->variant = ARCH_CONTEXT_VARIANT_RUNNING;

	active_context->gregs[REG_RSP] = sandbox_context->regs[UREG_SP];
	active_context->gregs[REG_RIP] = sandbox_context->regs[UREG_IP];
}

/**
 * @param a - the registers and context of the thing running
 * @param b - the registers and context of what we're switching to
 * @return always returns 0, indicating success
 *
 * NULL in either of these values indicates the "no sandbox to execute" state,
 * which defaults to resuming execution of main
 */
static inline int
arch_context_switch(struct arch_context *a, struct arch_context *b)
{
	/* Assumption: Software Interrupts are disabled by caller */
	assert(software_interrupt_is_disabled);

	/* if both a and b are NULL, there is no state change */
	assert(a != NULL || b != NULL);

	/* Assumption: The caller does not switch to itself */
	assert(a != b);

	/* Set any NULLs to worker_thread_base_context to resume execution of main */
	if (a == NULL) a = &worker_thread_base_context;
	if (b == NULL) b = &worker_thread_base_context;

	/* A Transition {Unused, Running} -> Fast */
	assert(a->variant == ARCH_CONTEXT_VARIANT_UNUSED || a->variant == ARCH_CONTEXT_VARIANT_RUNNING);

	/* B Transition {Fast, Slow} -> Running */
	assert(b->variant == ARCH_CONTEXT_VARIANT_FAST || b->variant == ARCH_CONTEXT_VARIANT_SLOW);

	/* Assumption: Fastpath state is well formed */
	if (b->variant == ARCH_CONTEXT_VARIANT_FAST) {
		assert(b->regs[UREG_IP] != 0);
		assert(b->regs[UREG_SP] != 0);
	}

	reg_t *a_registers = a->regs, *b_registers = b->regs;
	assert(a_registers && b_registers);

	/* If fast switching back to a sandbox context marked as preemptable, reenable
	 * interrupts before jumping. If this is a slow context switch, defer renabling until
	 * arch_mcontext_restore
	 * TODO: What if we receive a signal inside the inline assemly?
	 */
	if (b->variant == ARCH_CONTEXT_VARIANT_FAST && b->preemptable) software_interrupt_enable();

	asm volatile(
	  /* Create a new stack frame */
	  "pushq %%rbp\n\t"       /*  stack[stack_len++] = base_pointer */
	  "movq %%rsp, %%rbp\n\t" /*  base_pointer = stack_pointer. Start new Frame */

	  /*
	   * Save Context A as a fastpath context switch
	   * Save the active stack pointer to context A.
	   * Set context A's IP to the absolute address of label 2f
	   */
	  "movq $2f, 8(%%rax)\n\t"  /* Write the address of label 2 to context a's IP. */
	  "movq %%rsp, (%%rax)\n\t" /* a_registers[0] (stack_pointer) = stack_pointer */
	  "movq $1, (%%rcx)\n\t"    /* a->variant = ARCH_CONTEXT_VARIANT_FAST; */

	  /*
	   * Execute a fastpath or slowpath context switch based on context B's variant
	   *
	   * If slow (mcontext-based), jump to label 1 to restore via a signal handler
	   * Otherwise, fall through and execute fast path.
	   */
	  "cmpq $2, (%%rdx)\n\t" /* if (b->variant == ARCH_CONTEXT_VARIANT_SLOW); */
	  "je 1f\n\t"            /* 	goto 1; restore the existing sandbox using mcontext */

	  /*
	   * Fast Path
	   * We can just write update the stack pointer and jump to the target instruction
	   */
	  "movq (%%rbx), %%rsp\n\t" /* stack_pointer = b_registers[0] (stack_pointer) */
	  "jmpq *8(%%rbx)\n\t"      /* immediate jump to b_registers[1] (instruction_pointer) */


	  /*
	   * This is after the context switch. B is now the active context.
	   */

	  /*
	   * Slow Path
	   * If the variant is ARCH_CONTEXT_VARIANT_SLOW, that means the sandbox was preempted and we need to
	   * fallback to a full mcontext-based context switch. We do this by invoking
	   * arch_context_restore_preempted, which fires a SIGUSR1 signal. The SIGUSR1 signal handler
	   * executes the mcontext-based context switch.
	   */
	  "1:\n\t"
	  "call arch_context_restore_preempted\n\t"
	  ".align 8\n\t"

	  /*
	   * This label is saved as the IP of a context during a fastpath context switch
	   */
	  "2:\n\t"
	  "movq $3, (%%rdx)\n\t" /* b->variant = ARCH_CONTEXT_VARIANT_RUNNING; */
	  ".align 8\n\t"

	  /*
	   * During slowpath context switches caused by preemption, the caller function
	   * arch_context_restore sets the variant to running in C and then restores at label
	   * 3 using the address of label 2 plus an offset equal to ARCH_SIG_JMP_OFF
	   * This must always equal the value in the .align instruction above!
	   */
	  "3:\n\t"
	  "popq %%rbp\n\t" /* base_pointer = stack[--stack_len]; Base Pointer is restored */
	  :
	  : "a"(a_registers), "b"(b_registers), "c"(&a->variant), "d"(&b->variant)
	  : "memory", "cc", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "xmm0", "xmm1", "xmm2",
	    "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14",
	    "xmm15");

	return 0;
}
