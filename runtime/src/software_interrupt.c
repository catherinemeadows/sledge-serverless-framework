#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <threads.h>
#include <unistd.h>
#include <ucontext.h>

#include "arch/context.h"
#include "current_sandbox.h"
#include "debuglog.h"
#include "listener_thread.h"
#include "local_runqueue.h"
#include "module.h"
#include "panic.h"
#include "runtime.h"
#include "sandbox_set_as_running_user.h"
#include "sandbox_types.h"
#include "scheduler.h"
#include "software_interrupt.h"

/*******************
 * Process Globals *
 ******************/

static uint64_t software_interrupt_interval_duration_in_cycles;

/******************
 * Thread Globals *
 *****************/

thread_local _Atomic static volatile sig_atomic_t software_interrupt_SIGALRM_kernel_count = 0;
thread_local _Atomic static volatile sig_atomic_t software_interrupt_SIGALRM_thread_count = 0;
thread_local _Atomic static volatile sig_atomic_t software_interrupt_SIGUSR_count         = 0;
thread_local _Atomic volatile sig_atomic_t        software_interrupt_deferred_sigalrm     = 0;
thread_local _Atomic volatile sig_atomic_t        software_interrupt_signal_depth         = 0;

_Atomic volatile sig_atomic_t *software_interrupt_deferred_sigalrm_max;

void
software_interrupt_deferred_sigalrm_max_alloc()
{
#ifdef LOG_DEFERRED_SIGALRM_MAX
	software_interrupt_deferred_sigalrm_max = calloc(runtime_worker_threads_count, sizeof(_Atomic(sig_atomic_t)));
#endif
}

void
software_interrupt_deferred_sigalrm_max_free()
{
#ifdef LOG_DEFERRED_SIGALRM_MAX
	if (software_interrupt_deferred_sigalrm_max) free((void *)software_interrupt_deferred_sigalrm_max);
#endif
}

void
software_interrupt_deferred_sigalrm_max_print()
{
#ifdef LOG_DEFERRED_SIGALRM_MAX
	printf("Max Deferred Sigalrms\n");
	for (int i = 0; i < runtime_worker_threads_count; i++) {
		printf("Worker %d: %d\n", i, software_interrupt_deferred_sigalrm_max[i]);
	}
	fflush(stdout);
#endif
}

/***************************************
 * Externs
 **************************************/

extern pthread_t *runtime_worker_threads;

/**************************
 * Private Static Inlines *
 *************************/

/**
 * A POSIX signal is delivered to only one thread.
 * This function broadcasts the sigalarm signal to all other worker threads
 */
static inline void
sigalrm_propagate_workers(siginfo_t *signal_info)
{
	/* Signal was sent directly by the kernel, so forward to other threads */
	if (signal_info->si_code == SI_KERNEL) {
		atomic_fetch_add(&software_interrupt_SIGALRM_kernel_count, 1);
		for (int i = 0; i < runtime_worker_threads_count; i++) {
			/* All threads should have been initialized */
			assert(runtime_worker_threads[i] != 0);

			if (pthread_self() == runtime_worker_threads[i]) continue;

			switch (runtime_sigalrm_handler) {
			case RUNTIME_SIGALRM_HANDLER_TRIAGED: {
				if (scheduler_worker_would_preempt(i)) pthread_kill(runtime_worker_threads[i], SIGALRM);
				continue;
			}
			case RUNTIME_SIGALRM_HANDLER_BROADCAST: {
				pthread_kill(runtime_worker_threads[i], SIGALRM);
				continue;
			}
			default: {
				panic("Unexpected SIGALRM Handler: %d\n", runtime_sigalrm_handler);
			}
			}
		}
	} else {
		atomic_fetch_add(&software_interrupt_SIGALRM_thread_count, 1);
		/* Signal forwarded from another thread. Just confirm it resulted from pthread_kill */
		assert(signal_info->si_code == SI_TKILL);
	}
}

/**
 * Validates that the thread running the signal handler is a known worker thread
 */
static inline void
software_interrupt_validate_worker()
{
#ifndef NDEBUG
	if (listener_thread_is_running()) panic("The listener thread unexpectedly received a signal!");
#endif
}

/**
 * The handler function for Software Interrupts (signals)
 * SIGALRM is executed periodically by an interval timer, causing preemption of the current sandbox
 * SIGUSR1 restores a preempted sandbox
 * @param signal_type
 * @param signal_info data structure containing signal info
 * @param interrupted_context_raw void* to a interrupted_context struct
 */
static inline void
software_interrupt_handle_signals(int signal_type, siginfo_t *signal_info, void *interrupted_context_raw)
{
	/* Only workers should receive signals */
	assert(!listener_thread_is_running());

	/* Signals should be masked if runtime has disabled them */
	assert(runtime_preemption_enabled);

	/* Signals should not nest */
	assert(software_interrupt_signal_depth == 0);
	atomic_fetch_add(&software_interrupt_signal_depth, 1);

	ucontext_t *    interrupted_context = (ucontext_t *)interrupted_context_raw;
	struct sandbox *current_sandbox     = current_sandbox_get();

	switch (signal_type) {
	case SIGALRM: {
		sigalrm_propagate_workers(signal_info);

		/* Nonpreemptive, so defer */
		if (!sandbox_is_preemptable(current_sandbox)) {
			atomic_fetch_add(&software_interrupt_deferred_sigalrm, 1);
			goto done;
		}

		scheduler_preemptive_sched(interrupted_context);

		goto done;
	}
	case SIGUSR1: {
		assert(current_sandbox);
		assert(current_sandbox->state == SANDBOX_PREEMPTED);
		assert(current_sandbox->ctxt.variant == ARCH_CONTEXT_VARIANT_SLOW);

		atomic_fetch_add(&software_interrupt_SIGUSR_count, 1);
#ifdef LOG_PREEMPTION
		debuglog("Total SIGUSR1 Received: %d\n", software_interrupt_SIGUSR_count);
		debuglog("Restoring sandbox: %lu, Stack %llu\n", current_sandbox->id,
		         current_sandbox->ctxt.mctx.gregs[REG_RSP]);
#endif
		/* It is the responsibility of the caller to invoke current_sandbox_set before triggering the SIGUSR1 */
		scheduler_preemptive_switch_to(interrupted_context, current_sandbox);
		goto done;
	}
	default: {
		switch (signal_info->si_code) {
		case SI_TKILL:
			panic("Unexpectedly received signal %d from a thread kill, but we have no handler\n",
			      signal_type);
		case SI_KERNEL:
			panic("Unexpectedly received signal %d from the kernel, but we have no handler\n", signal_type);
		default:
			panic("Anomolous Signal\n");
		}
	}
	}
done:
	atomic_fetch_sub(&software_interrupt_signal_depth, 1);
	return;
}

/********************
 * Public Functions *
 *******************/

/**
 * Arms the Interval Timer to start in one quantum and then trigger a SIGALRM every quantum
 */
void
software_interrupt_arm_timer(void)
{
	if (!runtime_preemption_enabled) return;

	struct itimerval interval_timer;

	memset(&interval_timer, 0, sizeof(struct itimerval));
	interval_timer.it_value.tv_usec    = runtime_quantum_us;
	interval_timer.it_interval.tv_usec = runtime_quantum_us;

	int return_code = setitimer(ITIMER_REAL, &interval_timer, NULL);
	if (return_code) {
		perror("setitimer");
		exit(1);
	}
}

/**
 * Disarm the Interval Timer
 */
void
software_interrupt_disarm_timer(void)
{
	struct itimerval interval_timer;

	memset(&interval_timer, 0, sizeof(struct itimerval));
	interval_timer.it_value.tv_sec     = 0;
	interval_timer.it_interval.tv_usec = 0;

	int return_code = setitimer(ITIMER_REAL, &interval_timer, NULL);
	if (return_code) {
		perror("setitimer");
		exit(1);
	}
}


/**
 * Initialize software Interrupts
 * Register softint_handler to execute on SIGALRM and SIGUSR1
 */
void
software_interrupt_initialize(void)
{
	struct sigaction signal_action;
	memset(&signal_action, 0, sizeof(struct sigaction));
	signal_action.sa_sigaction = software_interrupt_handle_signals;
	signal_action.sa_flags     = SA_SIGINFO | SA_RESTART;

	/* all threads created by the calling thread will have signal blocked */
	/* TODO: What does sa_mask do? I have to call software_interrupt_mask_signal below */
	sigemptyset(&signal_action.sa_mask);
	sigaddset(&signal_action.sa_mask, SIGALRM);
	sigaddset(&signal_action.sa_mask, SIGUSR1);

	const int    supported_signals[]   = { SIGALRM, SIGUSR1 };
	const size_t supported_signals_len = 2;

	for (int i = 0; i < supported_signals_len; i++) {
		int signal = supported_signals[i];
		software_interrupt_mask_signal(signal);
		if (sigaction(signal, &signal_action, NULL)) {
			perror("sigaction");
			exit(1);
		}
	}

	software_interrupt_deferred_sigalrm_max_alloc();
}

void
software_interrupt_set_interval_duration(uint64_t cycles)
{
	software_interrupt_interval_duration_in_cycles = cycles;
}
