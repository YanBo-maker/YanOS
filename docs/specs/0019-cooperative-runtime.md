# 0019：协作式运行时

> 状态：**Spec Lock**（2026-09-20 项目所有者审核通过）。实现中如需变更接口、数据结构或行为，走 INTENTION / SPEC / IMPLE 审核，不得直接修改本文件。

## INTENTION

[0018](0018-block-protocol.md) 的块请求天生是异步的：发出请求之后，响应要等 Host 处理完才会出现。如果没有让出机制，驱动只剩一条路——原地忙等，CPU 在等 I/O 的时候什么也做不了。

本阶段给 `os/` 加一个**协作式、无抢占**的运行时：任务只在明确的让出点上切换。被选中的理由不是"简单"，而是它把"任务为什么停下"变成代码里看得见的一行——等事件、让出、退出。

有一条约束要先写死：**禁止用"yield + 忙等轮询"冒充 I/O wait**。如果 `yan_os_task_wait` 的实现是"让出一下、回来再问一次 predicate"，那它就没有把 CPU 让给真正能推进这件事的东西，只是把忙等拆成了碎片。等待只有一种实现：登记 waiter、阻塞自己、由中断唤醒。

## SPEC

### 任务模型

| 项 | 值 | 说明 |
| --- | --- | --- |
| 任务上限 | 8 | 静态数组，无动态分配 |
| 每任务栈 | 4 KiB | 静态分配，随任务槽位一起在 `.bss` |
| 上下文 | `ra`、`sp`、`s0`–`s11` | **只保存 callee-saved 寄存器** |

只保存 callee-saved 是 ABI 级决定，不是省事：切换由 C 函数调用触发（`switch_to` 是一个普通函数调用），caller-saved 寄存器在调用点已经由调用约定处理，保存它们等于把同一件事做两遍。切换本体写在汇编里（`os/task_switch.S`），遵循选定的 RISC-V 调用约定。

任务入口签名 `void (*entry)(void *)`：以一个参数启动，返回即视为 `yan_os_task_exit`。

### 任务状态

| 状态 | 含义 |
| --- | --- |
| `UNUSED` | 槽位空闲，可被 `spawn` 占用 |
| `RUNNABLE` | 可被调度，尚未运行或已被唤醒 |
| `RUNNING` | 当前正在执行的任务，任意时刻至多一个 |
| `BLOCKED` | 在等一个事件，只能由中断把它标回 `RUNNABLE` |

**中断只做一件事：把 `BLOCKED` 改成 `RUNNABLE`。** 中断里不允许出现第三种状态迁移。

### API

```c
#define YAN_OS_TASK_MAX 8
#define YAN_OS_TASK_STACK_SIZE 4096

typedef enum {
    YAN_OS_TASK_OK = 0,
    YAN_OS_TASK_NO_SLOT,   /* 8 个槽位都在用 */
    YAN_OS_TASK_INVALID    /* 入口为空，或参数非法 */
} YanOsTaskResult;

YanOsTaskResult yan_os_task_spawn(void (*entry)(void *), void *arg);
void yan_os_task_yield(void);
void yan_os_task_exit(void);          /* 入口函数返回也走这里 */
void yan_os_sched_run(void);          /* 不返回：空闲时也留在调度循环里 */
void yan_os_task_wait(YanOsEvent event, int (*predicate)(void *), void *context);
```

`YanOsTaskResult` 与 [0017](0017-console-and-os-layout.md) 的 `YanOsResult` **不合并**：后者描述"设备此刻可不可用"，前者描述"任务槽位是否拿到"，让运行时依赖控制台头是不必要的耦合。

### `yan_os_task_wait` 的执行序列

按顺序写死，实现不得重排：

1. **保存并关闭中断**：记下当前 `mstatus.MIE`，清零它（见"中断屏蔽用 save/restore"）。
2. `if (predicate(context))` → **恢复中断状态并直接返回**。完成事件已经发生，不阻塞、不登记 waiter。
3. 断言该 `event` **没有既有 waiter**；有则 panic（第一版单 waiter）。
4. `waiter[event] = 当前任务`。
5. 当前任务 `state = BLOCKED`。
6. **恢复中断状态**（恢复步骤 1 保存的值，不是无条件置 1）。
7. `switch_to(下一个 RUNNABLE 任务)`。
8. 被唤醒后从 `switch_to` 返回，`yan_os_task_wait` 返回；此后调用方自己重新检查它关心的状态。

### 中断屏蔽用 save/restore 语义

关中断保存原 `mstatus.MIE`，临界区结束时**恢复保存值**。**不得无条件把 MIE 置 1**：那样会把"调用者本来就关着中断"变成"从此开着"，把调用方的临界区悄悄切开。

**前置条件：`wait` / `yield` / `exit` 必须在中断使能状态下调用。** 违反是编程错误，运行时 panic。有了这条，save/restore 的两端就是同一个值。

**为什么不用"每任务保存 MIE"来解决。** 一个自然的替代方案是让每个任务记住自己的 MIE，切换时连带恢复。它并不能消除危险：如果所有 `BLOCKED` 任务都处在 MIE=0 的状态，就没有任何任务能让 ISR 运行，事件永远不会到达，系统照样死锁——只是死锁的位置从 wait 里面挪到了调度器里。真正消除它的是上面那条前置条件：任何进入 wait 的任务都必然是在中断使能状态下登记的，因此 ISR 总有运行的机会。

### ISR 顺序

中断服务程序按固定顺序做四件事：

1. `claim`：读 PLIC 的 claim 寄存器取得源号。
2. **service / ack 设备**：按该设备的语义处理 IRQ 条件。对传输通道而言就是写 1 清 `IRQ_STATUS.H2G_DATA`，使这条线的电平撤销。
3. `waiter[event]` 从 `BLOCKED` 改成 `RUNNABLE`，并清空该 waiter 槽。
4. `complete`：把源号写回 claim 寄存器。

**顺序的理由**：PLIC 的源是电平语义（[0016](0016-plic-gateway-and-irq-lines.md) 的网关表），`complete` 时若线仍 asserted 就会立刻重新 pending。所以必须**先让设备条件消失、再 complete**，否则 ISR 一返回就再次进入，表现为中断风暴。

**ISR 只 mark，不调度、不切换上下文。** 在中断上下文里切换任务，意味着中断帧还压在某个任务栈上、`mepc` / `mstatus` 也还挂在被中断的任务上；第一版不承担这个复杂度。ISR 返回后由调度器决定谁运行。

### lost wakeup 不可能的论证

设任务 T 调用 `wait(event, pred)`：

- 步骤 1–5 全程关中断，**任何 ISR 都无法插入**。因此不存在"predicate 检查通过、waiter 尚未登记"的窗口——那正是 lost wakeup 唯一可能的入口。
- 于是只有两种情形：
  1. 步骤 2 看到 predicate 已成立 → 不阻塞，直接返回；
  2. 步骤 2 看到未成立 → waiter 已完整登记为 T，且 T 已进入 `BLOCKED`。此后任何让 predicate 成立的 ISR 都会把 T 标回 `RUNNABLE`，T 不可能既没被唤醒又不在运行。
- 步骤 6 恢复中断之后、步骤 7 切换之前，ISR 可能立刻发生。此时它把**当前任务自己**标成 `RUNNABLE`——这不会破坏任何不变量：调度器即使当场又选中 T，`switch_to(T)` 就是一次立即返回的调用，T 从步骤 8 继续，语义仍然正确。

这条推理依赖"predicate 是纯查询"：如果 predicate 有副作用，或者它在两次调用之间改变了设备状态，上面第 2 种情形的前提就不成立。

### 前置条件与契约

- `wait` 要求**对应设备 → PLIC → MEIP 的 IRQ route 已经配置可用**：设备的 `IRQ_ENABLE`、PLIC 的 `enable` / `priority` / `threshold` 都由调用方（或更上层的初始化）负责。`wait` **不配置**这些，也**不检查**它们——它只等事件。
- **`predicate` 在关中断状态下被调用**，因此必须：短、无阻塞、只读设备或内存状态，并且**不得调用 `wait` / `yield` / `exit`**。
- `YanOsEvent` 与 PLIC source 一一对应，这是**第一版限制、不是永久 ABI**。将来一个 IRQ 可以映射多个 completion / event（例如一次中断意味着"三个等待者中有两个可以继续"），届时 `event` 不再等于源号，`YanOsEvent` 的值域会与 PLIC source 解耦。
- **全部任务都 `BLOCKED` 时**，调度器进入空闲循环：`mie` 保持使能、不做任何让出，等中断把某个任务标回 `RUNNABLE`。YanCPU 尚未实现 `WFI`，所以这确实是忙等——它只发生在没有任何可运行任务的时候，而不是在等待 I/O 的任务里。

### panic

下列情形是编程错误，运行时 panic：`wait` 时该事件已有 waiter；`wait` / `yield` / `exit` 在关中断状态下被调用；`spawn` 后栈或任务表状态非法。

panic 的后果：先用控制台尽力打印一行诊断（`CONNECTED = 0` 时跳过、不视为失败），然后把失败码写 `tohost`（沿用 [0013](0013-guest-trap-environment.md) 的约定：非 1 即失败，失败码最高位置 1，避免任何编号被读成通过），最后进入不再让出、不再返回的停机循环。`tohost` 是链接期符号，由镜像的链接脚本提供；`os/` 以弱声明引用，没有该符号时停机循环仍然成立，只是没有诊断。

### 与 0017 的关系

运行时是 `os/` 层的第二个成员，分区与依赖方向沿用 [0017](0017-console-and-os-layout.md)：`os/` 不引用 `tests/`，`tests/` 可以引用 `os/`。控制台的无阻塞契约（0017）与运行时不冲突：`yan_os_console_*` 在无终端时立即返回，因此它可以被任务调用而不会把整个运行时拖住。

运行时需要 PLIC 的 `claim` / `complete` / `enable` / `threshold` / `priority` 访问器，而当前 `os/platform.h` **只有 UART 与源号常量**。这些访问器要按 0017 的分区补进平台视图并纳入防漂移比对，是本规格落地的前置改动。

### 已知债务：树里会有两份陷阱入口

本规格新增 `os/trap_entry.S`，而 M1 已经有 `tests/guest/mtrap_entry.S`（[0013](0013-guest-trap-environment.md) 的最小陷阱环境：入口 + Direct 向量 + 固定 20 字帧 + 统一调度）。M2a 之后，树里**同时存在两份陷阱入口**。这不是冲突，但按 0017 的分区原则，陷阱入口只应该有一份，因此记为债务：

| 入口 | 服务对象 | 差异 |
| --- | --- | --- |
| `tests/guest/mtrap_entry.S` + `mtrap.c` | **验证程序**（`trap_env.c` 等自检） | 统一调度到 C 处理程序；已由 0013 独立验证，守着 20 字帧与槽位互斥 |
| `os/trap_entry.S`（本规格） | **运行时** | ISR 内按 claim → service/ack → 唤醒 waiter → complete 四步走，且只 mark、不调度 |

**M2a 不合并它们**：M1 那份已经过独立验证，并且是现有验证程序的运行基础；运行时的 ISR 语义与验证环境的"统一调度"目标不同，此刻合并等于同时改两件事，会把回归风险引进已经稳定的那一层。

**明确的待办**：终局是 `tests/guest/mtrap_entry.S` / `mtrap.c` 迁进 `os/`，验证程序改为引用它，树里只留一份陷阱入口。触发条件是本规格的运行时稳定并通过独立验证之后；在那之前，这份重复是**记录在案的债，不是默许的分叉**——两处入口的任何行为差异都必须回到本规格或 0013 里对齐。

### 明确不在范围内

不做抢占；不做优先级与优先级继承；不做多 waiter（一个事件一个等待者）；不做超时与定时等待；不做信号量、互斥锁与条件变量；不做动态栈（栈大小固定，且第一版不做溢出检测）；不做任务销毁与槽位回收；不做 `WFI` 或其它低功耗等待；不做 SMP。等待只有"事件到了"这一个出口。

### 规范依据

- callee-saved 集合与栈约定：选定的 RISC-V 调用约定（`ra` / `sp` / `s0`–`s11` 由被调用方保存）。
- 中断进入 / 返回与 `mstatus.MIE` / `MPIE` 语义：[0012：机器模式中断](0012-machine-interrupts.md)。
- PLIC 的 claim / complete、电平网关与源号分配：[0016：PLIC 网关与设备中断线](0016-plic-gateway-and-irq-lines.md)。
- 陷阱入口形态（Direct、固定帧、保存调用者保存寄存器与被中断的 `sp`）沿用 [0013](0013-guest-trap-environment.md) 已验证的做法。
- `os/` 分区与依赖方向：[0017：控制台与 `os/` 层](0017-console-and-os-layout.md)。
- 协作式调度与 wait/predicate 的形态参考成熟内核的等待队列做法，但第一版按上面写死的单 waiter 序列实现，不引入队列与超时。

## IMPLE PLAN

1. 本规格交项目所有者审核并锁定。
2. 扩展 `os/platform.h`：PLIC 的 claim / complete / enable / threshold / priority 访问器；`tests/test_boot.c` 增加防漂移比对。
3. 落地 `os/task.h` / `os/task.c`（任务表、调度器、wait）与 `os/task_switch.S`（callee-saved 切换）、`os/trap_entry.S`（Direct 向量 + 固定帧 + 调 C 层 ISR）。
4. Host 侧提供能在精确时点注入中断的测试驱动（现有 `guest_console` / `run_terminal` 的 scripted 后端是同类做法，可复用其形态），否则"在 predicate 与阻塞之间注入中断"这类对抗用例写不出来。
5. 先写用例确认失败，再实现；随后跑 Debug ASan/UBSan 与 Release 全量 CTest 与合并前检查清单。

## VERIFY

- **predicate 已成立时不阻塞**：登记前条件已满足，`wait` 立即返回，且**没有**发生上下文切换（用一个"切换就会留下的痕迹"来断言，例如任务局部计数器）。
- **waiter 已存在时 panic**：同一事件第二次 `wait` 触发 panic 路径（可观测的失败码），而不是挂起或覆盖 waiter。
- **ISR 顺序**：service / ack 发生在 `complete` 之前——用"complete 后源是否立刻重新 pending"来断言；顺序颠倒会表现为中断风暴或第二次进入。
- **对抗用例：在 predicate 与阻塞之间注入中断**（lost wakeup 的直接探测）：驱动在 `wait` 进入临界区之后、任务切换到下一个之前把设备条件置为成立并注入中断；断言任务被唤醒且 `wait` 返回。与此配对的反例是"把 predicate 检查挪到临界区之外"的变异体——它必须被这条用例杀掉。
- **save/restore**：在 `MIE = 0` 的调用者上下文里，`yield` / `wait` / `exit` 触发 panic（前置条件）；正常路径下 `wait` 返回后 `mstatus.MIE` 与进入前一致。
- **空闲路径**：全部任务 `BLOCKED` 时不 panic、不退出调度循环；中断到达后继续运行。
- **栈与上下文**：每个任务在自己的栈上运行，切换后 callee-saved 寄存器逐字段不变（包括 `sp` 指向自己的栈）。
- **变异检查（C 组）**：在 `os/task.c` 上至少注入下列 6 个缺陷，确认测试逐一报错、无存活者：
  1. 把 predicate 检查挪到临界区之外；
  2. ISR 里改调调度器（在中断上下文切换任务）；
  3. ISR 把 `complete` 放到 service / ack 之前；
  4. `switch_to` 之后不恢复中断状态——具体是**无条件清 0**，把保存的中断状态丢掉。"无条件置 1"不在清单里：它与"恢复保存值"在实现上等价、不可杀（证明见 [STATUS](../STATUS.md)），构造能区分二者的测试必须先违反本规格的前置条件。
  5. waiter 登记在 `state = BLOCKED` 之后（登记与阻塞之间留出窗口）；
  6. 唤醒时不清空 waiter 槽（第二次 `wait` 因此看到陈旧 waiter）。
- 命令：Debug ASan/UBSan 与 Release 下的 `cmake --build` 与 `ctest --output-on-failure`。

实现、验证与审查状态见 [STATUS](../STATUS.md)。
