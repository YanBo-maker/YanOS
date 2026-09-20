# 0015：UART 字符设备

> 状态：**Spec Lock v1**（2026-09-20 项目所有者审核通过）；**v2**（中断线语义改由 [0016](0016-plic-gateway-and-irq-lines.md) 定义）；**v3**（连接状态与 `TX_READY` 严格语义，见下方变更摘要）。**v2 与 v3 已于 2026-09-20 由项目所有者审核通过并锁定。**
> 实现中如需变更接口、数据结构或行为，走 INTENTION / SPEC / IMPLE 审核，不得直接修改本文件。

### v3 变更摘要

1. `STATUS` 新增 `CONNECTED`；原 `RX_AVAILABLE` 更名为 `RX_READY`；`IRQ_STATUS` 的位由 `RX_READY` 更名为 `RX_RX_PENDING` 语义的 `RX_IRQ_PENDING`，避免与 `STATUS.RX_READY` 重名而混淆"有数据可读"与"数据到达时使能了中断"两件事。
2. 明确 `TX_READY` 的严格语义：**"此刻写 `TXDATA` 是否会被设备接受"**，并据此禁止"`TX_READY=1` 但写入被拒"。
3. 无 Host terminal backend 时 `CONNECTED=0`、`TX_READY=0`、`RX_READY=0`，两个方向均以 `YAN_UNAVAILABLE` 拒绝。
4. 新增 Guest 驱动契约：`CONNECTED=0` 时不得无限轮询，必须返回明确的 unavailable 结果，允许 headless 继续运行。
5. `include/yan/status.h` 追加 `YAN_UNAVAILABLE`（追加在枚举末尾，既有取值不变）。

## INTENTION

本规格起草时，Guest 没有任何字符设备：`include/yan/` 与 `src/` 中 `uart`、`console` 零命中。Guest 只能写 `tohost` 让 `yan_run` 停机，既不能输出一个字符，也不能接收输入。控制台、演示与调试都建立在字符 I/O 之上。

本阶段提供一个最小且可以独立测试的字符终端设备：Guest 能输出字节、接收字节，并在收到数据时获得中断。它是 M1 中"Guest 能说话"这条验收的直接载体。

设备可能**根本没有另一端**：`yan_run` 可以用重定向或无人值守的方式启动，此时没有终端 backend。设备必须如实报告这一状态，而不是让 Guest 卡在一个永远不会就绪的就绪位上。

### 与传输通道的分工

[0014](0014-host-transport-channel.md) 的传输通道与 UART 都连着 Host，但语义不同，也不互为前提：

| 设备 | 面向 | 语义 |
| --- | --- | --- |
| UART（本规格） | 人与终端 | 逐个字符，带就绪状态与接收中断，形态对应真实串口 |
| 传输通道（0014） | 程序与程序 | 通用字节流，不解释载荷，供块设备与知识服务 RPC 使用 |

控制台在 M1 走 UART。传输通道在 M1 只做设备与模块测试，从 M2 开始被块设备消费。两者不重叠，也不存在"两个都能对 Host 说话"的歧义。

## SPEC

### 地址与访问宽度

| 项 | 值 |
| --- | --- |
| `YAN_UART_BASE` | `0x10000000` |
| `YAN_UART_SIZE` | `0x1000` |

访问宽度、对齐与未实现偏移的错误语义与 [0012](0012-machine-interrupts.md)、[0014](0014-host-transport-channel.md) 一致：只有 4 字节访问进入设备，其他宽度返回 `YAN_UNMAPPED`，未对齐返回 `YAN_UNALIGNED`。

该地址没有外部约束：已核对 `tests/act4/rvmodel_macros.h`，ACT4 框架不含 UART 地址宏，官方套件不访问此窗口。

### 寄存器

| 偏移 | 名称 | 访问 | 说明 |
| --- | --- | --- | --- |
| 0x00 | `TXDATA` | 只写 | 写发送一个字节，只取低 8 位；读返回 0 |
| 0x04 | `RXDATA` | 只读 | 读取出一个已接收字节（低 8 位）；读走清除 `RX_READY`；无数据时返回 0 且不改变状态；写入被忽略 |
| 0x08 | `STATUS` | 只读 | bit0 `TX_READY`、bit1 `RX_READY`、bit2 `CONNECTED`，其余保留读 0 |
| 0x0c | `CONTROL` | 读写 | bit0 `RX_IRQ_ENABLE`，其余保留读 0；复位 0 |
| 0x10 | `IRQ_STATUS` | 读写 | bit0 `RX_IRQ_PENDING`，写 1 清除；其余保留读 0 |

三个状态位的定义（这是本规格的核心）：

| 位 | 含义 | 何时为 1 |
| --- | --- | --- |
| `CONNECTED` | 是否已连接 Host terminal backend | 已附加一个可用的 backend |
| `TX_READY` | **此刻写 `TXDATA` 是否会被设备接受** | `CONNECTED` 且 backend 自报可接受一个字节 |
| `RX_READY` | 是否有待读取的数据 | `CONNECTED` 且接收缓冲非空 |

**不变式**：`TX_READY = 1` 与"该次 `TXDATA` 写入被接受"必须一致。设备不得在自报 `TX_READY=1` 的同时拒绝写入；也不得在 `TX_READY=0` 时接受并静默丢弃。`STATUS` 的读数与写入路径使用**同一个判定函数**。

### Terminal backend 与连接状态

Host 通过附加一个 backend 把设备的另一端接上：

```c
typedef struct {
    void *context;
    bool (*tx_ready)(void *context);              /* 此刻能否接受一个字节 */
    void (*tx_write)(void *context, uint8_t byte); /* 接受该字节；只在 tx_ready 为真后被调用 */
} YanUartTerminal;

YanStatus yan_uart_set_terminal(YanUart *uart, const YanUartTerminal *terminal);
bool      yan_uart_connected(const YanUart *uart);
bool      yan_uart_tx_ready(const YanUart *uart);
```

- `tx_ready` 是**背压**接口：它让设备在广告 `TX_READY=1` 之前就知道能否接受，从而不必"先答应再拒绝"。`tx_write` 因此被约定为**不得拒绝**——设备只在 `tx_ready` 为真后调用它。
- `yan_uart_set_terminal(uart, NULL)` 断开。传入缺少任一回调的 backend 返回 `YAN_INVALID_ARGUMENT` 且保留原连接。
- **断开时清空接收缓冲与 `IRQ_STATUS`**：终端离开就带走了尚未取走的输入。`CONTROL` 保留。
- **替换（非断开）不清空接收缓冲与 `IRQ_STATUS`**：`yan_uart_set_terminal(uart, &another)` 只是把另一端换人，尚未取走的输入仍然有效，`IRQ_STATUS` 与中断线一并保持；只有断开（`NULL`）或整机复位才清空。区分这两件事，是为了让"终端重连"不悄悄吃掉已经收到的字节。
- `yan_uart_connected` 与 `yan_uart_tx_ready` 是只读查询，供测试与驱动使用；设备内部的 `STATUS` 读数与 `TXDATA` 写入路径都走同一判定。

### 行为

复位后 `CONNECTED = 0`、`TX_READY = 0`、`RX_READY = 0`、`CONTROL = 0`、`IRQ_STATUS = 0`，接收缓冲为空。**未连接 backend 时这三个状态位恒为 0**，即使内部缓冲里还留有数据。

**发送**。Guest 读 `STATUS.TX_READY`，为 1 时写 `TXDATA`，设备调用 `tx_write` 把低 8 位交给 backend。下列两种情形写 `TXDATA` 都返回 `YAN_UNAVAILABLE`，且不发送、不缓冲：

- 未连接 backend（`CONNECTED = 0`）；
- 已连接但 backend 此刻不能接受（`TX_READY = 0`）。

两者都用 `YAN_UNAVAILABLE` 而不是 `YAN_INVALID_STATE`：调用方没有写错，只是此刻不可用。驱动靠 `CONNECTED` 区分这两种情形。

**读—写之间状态可能变化**。`TX_READY` 是读取 `STATUS` 那一刻的采样，而写入路径在写的那一刻重新判定。本实现是单线程模拟器，Host 只能在指令边界之间改变 backend 的 `tx_ready`，因此 Guest 的一次"读状态 → 写数据"序列**可能**观察到从 1 变 0。这时的写入按上述规则返回 `YAN_UNAVAILABLE`——**报告，而不是静默丢弃**。这也是"TX 写入被拒"这条测试用例要覆盖的路径。

**接收**。Host 通过 `yan_uart_push_rx(uart, byte)` 注入一个字节并置 `RX_READY`；未连接 backend 时该调用返回 `YAN_UNAVAILABLE`。若此时 `CONTROL.RX_IRQ_ENABLE = 1`，同时置 `IRQ_STATUS.RX_IRQ_PENDING`，且设备的**中断线**转为 asserted；平台把该线映射到 PLIC 源，由 PLIC 决定是否产生 `mip.MEIP`（[0016](0016-plic-gateway-and-irq-lines.md)）。Guest 读 `RXDATA` 取出字节并清除 `RX_READY`；读空不改变任何状态。

接收缓冲在 M1 是**单字节**：`RX_READY = 1` 时再调用 `push_rx` 返回 `YAN_INVALID_STATE`，不覆盖旧值、不排队。多字节 FIFO 留给真实需求（例如一次粘贴多个字符）出现时决定。

**中断**。`IRQ_STATUS` 写 1 清除。中断线是设备状态的纯函数：`RX_READY = 1` 且 `RX_IRQ_ENABLE = 1` 时为 asserted，否则 deasserted；由于未连接时 `RX_READY = 0`，断开后中断线自动为低。设备**不返回 mip 位、不接触 PLIC**；源的 pending、claim 与 complete 行为由 [0016](0016-plic-gateway-and-irq-lines.md) 定义。

### Guest 驱动契约

这一段约束的是 Guest 侧的驱动与控制台，不是设备本身；设备只负责如实报告状态位。

1. 驱动在轮询 `TX_READY` 之前**必须先读 `STATUS.CONNECTED`**。
2. `CONNECTED = 0` 时驱动**不得进入等待或无限轮询**，必须立即返回一个明确的"不可用"结果（`YAN_UNAVAILABLE`，等价于 POSIX 的 `ENODEV` 一类语义）。
3. 控制台上层收到该结果后**允许继续以 headless 方式运行**：输出被丢弃、程序照常推进，不因缺少终端而阻塞或崩溃。
4. `CONNECTED = 1` 而 `TX_READY = 0` 时，驱动可以在协作式让出点重试，但**重试必须有上限**；上限到达后同样返回 `YAN_UNAVAILABLE`。

这条契约的验收属于[控制台与 `os/` 层规格](0017-console-and-os-layout.md)；本规格只要求设备提供足以实现它的状态位。

### 与 PLIC 的源编号约定

源号是平台接线而不是设备属性，因此常量位于 `include/yan/machine.h`，分配表由 [0016](0016-plic-gateway-and-irq-lines.md) 定义：

| 源 | 用途 | 平台常量 |
| --- | --- | --- |
| 0 | 保留，不可用 | — |
| 1 | Host 传输通道（[0014](0014-host-transport-channel.md)） | `YAN_MACHINE_PLIC_SOURCE_TRANSPORT` |
| 2 | UART 接收（本规格） | `YAN_MACHINE_PLIC_SOURCE_UART` |
| 3 起 | 未分配 | — |

### 明确不在范围内

不做波特率与时钟分频；不做硬件流控（RTS / CTS）；不做 Modem 状态线；不做 16C550 寄存器兼容；不做发送 FIFO 与发送中断；不做 DMA；不做奇偶校验与帧错误检测；不做多终端与终端热切换（backend 只有一个槽位）。这些机制服务于真实串口线上的电气与协议问题，而本设备的另一端是 Host 进程。

### 规范依据

- 寄存器语义的形态参考 NS16550A 类 UART，但**不复用**其寄存器偏移与 FIFO 行为。参考：[TI PC16550D 数据手册](https://www.ti.com/product/PC16550D)。
- "就绪位必须与写入结果一致"与"设备缺失返回 ENODEV 类结果而非阻塞"是终端设备驱动的通行约定，本规格将其写成可测的不变式。
- 中断线语义、PLIC 源编号分配与网关卡口行为见 [0016：PLIC 网关与设备中断线](0016-plic-gateway-and-irq-lines.md)。
- MMIO 访问宽度与错误码沿用 [0003：Machine、Bus 与 RAM](0003-machine-bus-ram.md)；`YAN_UNAVAILABLE` 为本次新增，追加在 `YanStatus` 枚举末尾，既有取值不变。

## IMPLE PLAN

1. 本规格 v3 交项目所有者审核并锁定。
2. 更新 `include/yan/status.h`（追加 `YAN_UNAVAILABLE`）与 `include/yan/uart.h`（`CONNECTED` / `RX_READY` 位、`RX_IRQ_PENDING` 位、`YanUartTerminal` 与三个 API）。这两处是**冻结接口**，由架构侧修改，实现方不得改动。
3. 先改 `tests/test_uart.c`：按下方 VERIFY 新增"未连接 / 已连接 / TX 写入被拒"三组用例，并调整受位改名影响的既有断言，确认在实现前失败。
4. 实现 `src/uart.c`：两个方向共用同一个连接与就绪判定；写路径返回 `YAN_UNAVAILABLE`。
5. 运行 Debug ASan/UBSan 与 Release 全量 CTest，并重做变异检查。
6. Host 侧在 `tools/` 中提供一个 terminal backend（`tx_ready` 反映标准输出**此刻**能否接受一个字节、`tx_write` 写标准输出），标准输入接到 `push_rx`。`tx_ready` 不是恒真的常量：标准输出写失败后它转为假，设备随之报告 `TX_READY = 0`，这正是上面驱动契约里"`CONNECTED = 1` 而 `TX_READY = 0`"那条的来源。

控制台驱动（行编辑、回显、退格、headless 行为）属于 YanOS 层，不属于本设备的规格，见[控制台与 `os/` 层规格](0017-console-and-os-layout.md)。它的外部行为依赖本规格定义的 `STATUS` / `CONNECTED` / `RXDATA` / 中断语义与上面的驱动契约。

## VERIFY

- 复位值与位定义；保留位读 0；对只读寄存器的写入被忽略。
- **未连接**：`CONNECTED = TX_READY = RX_READY = 0`；`push_rx` 返回 `YAN_UNAVAILABLE`；读 `RXDATA` 返回 0 且不改变状态；写 `TXDATA` 返回 `YAN_UNAVAILABLE` 且没有任何字节被交付；中断线恒为 deasserted。
- **已连接且就绪**：`CONNECTED = 1`、`TX_READY = 1`；写 `TXDATA` 返回 `YAN_OK`，backend 收到**恰好一个**、值为低 8 位的字节。
- **已连接但不可接受**：backend 的 `tx_ready` 为假时 `TX_READY = 0`，写 `TXDATA` 返回 `YAN_UNAVAILABLE` 且 backend **没有**收到字节。
- **TX 写入被拒（读—写之间变化）**：读到 `TX_READY = 1` 之后把 backend 置为不可接受，再写 `TXDATA` 必须返回 `YAN_UNAVAILABLE`，且不得出现"已接受却报错"或"静默丢弃"。
- **就绪位与写入结果一致**：在 backend 就绪的任意时刻，`TX_READY` 为 1 与写入被接受必须同时成立（同一判定函数）。
- **断开**：`set_terminal(NULL)` 后立即 `CONNECTED = 0`、`TX_READY = 0`、`RX_READY = 0`，接收缓冲与 `IRQ_STATUS` 被清空，`CONTROL` 保留。
- **非法 backend**：缺少任一回调时返回 `YAN_INVALID_ARGUMENT` 且原连接保持不变。
- **接收**：`push_rx` 置 `RX_READY`；读 `RXDATA` 清除该位并返回正确字节；读空不改变状态；单字节缓冲满时再次 `push_rx` 返回 `YAN_INVALID_STATE` 且不覆盖旧值。
- **中断线真值表**：仅当 `RX_READY && RX_IRQ_ENABLE` 时为 asserted；读走 `RXDATA`、清 `CONTROL` 或断开 backend 后为 deasserted。经平台映射到 PLIC 源后，由 PLIC 决定 `mip.MEIP`。
- **`IRQ_STATUS` 写 1 清除**；位改名后语义不变。
- 生命周期：整机复位清空运行状态并**保留** terminal backend；两台 Machine 的 UART 互不影响。
- Guest 集成：最小 Guest 程序经 `os/` 的控制台封装输出一行字符，Host 侧捕获到逐字节相同的输出。
- 变异检查：至少注入 6 个缺陷（`TX_READY` 恒 0 或恒 1、断开时不清接收缓冲、`CONNECTED=0` 时仍接受写入、写路径不回 `YAN_UNAVAILABLE`、读 `RXDATA` 不清 `RX_READY`、接收中断使能判断取反），确认测试逐一报错。
- 命令：Debug 与 Release 下的 `cmake --build` 与 `ctest --output-on-failure`。

实现、验证与审查状态见 [STATUS](../STATUS.md)。
