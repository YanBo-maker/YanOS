#include "task.h"

#include <stddef.h>

#include "console.h"
#include "platform.h"

/* YanOS cooperative runtime: docs/specs/0019-cooperative-runtime.md.
 *
 * Eight static task slots, no preemption, and a wait primitive whose only exit
 * is "the event happened". Three rules carry the design and each of them is
 * visible in the code below:
 *
 *   1. A task stops only where its own code says so, and the reason it stopped
 *      is a state it wrote itself: wait marks the caller BLOCKED, yield marks
 *      it RUNNABLE, exit frees its slot.
 *   2. An interrupt does one thing: BLOCKED -> RUNNABLE, plus emptying the
 *      waiter slot it woke. It never schedules and never switches context,
 *      because a switch inside the handler would leave a trap frame on a task
 *      that is no longer the running one.
 *   3. Interrupt masking is save/restore. wait saves mstatus.MIE, holds it
 *      clear across the predicate check and the waiter registration, and
 *      restores *that saved value* - never an unconditional enable, which would
 *      silently open a caller's own critical section. The precondition that
 *      wait / yield / exit are only called with interrupts enabled is what
 *      makes the two ends of that pair the same value, and it is checked, not
 *      assumed.
 *
 * The lost wakeup argument in the specification is what the critical section
 * buys: with interrupts off across the predicate check and the registration
 * there is no window in which the event can happen without a waiter being
 * registered for it, so a woken task can never be missed.
 */

/* ---------------------------------------------------------------- context */

typedef struct {
    uint32_t ra;
    uint32_t sp;
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
} YanOsTaskContext;

/* Defined in os/task_switch.S. It is the seam between this file and the
 * assembly body, not part of the runtime's API: the context type above is
 * private to the runtime. */
void yan_os_task_switch(YanOsTaskContext *from, YanOsTaskContext *to);

/* The switch in os/task_switch.S writes this struct word by word. Every offset
 * is asserted, so a reordering here cannot silently corrupt a task. */
_Static_assert(offsetof(YanOsTaskContext, ra) == YAN_OS_TASK_CTX_RA,
               "task_switch.S: ra offset");
_Static_assert(offsetof(YanOsTaskContext, sp) == YAN_OS_TASK_CTX_SP,
               "task_switch.S: sp offset");
_Static_assert(offsetof(YanOsTaskContext, s0) == YAN_OS_TASK_CTX_S0,
               "task_switch.S: s0 offset");
_Static_assert(offsetof(YanOsTaskContext, s5) == YAN_OS_TASK_CTX_S5,
               "task_switch.S: s5 offset");
_Static_assert(offsetof(YanOsTaskContext, s11) == YAN_OS_TASK_CTX_S11,
               "task_switch.S: s11 offset");
_Static_assert(sizeof(YanOsTaskContext) == YAN_OS_TASK_CTX_BYTES,
               "task_switch.S: context size");

/* An event is a PLIC source in this first version (SPEC). The platform owns the
 * source numbers, so they are held against each other rather than restated. */
_Static_assert((uint32_t)YAN_OS_EVENT_TRANSPORT == YAN_OS_PLIC_SOURCE_TRANSPORT,
               "event 1 must be the transport source");
_Static_assert((uint32_t)YAN_OS_EVENT_UART == YAN_OS_PLIC_SOURCE_UART,
               "event 2 must be the UART source");

typedef enum {
    YAN_OS_TASK_UNUSED = 0, /* free slot                                  */
    YAN_OS_TASK_RUNNABLE,   /* can be picked, not running                 */
    YAN_OS_TASK_RUNNING,    /* the task that holds the CPU, at most one   */
    YAN_OS_TASK_BLOCKED     /* waiting for an event; only an ISR unblocks */
} YanOsTaskState;

/* mstatus.MIE, and the mie bit that turns the PLIC's MEIP into an interrupt.
 * os/platform.h describes the PLIC side of the route, not the CPU side. */
#define YAN_OS_MSTATUS_MIE UINT32_C(0x8)
#define YAN_OS_MIE_MEIE UINT32_C(0x800)

/* mcause: the interrupt bit and the external-interrupt code. */
#define YAN_OS_MCAUSE_INTERRUPT UINT32_C(0x80000000)
#define YAN_OS_MCAUSE_MEIP UINT32_C(11)

typedef struct {
    /* First member on purpose: os/task.h fixes these offsets for the switch. */
    YanOsTaskContext context;
    void (*entry)(void *);
    void *arg;
    YanOsTaskState state;
    /* Every task carries its own 4 KiB. The alignment is what keeps sp
     * 16-byte aligned at the task's first instruction, which the psABI
     * requires at a function entry. */
    _Alignas(16) uint32_t stack[YAN_OS_TASK_STACK_SIZE / 4];
} YanOsTask;

static YanOsTask tasks[YAN_OS_TASK_MAX];

/* The scheduler's own context. It is not a task: it has no slot, no stack of
 * its own beyond the boot stack, and it owns no state a task could wait on. */
static YanOsTaskContext sched_context;

/* The task holding the CPU, or NULL while the scheduler context runs. */
static YanOsTask *current;

/* One waiter per event: this version is deliberately single-waiter (SPEC), so
 * an event with a waiter is a programming error, not a queue to extend. */
static YanOsTask *waiters[YAN_OS_EVENT_MAX];

/* Round-robin cursor: the slot of the task picked last. It starts one slot
 * before the first one so that the first scheduling round begins at slot 0 and
 * tasks start in the order they were spawned. */
static uint32_t sched_cursor = YAN_OS_TASK_MAX - 1;

/* ---------------------------------------------------------------- report */

/* Both symbols are optional at link time. tohost is placed by the image's
 * linker script; without it the hang loop below still stands, only the report
 * is missing. The console is the same shape: the panic prints a line when a
 * driver and a terminal are there, and skips it otherwise (0017: the console
 * never blocks and reports YAN_OS_UNAVAILABLE instead of waiting). An image
 * that does not link os/console.c therefore builds and reports its failure code
 * through tohost; only the diagnostic line is missing.
 *
 * The weak attribute has to be applied *before* the first use - that is why
 * these redeclarations sit here, below console.h and above yan_os_panic(). A
 * weak declaration that comes after the symbol has been used is a compile
 * error in GCC ("declared weak after being used", measured on this file by
 * moving these three lines below yan_os_panic), so the constraint is enforced
 * rather than merely documented. */
extern volatile uint32_t tohost __attribute__((weak));
extern YanOsResult yan_os_console_puts(const char *text) __attribute__((weak));
extern YanOsResult yan_os_console_putc(char c) __attribute__((weak));

static void yan_os_irq_disable(void)
{
    __asm__ volatile ("csrci mstatus, 8" ::: "memory");
}

/* The last thing the runtime does: one line of diagnosis when it can be
 * printed, the failure code, and a loop that neither yields nor returns. */
static void yan_os_panic(uint32_t code)
{
    /* The report must not be interrupted: an interrupt taken here would run a
     * handler on a runtime that has already given up. */
    yan_os_irq_disable();

    if (yan_os_console_puts != NULL && yan_os_console_putc != NULL) {
        (void)yan_os_console_puts("yan_os: panic 0x");
        for (int shift = 28; shift >= 0; shift -= 4) {
            static const char digits[] = "0123456789abcdef";
            (void)yan_os_console_putc(digits[(code >> shift) & 0xfu]);
        }
        (void)yan_os_console_puts("\r\n");
    }
    /* A weak undefined symbol has the address zero, so this is the "no tohost
     * in this image" case and not a store to address zero. */
    if (&tohost != NULL) {
        tohost = code;
    }
    for (;;) {
    }
}

/* ------------------------------------------------------- interrupt state */

static uint32_t yan_os_irq_mie(void)
{
    uint32_t mstatus = 0;
    __asm__ volatile ("csrr %0, mstatus" : "=r"(mstatus));
    return mstatus & YAN_OS_MSTATUS_MIE;
}

/* Reads mstatus.MIE and clears it in one instruction, so the value that is
 * restored later is exactly the value that was there. */
static uint32_t yan_os_irq_save(void)
{
    uint32_t previous = 0;
    __asm__ volatile ("csrrci %0, mstatus, 8" : "=r"(previous));
    return previous & YAN_OS_MSTATUS_MIE;
}

/* Restores the *saved* value. An unconditional enable here would turn "the
 * caller had interrupts off" into "interrupts are on from now on" and cut a
 * caller's critical section in half; an unconditional disable would leave the
 * system unable to receive the event it is waiting for. */
static void yan_os_irq_restore(uint32_t saved)
{
    if ((saved & YAN_OS_MSTATUS_MIE) != 0) {
        __asm__ volatile ("csrsi mstatus, 8" ::: "memory");
    } else {
        __asm__ volatile ("csrci mstatus, 8" ::: "memory");
    }
}

/* SPEC precondition: wait / yield / exit are only called with interrupts
 * enabled. With it, save and restore act on the same value, and every task
 * that registers a waiter does so in a state where an interrupt can reach it
 * afterwards. */
static void yan_os_require_interrupts_enabled(void)
{
    if (yan_os_irq_mie() == 0) {
        yan_os_panic(YAN_OS_PANIC_INTERRUPTS_DISABLED);
    }
}

/* The runtime owns the CPU interrupt gate. mstatus.MIE is not part of any
 * task's context: there is one CPU gate, not one per task, and a task is only
 * ever entered with interrupts enabled. */
static void yan_os_enable_interrupts(void)
{
    __asm__ volatile ("csrs mie, %0" :: "r"(YAN_OS_MIE_MEIE) : "memory");
    __asm__ volatile ("csrsi mstatus, 8" ::: "memory");
}

/* ------------------------------------------------------------- switching */

/* The next RUNNABLE task in slot order after the last one picked, or NULL when
 * nothing can run. No critical section is needed: a task's state is written by
 * that task alone, and the interrupt handler only ever moves a *registered*
 * waiter from BLOCKED to RUNNABLE - it cannot touch the RUNNABLE task this scan
 * is about to hand the CPU to. A mark that lands mid-scan is simply picked up
 * on the next round, and one that lands after this scan is handled by the
 * scheduler's loop. */
static YanOsTask *yan_os_sched_pick(void)
{
    for (uint32_t offset = 1; offset <= YAN_OS_TASK_MAX; ++offset) {
        const uint32_t index = (sched_cursor + offset) % YAN_OS_TASK_MAX;
        if (tasks[index].state == YAN_OS_TASK_RUNNABLE) {
            sched_cursor = index;
            return &tasks[index];
        }
    }
    return NULL;
}

/* Hands the CPU over. Called by a task that has already written its own state
 * (BLOCKED, RUNNABLE or UNUSED), and returns when that task runs again. */
static void yan_os_sched_reschedule(void)
{
    YanOsTask *next = yan_os_sched_pick();
    if (next == current) {
        /* Nobody else can run. The caller keeps the CPU, so its state goes back
         * to RUNNING: returning to it while it is marked RUNNABLE would mean
         * "executing while runnable". This branch is also the window SPEC
         * describes after step 6: an interrupt may already have woken the
         * caller, in which case switch_to(itself) is an immediate return. */
        current->state = YAN_OS_TASK_RUNNING;
        return;
    }
    YanOsTask *self = current;
    if (next == NULL) {
        /* No RUNNABLE task at all: park on the scheduler context, which spins
         * with interrupts enabled until an interrupt marks a waiter RUNNABLE
         * and it picks that task. */
        current = NULL;
        yan_os_task_switch(&self->context, &sched_context);
        return;
    }
    next->state = YAN_OS_TASK_RUNNING;
    /* Whoever switches publishes who runs next, so the resumed task always
     * finds `current` pointing at itself. */
    current = next;
    yan_os_task_switch(&self->context, &next->context);
}

/* Entered by the first switch into a task: spawn planted ra = this function and
 * sp = the top of the task's own stack, and the switch's `ret` lands here. */
static void yan_os_task_trampoline(void)
{
    YanOsTask *task = current;
    task->entry(task->arg);
    /* An entry that returns ends the task (SPEC). */
    yan_os_task_exit();
}

/* SPEC: a slot or stack that is illegal right after spawn is a programming
 * error. The checks are cheap and they fail at the one moment the state can be
 * introduced - inside spawn itself. */
static int yan_os_spawn_is_legal(const YanOsTask *task)
{
    const uintptr_t bottom = (uintptr_t)&task->stack[0];
    const uintptr_t top = (uintptr_t)&task->stack[YAN_OS_TASK_STACK_SIZE / 4];
    const uintptr_t sp = (uintptr_t)task->context.sp;
    return task->state == YAN_OS_TASK_RUNNABLE && task->entry != NULL &&
           task->context.ra == (uint32_t)(uintptr_t)&yan_os_task_trampoline &&
           sp == top && sp % 16u == 0 && sp > bottom &&
           sp - bottom == (uintptr_t)YAN_OS_TASK_STACK_SIZE;
}

/* ------------------------------------------------------------------- API */

YanOsTaskResult yan_os_task_spawn(void (*entry)(void *), void *arg)
{
    if (entry == NULL) {
        return YAN_OS_TASK_INVALID;
    }
    /* The table is shared with the interrupt handler, but the handler only ever
     * reads the waiter slots, and a slot that is being filled is UNUSED until
     * the last store below. There is no preemption, so no other task can see a
     * half-built slot either. */
    for (uint32_t index = 0; index < YAN_OS_TASK_MAX; ++index) {
        YanOsTask *task = &tasks[index];
        if (task->state != YAN_OS_TASK_UNUSED) {
            continue;
        }
        task->entry = entry;
        task->arg = arg;
        task->context.ra = (uint32_t)(uintptr_t)&yan_os_task_trampoline;
        task->context.sp =
            (uint32_t)(uintptr_t)&task->stack[YAN_OS_TASK_STACK_SIZE / 4];
        /* A defined start for the first switch. The callee-saved registers are
         * the new task's from its own prologue on, but a slot that was
         * occupied before must not leak the old occupant's values into it. */
        task->context.s0 = 0;
        task->context.s1 = 0;
        task->context.s2 = 0;
        task->context.s3 = 0;
        task->context.s4 = 0;
        task->context.s5 = 0;
        task->context.s6 = 0;
        task->context.s7 = 0;
        task->context.s8 = 0;
        task->context.s9 = 0;
        task->context.s10 = 0;
        task->context.s11 = 0;
        task->state = YAN_OS_TASK_RUNNABLE;
        if (!yan_os_spawn_is_legal(task)) {
            task->state = YAN_OS_TASK_UNUSED;
            yan_os_panic(YAN_OS_PANIC_TASK_TABLE);
        }
        return YAN_OS_TASK_OK;
    }
    return YAN_OS_TASK_NO_SLOT;
}

void yan_os_task_yield(void)
{
    yan_os_require_interrupts_enabled();
    if (current == NULL) {
        yan_os_panic(YAN_OS_PANIC_NO_TASK);
    }
    /* RUNNABLE and then hand over: the scheduler may pick this task again
     * immediately, and then reschedule() puts it back to RUNNING without a
     * switch at all. */
    current->state = YAN_OS_TASK_RUNNABLE;
    yan_os_sched_reschedule();
}

void yan_os_task_exit(void)
{
    yan_os_require_interrupts_enabled();
    if (current == NULL) {
        yan_os_panic(YAN_OS_PANIC_NO_TASK);
    }
    YanOsTask *self = current;
    self->state = YAN_OS_TASK_UNUSED;
    /* Straight back to the scheduler rather than through reschedule(): the slot
     * is free from here on, and the context saved by this switch is never
     * restored again, because nothing picks an UNUSED slot. */
    current = NULL;
    yan_os_task_switch(&self->context, &sched_context);
    yan_os_panic(YAN_OS_PANIC_EXIT_RETURNED);
}

void yan_os_sched_run(void)
{
    /* The runtime takes the CPU interrupt gate here: mie.MEIE is what turns the
     * PLIC's MEIP into an interrupt, and mstatus.MIE is global, not per task.
     * The caller above only prepares the device and the PLIC, which SPEC lists
     * as its own responsibility. */
    yan_os_enable_interrupts();

    for (;;) {
        YanOsTask *next = yan_os_sched_pick();
        if (next == NULL) {
            /* Every task is BLOCKED or gone. mie stays enabled and nothing is
             * handed over: this loop is where the CPU waits for the interrupt
             * that marks a waiter RUNNABLE. YanCPU has no WFI, so it is a busy
             * loop - but it only runs when there is no runnable task, never
             * while a task is waiting for I/O. */
            continue;
        }
        next->state = YAN_OS_TASK_RUNNING;
        current = next;
        yan_os_task_switch(&sched_context, &next->context);
        /* Back here once the task blocks, yields or exits. It has already
         * written its own state, so no task is RUNNING in this context. */
    }
}

void yan_os_task_wait(YanOsEvent event, int (*predicate)(void *), void *context)
{
    /* 1. Save and disable. The precondition is checked on the value that was
     *    saved, so "interrupts were off" panics before anything is touched. */
    const uint32_t previous = yan_os_irq_save();
    if (previous == 0) {
        yan_os_panic(YAN_OS_PANIC_INTERRUPTS_DISABLED);
    }
    if (current == NULL) {
        yan_os_panic(YAN_OS_PANIC_NO_TASK);
    }
    if (predicate == NULL) {
        yan_os_panic(YAN_OS_PANIC_PREDICATE);
    }
    /* Event 0 is the reserved PLIC source: a claim never returns it, so a
     * waiter on it could only block forever. That is a programming error, not a
     * wait. */
    if ((uint32_t)event == 0 || (uint32_t)event >= (uint32_t)YAN_OS_EVENT_MAX) {
        yan_os_panic(YAN_OS_PANIC_EVENT_RANGE);
    }

    /* 2. Already happened: restore and return. No waiter is registered and the
     *    caller keeps running, which is why this path must not touch the task
     *    table at all. */
    if (predicate(context)) {
        yan_os_irq_restore(previous);
        return;
    }

    /* 3. One waiter per event: a second one is a programming error, and the
     *    panic is the report. Overwriting instead would lose the first task
     *    forever. */
    if (waiters[event] != NULL) {
        yan_os_panic(YAN_OS_PANIC_WAITER_EXISTS);
    }

    /* 4.-5. Register and block, still with interrupts off: the two stores are
     *    one indivisible change of state as far as the handler is concerned, so
     *    there is no window in which the event can happen with the waiter
     *    registered but the task not yet marked BLOCKED. */
    waiters[event] = current;
    current->state = YAN_OS_TASK_BLOCKED;

    /* 6. Restore the saved value. After this the interrupt that makes the event
     *    happen can be taken, up to and including inside the switch below - and
     *    if it is, it marks this very task RUNNABLE, which the scheduler
     *    handles by simply picking it. */
    yan_os_irq_restore(previous);

    /* 7. Hand the CPU over. */
    yan_os_sched_reschedule();

    /* 8. Woken: the caller re-checks the state it cares about, because the
     *    event may have arrived in the window above without this task ever
     *    stopping. */
}

/* ------------------------------------------------------------- trap handler */

/* Moves the event's waiter back to RUNNABLE and empties the slot. Called from
 * the trap handler only, where the CPU has cleared mstatus.MIE, so the update
 * cannot interleave with a task's registration. */
static void yan_os_wake(YanOsEvent event)
{
    YanOsTask *task = waiters[event];
    if (task == NULL) {
        /* The event has no waiter: a device can interrupt for reasons nobody is
         * waiting for, and that is not an error. */
        return;
    }
    /* Clear the slot with the mark. Leaving it behind would make the next wait
     * on this event see a stale waiter and panic. */
    waiters[event] = NULL;
    if (task->state == YAN_OS_TASK_BLOCKED) {
        /* The only state change an interrupt is allowed to make. */
        task->state = YAN_OS_TASK_RUNNABLE;
    }
}

/* Called by os/trap_entry.S. Returns the pc to resume at: the interrupted
 * instruction for an interrupt, because nothing faulted. */
uint32_t yan_os_trap_handler(uint32_t mcause, uint32_t mepc, uint32_t mtval)
{
    (void)mtval;
    if ((mcause & YAN_OS_MCAUSE_INTERRUPT) == 0) {
        /* An exception is never part of this runtime: it means the Guest did
         * something illegal instead of waiting for an event. */
        yan_os_panic(YAN_OS_PANIC_UNEXPECTED_TRAP);
    }
    if ((mcause & UINT32_C(0xff)) != YAN_OS_MCAUSE_MEIP) {
        /* The runtime routes external interrupts only. MTIP and MSIP have no
         * waiter to mark, and returning to the interrupted instruction would
         * enter this handler again immediately. */
        yan_os_panic(YAN_OS_PANIC_UNEXPECTED_TRAP);
    }

    /* 1. Claim: a handshake, not a peek. Zero means this context had nothing
     *    pending - the ordinary outcome of a spurious external interrupt. */
    const uint32_t source = yan_os_plic_claim();
    if (source == 0) {
        return mepc;
    }

    if (source == YAN_OS_PLIC_SOURCE_TRANSPORT) {
        /* 2. Service and ack the device first: write one to clear
         *    IRQ_STATUS.H2G_DATA, which withdraws the level. */
        if ((yan_os_transport_irq_status() & YAN_OS_TRANSPORT_IRQ_H2G_DATA) != 0) {
            yan_os_transport_irq_ack();
        }
        /* 3. Mark the waiter runnable. Nothing else: no scheduling, no context
         *    switch, no state change for any other task. */
        yan_os_wake(YAN_OS_EVENT_TRANSPORT);
    } else {
        /* No service routine for this device. Completing a source whose line is
         * still asserted pends it again immediately, which is an interrupt
         * storm, so the runtime reports the missing service instead. */
        yan_os_panic(YAN_OS_PANIC_UNSERVICED_SOURCE);
    }

    /* 4. Complete. By now the device condition is gone, so the completion does
     *    not merely re-pend the source it is releasing. */
    yan_os_plic_complete(source);
    return mepc;
}
