#ifndef YAN_OS_TASK_H
#define YAN_OS_TASK_H

/* YanOS cooperative runtime: docs/specs/0019-cooperative-runtime.md.
 *
 * Eight fixed task slots with a static 4 KiB stack each, a cooperative
 * scheduler with no preemption, and one wait primitive whose only exit is "the
 * event happened". A task only ever stops at a line of its own code - wait,
 * yield, exit - which is the point of the design: "why did this task stop" is
 * readable rather than inferred.
 *
 * Nothing here spins on a device. `yan_os_task_wait` registers the caller as
 * the event's waiter, marks it BLOCKED and hands the CPU over; only the trap
 * handler turns a BLOCKED task back into a RUNNABLE one.
 */

/* ------------------------------------------------------------ context layout
 *
 * Word-for-word layout of the saved context, shared with os/task_switch.S.
 * The switch saves exactly the callee-saved set of the RISC-V calling
 * convention (ra, sp, s0-s11): the switch is reached through an ordinary C
 * function call, so every caller-saved register is already accounted for at
 * the call site. task.c asserts these offsets against its struct, so the two
 * descriptions cannot drift apart silently.
 */
#define YAN_OS_TASK_CTX_RA 0
#define YAN_OS_TASK_CTX_SP 4
#define YAN_OS_TASK_CTX_S0 8
#define YAN_OS_TASK_CTX_S1 12
#define YAN_OS_TASK_CTX_S2 16
#define YAN_OS_TASK_CTX_S3 20
#define YAN_OS_TASK_CTX_S4 24
#define YAN_OS_TASK_CTX_S5 28
#define YAN_OS_TASK_CTX_S6 32
#define YAN_OS_TASK_CTX_S7 36
#define YAN_OS_TASK_CTX_S8 40
#define YAN_OS_TASK_CTX_S9 44
#define YAN_OS_TASK_CTX_S10 48
#define YAN_OS_TASK_CTX_S11 52
#define YAN_OS_TASK_CTX_BYTES 56

#ifndef __ASSEMBLER__

#include <stdint.h>

#define YAN_OS_TASK_MAX 8
#define YAN_OS_TASK_STACK_SIZE 4096

typedef enum {
    YAN_OS_TASK_OK = 0,
    YAN_OS_TASK_NO_SLOT, /* all YAN_OS_TASK_MAX slots are in use          */
    YAN_OS_TASK_INVALID /* a NULL entry, or an argument the runtime refuses */
} YanOsTaskResult;

/* One waiting slot per PLIC source: in this first version an event *is* its
 * source number, which is a limitation and not a permanent ABI (SPEC). The
 * values are asserted against os/platform.h in task.c, so renumbering a source
 * there cannot leave the runtime waiting on the wrong line.
 *
 * The trap handler services the host transport channel (source 1), whose
 * IRQ_STATUS.H2G_DATA bit withdraws the level. A source without a service
 * routine is a programming error: completing it while its line is still
 * asserted would re-pend it immediately, so the handler reports it instead of
 * spinning. Waiting on such an event is therefore legal but only useful once
 * the handler knows how to withdraw that device's condition. */
typedef enum {
    YAN_OS_EVENT_TRANSPORT = 1,
    YAN_OS_EVENT_UART = 2,
    /* The event table is indexed by event, so its size is the platform's source
     * count; no event above the last usable source is addressable. Source 0 is
     * reserved and is not a waitable event. */
    YAN_OS_EVENT_MAX = 32
} YanOsEvent;

/* Every panic code has the top bit set, so no failure can be read as 0013's
 * "the Guest reached its final check" value 1. The codes are the runtime's
 * report: a host sees them in tohost, and the console line names the reason. */
#define YAN_OS_PANIC_TOP UINT32_C(0x80000000)
#define YAN_OS_PANIC_WAITER_EXISTS \
    (YAN_OS_PANIC_TOP | UINT32_C(1)) /* the event already has a waiter        */
#define YAN_OS_PANIC_INTERRUPTS_DISABLED \
    (YAN_OS_PANIC_TOP | UINT32_C(2)) /* wait / yield / exit with MIE = 0      */
#define YAN_OS_PANIC_TASK_TABLE \
    (YAN_OS_PANIC_TOP | UINT32_C(3)) /* spawn left a slot or stack illegal    */
#define YAN_OS_PANIC_EVENT_RANGE \
    (YAN_OS_PANIC_TOP | UINT32_C(4)) /* wait on an event with no slot         */
#define YAN_OS_PANIC_NO_TASK \
    (YAN_OS_PANIC_TOP | UINT32_C(5)) /* wait / yield / exit outside a task    */
#define YAN_OS_PANIC_EXIT_RETURNED \
    (YAN_OS_PANIC_TOP | UINT32_C(6)) /* yan_os_task_exit returned             */
#define YAN_OS_PANIC_UNEXPECTED_TRAP \
    (YAN_OS_PANIC_TOP | UINT32_C(7)) /* an exception, or an unrouted cause    */
#define YAN_OS_PANIC_UNSERVICED_SOURCE \
    (YAN_OS_PANIC_TOP | UINT32_C(8)) /* an interrupt for a device with no service */
#define YAN_OS_PANIC_PREDICATE \
    (YAN_OS_PANIC_TOP | UINT32_C(9)) /* wait without a predicate to ask       */

/* Claims a free slot and prepares it to run `entry(arg)` on its own stack.
 * Returns YAN_OS_TASK_NO_SLOT when all eight are in use. */
YanOsTaskResult yan_os_task_spawn(void (*entry)(void *), void *arg);

/* Hands the CPU to the next RUNNABLE task; returns when this one runs again.
 * Requires interrupts enabled. */
void yan_os_task_yield(void);

/* Retires the calling task and never returns. An entry function that returns
 * reaches this too. Requires interrupts enabled. */
void yan_os_task_exit(void);

/* The scheduler. Enables mie.MEIE and mstatus.MIE, then never returns: with no
 * RUNNABLE task it stays in its loop with interrupts enabled until one is
 * marked RUNNABLE again. */
void yan_os_sched_run(void);

/* Blocks the calling task until `predicate(context)` answers non-zero.
 *
 * The predicate is asked inside the critical section, so it must be short, must
 * not block, and may only read device or memory state; it must not call wait,
 * yield or exit. It is asked once, before the task blocks, and the caller
 * re-checks whatever it cares about after wait returns - the event may also
 * have arrived in the window between the check and the switch, in which case
 * this task is marked RUNNABLE and simply keeps running. A predicate that does
 * call one of those three panics on their precondition, because the predicate
 * itself runs with interrupts off.
 *
 * Requires interrupts enabled, and an IRQ route that already reaches the CPU:
 * the device's IRQ_ENABLE and the PLIC's enable / priority / threshold are the
 * caller's, and wait neither configures nor checks them. */
void yan_os_task_wait(YanOsEvent event, int (*predicate)(void *), void *context);

/* The trap entry: os/trap_entry.S builds the frame, calls this with the trap
 * cause CSRs and resumes at the returned pc. Exposed for the vector, not for
 * callers. */
uint32_t yan_os_trap_handler(uint32_t mcause, uint32_t mepc, uint32_t mtval);

#endif /* __ASSEMBLER__ */

#endif
