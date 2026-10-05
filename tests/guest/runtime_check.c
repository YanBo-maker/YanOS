/* Guest self-check for the cooperative runtime: docs/specs/0019-cooperative-runtime.md.
 *
 * One scenario per image, selected with -DYAN_RT_SCENARIO=<number> (the build
 * script does that, the way run_console.sh builds console_check.c). Every
 * scenario ends by writing tohost: 1 means "the Guest reached its final check",
 * anything else is a failure code with the top bit set (0013). The test's own
 * assertion codes carry 0x40000000 so a host can tell them apart from the
 * runtime's panic codes (0x80000000 | reason).
 *
 * The image links the runtime under test (os/task.c, os/task_switch.S,
 * os/trap_entry.S) and os/console.c, so the panic diagnostic can be checked
 * too. It does not use tests/guest/mtrap.c: os/trap_entry.S owns the trap
 * vector and the frame layout here, and the helpers of the M1 environment
 * belong to the other entry point.
 *
 * rt_probe is the window the host-side driver watches. It is a plain struct of
 * 32-bit words on purpose: the driver runs on the host and cannot include
 * os/task.h (the Guest's pointer size is not the host's), so it reads these
 * words by symbol name and nothing else.
 */
#include <stddef.h>
#include <stdint.h>

#include "guest.h"
#include "platform.h"
#include "task.h"

/* --------------------------------------------------------------- scenarios */

#define YAN_RT_SCENARIO_PREDICATE_TRUE 1
#define YAN_RT_SCENARIO_WAITER_EXISTS 2
#define YAN_RT_SCENARIO_PRECONDITION_YIELD 3
#define YAN_RT_SCENARIO_PRECONDITION_WAIT 4
#define YAN_RT_SCENARIO_PRECONDITION_EXIT 5
#define YAN_RT_SCENARIO_WAKE 6
#define YAN_RT_SCENARIO_SECOND_WAIT 7
#define YAN_RT_SCENARIO_IDLE 8
#define YAN_RT_SCENARIO_CONTEXT 9
#define YAN_RT_SCENARIO_SPAWN 10
#define YAN_RT_SCENARIO_NO_SWITCH 11

#if !defined(YAN_RT_SCENARIO)
#error "build this image with -DYAN_RT_SCENARIO=<scenario>"
#endif

/* ------------------------------------------------------------ failure codes */

#define RT_FAIL(reason) (UINT32_C(0x40000000) | (reason))

#define RT_FAIL_PREDICATE_MIE 1u      /* the predicate ran with interrupts enabled */
#define RT_FAIL_SWITCHED 2u           /* a context switch happened where none may */
#define RT_FAIL_PREDICATE_CALLS 3u    /* the predicate was not asked exactly once */
#define RT_FAIL_READY_SEEN 4u         /* the predicate answered the wrong thing */
#define RT_FAIL_DATA_LOST 5u          /* the injected byte is not in the ring */
#define RT_FAIL_TRACE_RAN 6u          /* the trace task ran: a switch happened */
#define RT_FAIL_MIE_NOT_RESTORED 7u   /* mstatus.MIE after wait != before wait */
#define RT_FAIL_NOT_SERVICED 8u       /* the ISR did not withdraw the device line */
#define RT_FAIL_NO_WAKEUP 9u          /* the injected event never woke the waiter */
#define RT_FAIL_POP 10u               /* consuming the published byte failed */
#define RT_FAIL_NO_PANIC 11u          /* a call that must panic returned instead */
#define RT_FAIL_UNEXPECTED_WAKE 12u   /* a task that must stay blocked was woken */
#define RT_FAIL_CANARY 13u            /* a callee-saved register did not survive */
#define RT_FAIL_STACK 14u             /* sp left the running task's own stack */
#define RT_FAIL_STACK_OVERLAP 15u     /* the task stacks are not distinct */
#define RT_FAIL_SPAWN_RESULT 16u      /* spawn answered the wrong result */
#define RT_FAIL_MEIE_OFF 17u          /* the scheduler did not enable mie.MEIE */
#define RT_FAIL_CONTEXT_TIMEOUT 18u   /* the context rounds did not finish */

/* rt_finish() loops forever, but the image has to say so: without it
 * every main() looks like it can fall off its end. */
__attribute__((noreturn)) static void rt_finish(uint32_t code)
{
    guest_finish(code); /* writes tohost and loops forever */
    for (;;) {
    }
}


/* ------------------------------------------------------------------- probe */

/* mie.MEIE. The runtime owns this bit (SPEC: the scheduler enables mie in its
 * own entry); os/platform.h names the PLIC side of the route, not the CPU
 * side, so the bit is restated here for the test's assertion. */
#define RT_MIE_MEIE UINT32_C(0x800)

#define RT_PROBE_MAGIC UINT32_C(0x59414e52) /* "YANR" */

typedef struct {
    uint32_t magic;           /* RT_PROBE_MAGIC; the host checks the symbol */
    uint32_t predicate_calls; /* one per predicate call, written after the answer */
    uint32_t running_bits;    /* one bit per task, set only while it executes */
    uint32_t wake_returns;    /* one per wait that returned */
    uint32_t switch_traces;   /* bumped by the trace task every time it runs */
    uint32_t stage;           /* scenario progress the host injects on */
    uint32_t t_ticks;         /* the interrupted task's own progress counter */
    uint32_t stack_marks[2];  /* entry-frame addresses of the two context tasks */
    uint32_t context_done;    /* bits of the tasks that finished their rounds */
} RtProbe;

/* The only thing the host driver knows about this image besides tohost. */
volatile RtProbe rt_probe;

typedef struct {
    uint32_t calls;      /* how often this predicate was asked */
    uint32_t ready_seen; /* how often it answered "the event has happened" */
} RtWaitContext;

/* ------------------------------------------------------------- CPU state */

static uint32_t rt_mstatus(void)
{
    uint32_t value = 0;
    __asm__ volatile ("csrr %0, mstatus" : "=r"(value));
    return value & UINT32_C(0x8);
}

/* Only the scenarios that check the runtime's gate use this one. */
__attribute__((unused)) static uint32_t rt_mie(void)
{
    uint32_t value = 0;
    __asm__ volatile ("csrr %0, mie" : "=r"(value));
    return value;
}

/* Only the stack check in the context scenario uses this one. */
__attribute__((unused)) static uint32_t rt_sp(void)
{
    uint32_t value = 0;
    __asm__ volatile ("mv %0, sp" : "=r"(value));
    return value;
}

/* ------------------------------------------------------- running-bit trace
 *
 * A task sets its bit immediately after it is resumed and clears it
 * immediately before it hands the CPU over. The host asserts that at most one
 * bit is ever set: an interrupt handler that switches tasks would leave the
 * interrupted task's bit set while the woken task sets its own.
 */
static void rt_enter(uint32_t bit)
{
    rt_probe.running_bits |= bit;
}

static void rt_leave(uint32_t bit)
{
    rt_probe.running_bits &= ~bit;
}

static void rt_yield(uint32_t bit)
{
    rt_leave(bit);
    yan_os_task_yield();
    rt_enter(bit);
}

__attribute__((unused)) static void rt_wait(uint32_t bit, int (*predicate)(void *), RtWaitContext *state)
{
    rt_leave(bit);
    yan_os_task_wait(YAN_OS_EVENT_TRANSPORT, predicate, state);
    rt_enter(bit);
}

/* -------------------------------------------------------------- predicate
 *
 * The runtime calls this inside its critical section (SPEC: the predicate runs
 * with interrupts disabled), so the assertion below is part of the contract,
 * not a convenience. The last thing the predicate does is bump the host's
 * counter: the host injects the event at the next instruction boundary, which
 * is therefore after the predicate has answered and before wait has decided
 * anything. That is the window the adversarial case is about.
 */
__attribute__((unused)) static int rt_predicate(void *context)
{
    RtWaitContext *state = context;
    const int ready = yan_os_transport_h2g_available() != 0;
    const uint32_t mie_set = rt_mstatus();

    ++state->calls;
    if (ready) {
        ++state->ready_seen;
    }
    if (mie_set != 0) {
        rt_finish(RT_FAIL(RT_FAIL_PREDICATE_MIE));
    }
    ++rt_probe.predicate_calls;
    return ready;
}

/* ------------------------------------------------------------- register canary
 *
 * uint32_t rt_register_canary(uint32_t magic, uint32_t running_bit);
 *
 * Loads s0-s11 with `magic`, yields - which switches to the other task and
 * back - and compares every callee-saved register with the value it held.
 * Returns 0 when they all survived, nonzero otherwise. The incoming values are
 * saved on the frame and restored before returning, so the C code around it
 * sees a well-behaved callee. The yield goes through rt_canary_yield() so the
 * host-visible running bit is maintained across it, exactly as rt_yield() does
 * for the C callers.
 */
uint32_t rt_register_canary(uint32_t magic, uint32_t running_bit);

__asm__(
    "  .text\n"
    "  .align 2\n"
    "  .globl rt_register_canary\n"
    "  .type rt_register_canary, @function\n"
    "rt_register_canary:\n"
    "  addi sp, sp, -64\n"
    "  sw ra, 60(sp)\n"
    "  sw s0, 0(sp)\n"
    "  sw s1, 4(sp)\n"
    "  sw s2, 8(sp)\n"
    "  sw s3, 12(sp)\n"
    "  sw s4, 16(sp)\n"
    "  sw s5, 20(sp)\n"
    "  sw s6, 24(sp)\n"
    "  sw s7, 28(sp)\n"
    "  sw s8, 32(sp)\n"
    "  sw s9, 36(sp)\n"
    "  sw s10, 40(sp)\n"
    "  sw s11, 44(sp)\n"
    "  sw a0, 48(sp)\n"
    "  sw a1, 52(sp)\n"
    "  mv s0, a0\n"
    "  mv s1, a0\n"
    "  mv s2, a0\n"
    "  mv s3, a0\n"
    "  mv s4, a0\n"
    "  mv s5, a0\n"
    "  mv s6, a0\n"
    "  mv s7, a0\n"
    "  mv s8, a0\n"
    "  mv s9, a0\n"
    "  mv s10, a0\n"
    "  mv s11, a0\n"
    "  lw a0, 52(sp)\n"
    "  call rt_canary_yield\n"
    "  lw t0, 48(sp)\n"
    "  xor t1, s0, t0\n"
    "  xor t2, s1, t0\n"
    "  or t1, t1, t2\n"
    "  xor t2, s2, t0\n"
    "  or t1, t1, t2\n"
    "  xor t2, s3, t0\n"
    "  or t1, t1, t2\n"
    "  xor t2, s4, t0\n"
    "  or t1, t1, t2\n"
    "  xor t2, s5, t0\n"
    "  or t1, t1, t2\n"
    "  xor t2, s6, t0\n"
    "  or t1, t1, t2\n"
    "  xor t2, s7, t0\n"
    "  or t1, t1, t2\n"
    "  xor t2, s8, t0\n"
    "  or t1, t1, t2\n"
    "  xor t2, s9, t0\n"
    "  or t1, t1, t2\n"
    "  xor t2, s10, t0\n"
    "  or t1, t1, t2\n"
    "  xor t2, s11, t0\n"
    "  or t1, t1, t2\n"
    "  mv a0, t1\n"
    "  lw ra, 60(sp)\n"
    "  lw s0, 0(sp)\n"
    "  lw s1, 4(sp)\n"
    "  lw s2, 8(sp)\n"
    "  lw s3, 12(sp)\n"
    "  lw s4, 16(sp)\n"
    "  lw s5, 20(sp)\n"
    "  lw s6, 24(sp)\n"
    "  lw s7, 28(sp)\n"
    "  lw s8, 32(sp)\n"
    "  lw s9, 36(sp)\n"
    "  lw s10, 40(sp)\n"
    "  lw s11, 44(sp)\n"
    "  addi sp, sp, 64\n"
    "  ret\n"
    "  .size rt_register_canary, .-rt_register_canary\n");

/* The canary's yield: the same bookkeeping as rt_yield(), reachable from the
 * assembly above. */
__attribute__((used, noinline)) static void rt_canary_yield(uint32_t bit)
{
    rt_yield(bit);
}

/* ------------------------------------------------------------- platform init
 *
 * The IRQ route below the CPU is the caller's business (SPEC): the device's
 * IRQ_ENABLE and the PLIC's enable / priority / threshold. mie.MEIE is the
 * runtime's, so it is not touched here.
 */
static void rt_configure_platform(void)
{
    yan_os_transport_set_irq_enable(1);
    yan_os_plic_set_priority(YAN_OS_PLIC_SOURCE_TRANSPORT, 1);
    yan_os_plic_set_threshold(0);
    yan_os_plic_enable(YAN_OS_PLIC_SOURCE_TRANSPORT, 1);
    rt_probe.magic = RT_PROBE_MAGIC;
    rt_probe.stage = 1;
}

/* =============================================================== scenario 1
 * The event has already happened when wait is entered: no blocking, and no
 * context switch.
 *
 * What asserts that is the trace task: it is runnable, so any switch away from
 * the waiter gives it the CPU, and it reports the failure itself
 * (rt_probe.switch_traces != 0 is the same fact in the waiter's own check).
 * Together with "the predicate was asked exactly once" and "the byte is still
 * there", that is the whole scenario.
 *
 * A register-level trace - compute a value before wait, check it after - is
 * deliberately *not* used here: at -O2 the compiler proves a callee-saved
 * register cannot change across the call and folds the comparison away (the
 * constant 0x9e3779b9 does not appear in the image's .text at all, and an
 * earlier version of this scenario carried exactly such a dead assertion). The
 * check that does bite on register preservation is the canary in the context
 * scenario, which compares all twelve registers in assembly around a real
 * switch.
 */
#if YAN_RT_SCENARIO == YAN_RT_SCENARIO_PREDICATE_TRUE
static void rt_task_waiter(void *arg)
{
    const uint32_t bit = (uint32_t)(uintptr_t)arg;
    RtWaitContext state = {0, 0};

    rt_enter(bit);
    rt_wait(bit, rt_predicate, &state);
    if (rt_probe.switch_traces != 0) {
        rt_finish(RT_FAIL(RT_FAIL_SWITCHED));
    }
    if (state.calls != 1) {
        rt_finish(RT_FAIL(RT_FAIL_PREDICATE_CALLS));
    }
    if (state.ready_seen != 1) {
        rt_finish(RT_FAIL(RT_FAIL_READY_SEEN));
    }
    if (yan_os_transport_h2g_available() != 1) {
        rt_finish(RT_FAIL(RT_FAIL_DATA_LOST));
    }
    rt_leave(bit);
    rt_finish(1);
}

static void rt_task_trace(void *arg)
{
    const uint32_t bit = (uint32_t)(uintptr_t)arg;
    rt_enter(bit);
    ++rt_probe.switch_traces;
    /* Running at all is the failure: wait returned only after a switch. */
    rt_finish(RT_FAIL(RT_FAIL_TRACE_RAN));
}
#endif

/* =============================================================== scenario 2
 * A second wait on an event that already has a waiter is a programming error:
 * the runtime panics with an observable failure code instead of hanging or
 * overwriting the waiter. The host expects tohost == PANIC_WAITER_EXISTS.
 * Nothing is injected here, so the first wait really blocks.
 */
#if YAN_RT_SCENARIO == YAN_RT_SCENARIO_WAITER_EXISTS
static void rt_task_first_waiter(void *arg)
{
    const uint32_t bit = (uint32_t)(uintptr_t)arg;
    RtWaitContext state = {0, 0};

    rt_enter(bit);
    rt_wait(bit, rt_predicate, &state);
    /* Reaching here means something woke a task that had nothing to wait for. */
    rt_finish(RT_FAIL(RT_FAIL_UNEXPECTED_WAKE));
}

static void rt_task_second_waiter(void *arg)
{
    const uint32_t bit = (uint32_t)(uintptr_t)arg;
    RtWaitContext state = {0, 0};

    rt_enter(bit);
    /* The event has no data and already has a waiter: step 3 must panic. */
    rt_wait(bit, rt_predicate, &state);
    rt_finish(RT_FAIL(RT_FAIL_NO_PANIC));
}
#endif

/* ============================================================ scenarios 3-5
 * The three calls that may only be made with interrupts enabled. Each one is
 * called from a context with mstatus.MIE cleared: the runtime must panic with
 * the precondition code rather than run a critical section over a caller that
 * already had interrupts off.
 */
#if YAN_RT_SCENARIO == YAN_RT_SCENARIO_PRECONDITION_YIELD || \
    YAN_RT_SCENARIO == YAN_RT_SCENARIO_PRECONDITION_WAIT || \
    YAN_RT_SCENARIO == YAN_RT_SCENARIO_PRECONDITION_EXIT
static void rt_interrupts_off(void)
{
    __asm__ volatile ("csrci mstatus, 8" ::: "memory");
}
#endif

/* =============================================================== scenario 6
 * The adversarial case: the host injects the event after the predicate has
 * answered "not yet" and before the task switches away - inside the critical
 * section. The wakeup can only come from the interrupt handler, and the
 * watchdog task fails the run if wait never returns.
 */
#if YAN_RT_SCENARIO == YAN_RT_SCENARIO_WAKE
#define RT_WATCHDOG_YIELDS 64u

static void rt_task_waiter(void *arg)
{
    const uint32_t bit = (uint32_t)(uintptr_t)arg;
    RtWaitContext state = {0, 0};
    uint8_t byte = 0;

    rt_enter(bit);
    /* The precondition of wait: interrupts on, and the runtime enabled MEIE. */
    if (rt_mstatus() == 0) {
        rt_finish(RT_FAIL(RT_FAIL_MIE_NOT_RESTORED));
    }
    if ((rt_mie() & RT_MIE_MEIE) == 0) {
        rt_finish(RT_FAIL(RT_FAIL_MEIE_OFF));
    }
    rt_wait(bit, rt_predicate, &state);
    ++rt_probe.wake_returns;
    /* wait returned with the interrupt state it was entered with... */
    if (rt_mstatus() == 0) {
        rt_finish(RT_FAIL(RT_FAIL_MIE_NOT_RESTORED));
    }
    /* ...because the handler serviced the device and marked this task. */
    if (yan_os_transport_irq_status() != 0) {
        rt_finish(RT_FAIL(RT_FAIL_NOT_SERVICED));
    }
    if (yan_os_transport_h2g_available() != 1) {
        rt_finish(RT_FAIL(RT_FAIL_DATA_LOST));
    }
    if (state.calls != 1 || state.ready_seen != 0) {
        rt_finish(RT_FAIL(RT_FAIL_PREDICATE_CALLS));
    }
    if (yan_os_transport_h2g_pop(&byte, 1) != 0) {
        rt_finish(RT_FAIL(RT_FAIL_POP));
    }
    rt_leave(bit);
    rt_finish(1);
}

static void rt_task_watchdog(void *arg)
{
    const uint32_t bit = (uint32_t)(uintptr_t)arg;

    rt_enter(bit);
    for (uint32_t round = 0; round < RT_WATCHDOG_YIELDS; ++round) {
        if (rt_probe.wake_returns != 0) {
            rt_leave(bit);
            return; /* the waiter was woken; it reports the verdict itself */
        }
        rt_yield(bit);
    }
    rt_leave(bit);
    rt_finish(RT_FAIL(RT_FAIL_NO_WAKEUP));
}
#endif

/* =============================================================== scenario 7
 * Two waits in a row on the same event. The second one can only register if
 * the first wake emptied the waiter slot; a runtime that leaves it behind
 * reaches the "already has a waiter" panic instead of waiting again.
 */
#if YAN_RT_SCENARIO == YAN_RT_SCENARIO_SECOND_WAIT
static void rt_task_two_waits(void *arg)
{
    const uint32_t bit = (uint32_t)(uintptr_t)arg;
    RtWaitContext first = {0, 0};
    RtWaitContext second = {0, 0};
    uint8_t byte = 0;

    rt_enter(bit);
    rt_wait(bit, rt_predicate, &first);
    ++rt_probe.wake_returns;
    if (yan_os_transport_h2g_pop(&byte, 1) != 0) {
        rt_finish(RT_FAIL(RT_FAIL_POP));
    }
    /* The event is quiet again: this wait must register, not panic. */
    rt_wait(bit, rt_predicate, &second);
    ++rt_probe.wake_returns;
    if (yan_os_transport_h2g_pop(&byte, 1) != 0) {
        rt_finish(RT_FAIL(RT_FAIL_POP));
    }
    if (first.calls != 1 || second.calls != 1) {
        rt_finish(RT_FAIL(RT_FAIL_PREDICATE_CALLS));
    }
    rt_leave(bit);
    rt_finish(1);
}
#endif

/* =============================================================== scenario 8
 * Every task is out of the way: one is blocked, the other exited. The
 * scheduler must stay in its loop with interrupts enabled (it does not panic
 * and does not leave) until the interrupt marks the waiter runnable again.
 */
#if YAN_RT_SCENARIO == YAN_RT_SCENARIO_IDLE
static void rt_task_idle_waiter(void *arg)
{
    const uint32_t bit = (uint32_t)(uintptr_t)arg;
    RtWaitContext state = {0, 0};
    uint8_t byte = 0;

    rt_enter(bit);
    rt_wait(bit, rt_predicate, &state);
    ++rt_probe.wake_returns;
    if (yan_os_transport_irq_status() != 0) {
        rt_finish(RT_FAIL(RT_FAIL_NOT_SERVICED));
    }
    if (yan_os_transport_h2g_available() != 1) {
        rt_finish(RT_FAIL(RT_FAIL_DATA_LOST));
    }
    if (yan_os_transport_h2g_pop(&byte, 1) != 0) {
        rt_finish(RT_FAIL(RT_FAIL_POP));
    }
    rt_leave(bit);
    rt_finish(1);
}

static void rt_task_leaves(void *arg)
{
    const uint32_t bit = (uint32_t)(uintptr_t)arg;
    rt_enter(bit);
    rt_probe.stage = 2; /* the host waits for this, then injects */
    rt_leave(bit);
    yan_os_task_exit();
    rt_finish(RT_FAIL(RT_FAIL_NO_PANIC)); /* exit must not return */
}
#endif

/* =============================================================== scenario 9
 * Two tasks alternate: each runs the callee-saved canary across a switch and
 * checks that sp still points into its own 4 KiB stack. The stacks are
 * compared afterwards to prove they are distinct regions.
 */
#if YAN_RT_SCENARIO == YAN_RT_SCENARIO_CONTEXT
#define RT_CONTEXT_ROUNDS 4u
#define RT_CANARY_BASE UINT32_C(0xa5a50000)

static int rt_stack_ok(uint32_t mark)
{
    const uint32_t sp = rt_sp();
    return sp <= mark && (mark - sp) < YAN_OS_TASK_STACK_SIZE;
}

static void rt_task_context(void *arg)
{
    const uint32_t bit = (uint32_t)(uintptr_t)arg;
    const uint32_t index = bit == 1u ? 0u : 1u;
    volatile uint32_t here = 0;

    rt_enter(bit);
    rt_probe.stack_marks[index] = (uint32_t)(uintptr_t)&here;
    for (uint32_t round = 0; round < RT_CONTEXT_ROUNDS; ++round) {
        if (rt_register_canary(RT_CANARY_BASE | bit, bit) != 0) {
            rt_finish(RT_FAIL(RT_FAIL_CANARY));
        }
        if (!rt_stack_ok(rt_probe.stack_marks[index])) {
            rt_finish(RT_FAIL(RT_FAIL_STACK));
        }
        rt_yield(bit);
    }
    rt_probe.context_done |= bit;
    if (rt_probe.context_done == 3u) {
        const uint32_t first = rt_probe.stack_marks[0];
        const uint32_t second = rt_probe.stack_marks[1];
        const uint32_t distance = first > second ? first - second : second - first;
        if (distance < YAN_OS_TASK_STACK_SIZE) {
            rt_finish(RT_FAIL(RT_FAIL_STACK_OVERLAP));
        }
        rt_leave(bit);
        rt_finish(1);
    }
    rt_leave(bit);
    /* The other task reports the verdict; this one stays out of the way. */
    for (;;) {
        rt_yield(bit);
    }
}
#endif

/* ============================================================== scenario 10
 * The slot contract: eight slots, a ninth spawn refused with NO_SLOT, a NULL
 * entry refused with INVALID, and a task entry that returns ending in
 * yan_os_task_exit. The last spawned task reports the verdict.
 */
#if YAN_RT_SCENARIO == YAN_RT_SCENARIO_SPAWN
#define RT_SPAWN_TASKS 8u

static void rt_task_quiet(void *arg)
{
    (void)arg;
    /* Returning from an entry is an exit (SPEC). */
}

static void rt_task_reports(void *arg)
{
    const uint32_t bit = (uint32_t)(uintptr_t)arg;
    (void)bit;
    rt_finish(1);
}
#endif

/* ============================================================== scenario 11
 * An interrupt taken while this task runs must return to this task: the
 * handler marks the waiter and does not switch. The interrupted task checks
 * that the handler serviced the device and that no other task ran.
 */
#if YAN_RT_SCENARIO == YAN_RT_SCENARIO_NO_SWITCH
static void rt_task_waiter(void *arg)
{
    const uint32_t bit = (uint32_t)(uintptr_t)arg;
    RtWaitContext state = {0, 0};
    uint8_t byte = 0;

    rt_enter(bit);
    rt_wait(bit, rt_predicate, &state); /* blocks: the host injects later */
    ++rt_probe.wake_returns;
    if (yan_os_transport_irq_status() != 0) {
        rt_finish(RT_FAIL(RT_FAIL_NOT_SERVICED));
    }
    if (yan_os_transport_h2g_available() != 1) {
        rt_finish(RT_FAIL(RT_FAIL_DATA_LOST));
    }
    if (yan_os_transport_h2g_pop(&byte, 1) != 0) {
        rt_finish(RT_FAIL(RT_FAIL_POP));
    }
    rt_leave(bit);
    rt_finish(1);
}

static void rt_task_interrupted(void *arg)
{
    const uint32_t bit = (uint32_t)(uintptr_t)arg;

    rt_enter(bit);
    rt_probe.stage = 2; /* the host injects at the next instruction boundary */
    /* Execution continues here after the injected interrupt has been taken and
     * serviced: the handler must have returned to this task. */
    if (yan_os_transport_irq_status() != 0) {
        rt_finish(RT_FAIL(RT_FAIL_NOT_SERVICED));
    }
    if (yan_os_transport_h2g_available() != 1) {
        rt_finish(RT_FAIL(RT_FAIL_DATA_LOST));
    }
    if (rt_probe.running_bits != bit) {
        rt_finish(RT_FAIL(RT_FAIL_SWITCHED));
    }
    rt_leave(bit);
    yan_os_task_yield(); /* now the woken waiter may run */
    rt_enter(bit);
    rt_leave(bit);
    for (;;) {
        yan_os_task_yield(); /* the waiter reports the verdict */
    }
}
#endif

/* -------------------------------------------------------------------- main */

#if YAN_RT_SCENARIO == YAN_RT_SCENARIO_PREDICATE_TRUE
int main(void)
{
    rt_configure_platform();
    guest_check(yan_os_task_spawn(rt_task_waiter, (void *)(uintptr_t)1u) ==
                    YAN_OS_TASK_OK, RT_FAIL(RT_FAIL_SPAWN_RESULT));
    guest_check(yan_os_task_spawn(rt_task_trace, (void *)(uintptr_t)2u) ==
                    YAN_OS_TASK_OK, RT_FAIL(RT_FAIL_SPAWN_RESULT));
    yan_os_sched_run();
    rt_finish(RT_FAIL(RT_FAIL_NO_PANIC));
}
#elif YAN_RT_SCENARIO == YAN_RT_SCENARIO_WAITER_EXISTS
int main(void)
{
    rt_configure_platform();
    guest_check(yan_os_task_spawn(rt_task_first_waiter, (void *)(uintptr_t)1u) ==
                    YAN_OS_TASK_OK, RT_FAIL(RT_FAIL_SPAWN_RESULT));
    guest_check(yan_os_task_spawn(rt_task_second_waiter, (void *)(uintptr_t)2u) ==
                    YAN_OS_TASK_OK, RT_FAIL(RT_FAIL_SPAWN_RESULT));
    yan_os_sched_run();
    rt_finish(RT_FAIL(RT_FAIL_NO_PANIC));
}
#elif YAN_RT_SCENARIO == YAN_RT_SCENARIO_PRECONDITION_YIELD
int main(void)
{
    rt_configure_platform();
    rt_interrupts_off();
    yan_os_task_yield();
    rt_finish(RT_FAIL(RT_FAIL_NO_PANIC));
}
#elif YAN_RT_SCENARIO == YAN_RT_SCENARIO_PRECONDITION_WAIT
int main(void)
{
    RtWaitContext state = {0, 0};
    rt_configure_platform();
    rt_interrupts_off();
    yan_os_task_wait(YAN_OS_EVENT_TRANSPORT, rt_predicate, &state);
    rt_finish(RT_FAIL(RT_FAIL_NO_PANIC));
}
#elif YAN_RT_SCENARIO == YAN_RT_SCENARIO_PRECONDITION_EXIT
int main(void)
{
    rt_configure_platform();
    rt_interrupts_off();
    yan_os_task_exit();
    rt_finish(RT_FAIL(RT_FAIL_NO_PANIC));
}
#elif YAN_RT_SCENARIO == YAN_RT_SCENARIO_WAKE
int main(void)
{
    rt_configure_platform();
    guest_check(yan_os_task_spawn(rt_task_waiter, (void *)(uintptr_t)1u) ==
                    YAN_OS_TASK_OK, RT_FAIL(RT_FAIL_SPAWN_RESULT));
    guest_check(yan_os_task_spawn(rt_task_watchdog, (void *)(uintptr_t)2u) ==
                    YAN_OS_TASK_OK, RT_FAIL(RT_FAIL_SPAWN_RESULT));
    yan_os_sched_run();
    rt_finish(RT_FAIL(RT_FAIL_NO_PANIC));
}
#elif YAN_RT_SCENARIO == YAN_RT_SCENARIO_SECOND_WAIT
int main(void)
{
    rt_configure_platform();
    guest_check(yan_os_task_spawn(rt_task_two_waits, (void *)(uintptr_t)1u) ==
                    YAN_OS_TASK_OK, RT_FAIL(RT_FAIL_SPAWN_RESULT));
    yan_os_sched_run();
    rt_finish(RT_FAIL(RT_FAIL_NO_PANIC));
}
#elif YAN_RT_SCENARIO == YAN_RT_SCENARIO_IDLE
int main(void)
{
    rt_configure_platform();
    guest_check(yan_os_task_spawn(rt_task_idle_waiter, (void *)(uintptr_t)1u) ==
                    YAN_OS_TASK_OK, RT_FAIL(RT_FAIL_SPAWN_RESULT));
    guest_check(yan_os_task_spawn(rt_task_leaves, (void *)(uintptr_t)2u) ==
                    YAN_OS_TASK_OK, RT_FAIL(RT_FAIL_SPAWN_RESULT));
    yan_os_sched_run();
    rt_finish(RT_FAIL(RT_FAIL_NO_PANIC));
}
#elif YAN_RT_SCENARIO == YAN_RT_SCENARIO_CONTEXT
int main(void)
{
    rt_configure_platform();
    guest_check(yan_os_task_spawn(rt_task_context, (void *)(uintptr_t)1u) ==
                    YAN_OS_TASK_OK, RT_FAIL(RT_FAIL_SPAWN_RESULT));
    guest_check(yan_os_task_spawn(rt_task_context, (void *)(uintptr_t)2u) ==
                    YAN_OS_TASK_OK, RT_FAIL(RT_FAIL_SPAWN_RESULT));
    yan_os_sched_run();
    rt_finish(RT_FAIL(RT_FAIL_NO_PANIC));
}
#elif YAN_RT_SCENARIO == YAN_RT_SCENARIO_SPAWN
int main(void)
{
    rt_configure_platform();
    for (uint32_t index = 0; index < RT_SPAWN_TASKS; ++index) {
        void (*entry)(void *) =
            index + 1 == RT_SPAWN_TASKS ? rt_task_reports : rt_task_quiet;
        guest_check(yan_os_task_spawn(entry, (void *)(uintptr_t)1u) ==
                        YAN_OS_TASK_OK, RT_FAIL(RT_FAIL_SPAWN_RESULT));
    }
    guest_check(yan_os_task_spawn(rt_task_quiet, NULL) == YAN_OS_TASK_NO_SLOT,
                RT_FAIL(RT_FAIL_SPAWN_RESULT));
    guest_check(yan_os_task_spawn(NULL, NULL) == YAN_OS_TASK_INVALID,
                RT_FAIL(RT_FAIL_SPAWN_RESULT));
    yan_os_sched_run();
    rt_finish(RT_FAIL(RT_FAIL_NO_PANIC));
}
#elif YAN_RT_SCENARIO == YAN_RT_SCENARIO_NO_SWITCH
int main(void)
{
    rt_configure_platform();
    /* The waiter is spawned first so it blocks before the interrupted task
     * starts: the injected interrupt then lands on a task that was already
     * running, which is where a switching handler would show up. */
    guest_check(yan_os_task_spawn(rt_task_waiter, (void *)(uintptr_t)1u) ==
                    YAN_OS_TASK_OK, RT_FAIL(RT_FAIL_SPAWN_RESULT));
    guest_check(yan_os_task_spawn(rt_task_interrupted, (void *)(uintptr_t)2u) ==
                    YAN_OS_TASK_OK, RT_FAIL(RT_FAIL_SPAWN_RESULT));
    yan_os_sched_run();
    rt_finish(RT_FAIL(RT_FAIL_NO_PANIC));
}
#else
#error "unknown YAN_RT_SCENARIO"
#endif
