#pragma once

#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#include "software_interrupt.h"

/*
 * This file contains the common dependencies of the architecture-dependent code.
 *
 * While all of the content in this file could alternatively be placed in context.h
 * above the conditional preprocessor includes, IDEs generally assume each file includes
 * their own dependent headers directly and form a clean independent subtree that
 * can be walked to resolve all symbols when the file is active
 */

typedef uint64_t reg_t;

/* Register context saved and restored on user-level, direct context switches. */
typedef enum
{
	ureg_rsp = 0,
	ureg_rip = 1,
	ureg_count
} ureg_t;

/* The enum is compared directly in assembly, so maintain integral values! */
typedef enum
{
	arch_context_unused  = 0,
	arch_context_fast    = 1,
	arch_context_slow    = 2,
	arch_context_running = 3
} arch_context_t;


struct arch_context {
	arch_context_t variant;
	reg_t          regs[ureg_count];
	mcontext_t     mctx;
};

/*
 * This is the slowpath switch to a preempted sandbox!
 * SIGUSR1 on the current thread and restore mcontext there!
 */
extern __thread struct arch_context worker_thread_base_context;

/* Cannot be inlined because called in assembly */
void __attribute__((noinline)) __attribute__((noreturn)) arch_context_mcontext_restore(void);

extern __thread volatile bool worker_thread_is_switching_context;
