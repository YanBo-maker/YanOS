# 0014：Host 传输通道

> 状态：**Spec Lock v1**；**v2**（设备中断线与 PLIC 源映射改由 [0016：PLIC 网关与设备中断线](0016-plic-gateway-and-irq-lines.md) 定义）。**v1 与 v2 均已由项目所有者审核通过并锁定。**
> 实现中如需变更接口、数据结构或行为，走 INTENTION / SPEC / IMPLE 审核，不得直接修改本文件。

## INTENTION

平台目前只有两个设备：CLINT 与 PLIC。两者承载的都是中断与计时语义，没有任何数据通路。`tohost` 是 `yan_run` 在 RAM 里轮询一个字的停机约定，不是设备，不能传数据。

结果是 Guest 无法与 Host 交换任何字节。M1 的控制台、M2 的块设备、M3 的知识服务 RPC 都需要这样一条通路；如果每个阶段各定义一套 MMIO 协议，平台会长出三套 ABI、三份测试与三处 Host 适配，而定位文档要求 YanOS 只暴露简单的消息接口。

本阶段建立**一条通用的双向字节通道**：设备只负责搬运字节，不理解载荷含义。范围限定在单 hart、M-mode、单生产者单消费者字节环、门铃加一条 PLIC 源。

## SPEC

### 地址与访问宽度

| 项 | 值 |
| --- | --- |
| `YAN_TRANSPORT_BASE` | `0x10001000` |
| `YAN_TRANSPORT_SIZE` | `0x1000` |

地址选择避开 CLINT `0x02000000`（大小 `0x10000`）、PLIC `0x0c000000`（大小 `0x400000`）与 RAM `0x80000000`。已核对 `tests/act4/rvmodel_macros.h`，ACT4 框架不含任何 IO 设备地址宏，官方套件不访问该窗口。

访问规则沿用既有设备语义：只有 4 字节访问进入设备，其他宽度按原路径返回 `YAN_UNMAPPED`；未对齐的字访问返回 `YAN_UNALIGNED`；区内未实现的偏移返回 `YAN_UNMAPPED`。

### 寄存器

| 偏移 | 名称 | 访问 | 说明 |
| --- | --- | --- | --- |
| 0x000 | `MAGIC` | 只读 | 恒为 `0x59414e31`（"YAN1"），供 Guest 自检 |
| 0x004 | `VERSION` | 只读 | 高 16 位主版本、低 16 位次版本；本规格为 `0x00010000` |
| 0x008 | `STATUS` | 只读 | bit0 `HOST_READY`、bit1 `G2H_FULL`、bit2 `H2G_EMPTY`、bit3 `OVERFLOW_DETECTED`，其余保留读 0 |
| 0x00c | `RING_BASE` | 只读 | 两个环在 RAM 中的基址，由 Host 配置 |
| 0x010 | `RING_SIZE` | 只读 | 单个环的字节数，由 Host 配置；必须是 2 的幂且不小于 64，未配置时为 0 |
| 0x014 | `G2H_HEAD` | 读写 | Guest 的生产位置 |
| 0x018 | `G2H_TAIL` | 只读 | Host 维护的消费位置 |
| 0x01c | `H2G_HEAD` | 只读 | Host 维护的生产位置 |
| 0x020 | `H2G_TAIL` | 读写 | Guest 的消费位置 |
| 0x024 | `DOORBELL` | 只写 | Guest 写任意值触发一次 Host 通知；读返回 0 |
| 0x028 | `IRQ_STATUS` | 读写 | bit0 `H2G_DATA`，写 1 清除 |
| 0x02c | `IRQ_ENABLE` | 读写 | bit0 `H2G_DATA` 中断使能；复位 0 |

`G2H_TAIL` 与 `H2G_HEAD` 由 Host 驱动：Guest 对它们的写入被忽略，读返回 Host 维护的值。这与 `mip` 的既有处理一致——由硬件驱动的位不接受软件写入。`RING_BASE`、`RING_SIZE`、`VERSION`、`MAGIC` 同样忽略写入。

### 环的布局

两个环共享 `RING_BASE`，连续排列，各占 `RING_SIZE` 字节：

| 环 | 区间 | 生产者 | 消费者 |
| --- | --- | --- | --- |
| Guest → Host | `[RING_BASE, RING_BASE + RING_SIZE)` | Guest（写 `G2H_HEAD`） | Host |
| Host → Guest | `[RING_BASE + RING_SIZE, RING_BASE + 2 * RING_SIZE)` | Host | Guest（写 `H2G_TAIL`） |

环位于 Guest 的 RAM 中：Guest 用普通 `load` / `store` 访问环内字节，Host 直接读写同一段 RAM。`HEAD` 与 `TAIL` 是 `[0, RING_SIZE)` 内的**字节偏移**，不是地址。

空与满的判定：

- 空：`HEAD == TAIL`
- 满：`(HEAD + 1) % RING_SIZE == TAIL`

因此单个环的可用容量是 `RING_SIZE - 1` 字节，留一格用于区分空与满。

### 流控契约

设备**不检测** Guest 对环的越界写：环是普通 RAM，Guest 用 `store` 写它时设备无法拦截。因此流控是调用方的责任，并以如下方式约束：

- Guest 在写入前必须读 `G2H_TAIL` 计算剩余空间，空间不足时不得推进 `G2H_HEAD`。这一检查由 `os/` 层的通道封装函数统一实现，使用方不得直接操作环指针。
- Host 侧在**记账 API**（`yan_transport_host_consume` 与 `yan_transport_host_publish`）处校验：请求搬运的字节数超过环内当时可读或可写的字节数时，置 `STATUS.OVERFLOW_DETECTED` 并保留第一个错误。
- **判定时机说明**：`DOORBELL` 只做通知，不在那一刻判定越界。原因是 doorbell 无法区分"诚实写满"与"越界后回绕"——两种情形下 Guest 写入的指针读数可以完全相同，只有在 Host 记账时才知道本次搬运量是否超出了环能容纳的范围。
- Host **只检测不修正**。越界写已经破坏了环内容，静默修正会掩盖错误，也会让"数据正确"变成一个无法验证的声明。

因此"环满"是 Guest 侧可以检测并处理的条件，而不是设备返回的错误码。

### 内存序

Guest 写环内数据之后、推进 `G2H_HEAD` 或写 `DOORBELL` 之前，必须执行一次 `FENCE`。

当前实现是单 hart 顺序执行的 C 模拟器，Host 与 Guest 不会真正并发，所以在今天的实现里缺少 `FENCE` 不会导致失败。把它写进契约是为了让 Guest 代码在真实硬件上依然成立。**这条要求属于接口契约，不属于当前实现的必要条件，验证中不得声称"缺少 `FENCE` 会导致失败"。**

### 中断

| 方向 | 机制 |
| --- | --- |
| Host → Guest | 设备**中断线**。Host 写入环并推进 `H2G_HEAD` 后置 `IRQ_STATUS.H2G_DATA`；`IRQ_ENABLE.bit0 = 1` 时该线 asserted。设备只报告线电平，平台把线映射到 PLIC 源，由 PLIC 决定是否产生 `mip.MEIP` —— 见 [0016](0016-plic-gateway-and-irq-lines.md) |
| Guest → Host | 不占用中断源。写 `DOORBELL` 触发 Host 侧注册的通知回调 |

Guest 的处理程序读走环内数据并推进 `H2G_TAIL` 后，写 1 清除 `IRQ_STATUS`。中断线采用电平语义：只要还有未读数据且中断使能，线保持 asserted。源被 claim 之后、complete 之前不得重新 pending；complete 后若线仍 asserted 才允许重新 pending（[0016](0016-plic-gateway-and-irq-lines.md)）。

### 生命周期与连接

- 窗口只在 Machine 连接了 transport 时映射。`bus.transport == NULL` 时该区间不被设备占用，访问沿既有 RAM 路径返回错误，因此 `yan_difftest` 这类不连接设备的执行器上该地址仍是 `YAN_UNMAPPED`。
- Host 通过 `yan_transport_configure(transport, ring_base, ring_size, ram_base, ram_size)` 配置环。设备不持有 RAM 指针（通道不搬运字节），所以 RAM 窗口由调用方在配置时传入供校验：`ring_base` 必须落在 `[ram_base, ram_base + ram_size)` 内，`ring_size` 必须是 2 的幂且不小于 `YAN_TRANSPORT_MIN_RING_SIZE`，并且两个环都不越过 RAM 末尾；不满足时返回 `YAN_INVALID_ARGUMENT`，配置保持不变。
- `HOST_READY` 完全由配置推导：`RING_SIZE != 0` 且 Host 已注册通知回调时为 1，否则为 0。它不额外锁存，也不单独清除。`HOST_READY == 0` 时 Guest 不得读写环数据。
- 整机复位清空 `HEAD` / `TAIL` / `IRQ` / `STATUS`，并把 `HEAD` 与 `TAIL` 置零；`RING_BASE`、`RING_SIZE` 与回调注册保留，因为它们是 Host 的配置而不是运行状态。由此复位后 `HOST_READY` 仍为 1，而两个环按"`HEAD == TAIL` 即为空"的定义处于空状态。整机复位会清零 RAM，所以复位前环内的字节按定义失效——这一点由置零的指针表达，不需要额外的清除步骤。
- `yan_machine_destroy` 断开指针并清零设备。

### 分层约束

通道不解释任何字节：它不知道载荷是控制台字符、块请求还是知识服务 RPC。`src/transport.c` 中不得出现上层词汇（`note`、`markdown`、`rpc`、`block` 等）；Host 侧适配不得解析知识格式。上层协议各自在自己的规格里定义载荷格式。

### 明确不在范围内

不做 descriptor chain；不做 scatter-gather；不做多队列；不做 feature negotiation；不做 DMA 与描述符表；不做零拷贝保证；不做信用或窗口类的流控协商；不做多 hart 与 S-mode context；不定义任何上层协议。之所以逐条列出，是因为这些正是"通用通道"最常见的膨胀方向，而 M1 只需要一条能搬运字节的通路。

### 规范依据

- 环的空满判定与单生产者单消费者语义在本规格中自行定义，没有可援引的外部规范。
- 门铃加共享环的形态参考 [virtio 1.2](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html) 的 split virtqueue 思路，但不实现其描述符链、特性协商与通知抑制。
- PLIC source 0 保留、源编号分配与网关卡口语义见 [0016：PLIC 网关与设备中断线](0016-plic-gateway-and-irq-lines.md) 与 [RISC-V PLIC 规格](https://github.com/riscv/riscv-plic-spec)。
- MMIO 访问宽度与错误码沿用 [0003：Machine、Bus 与 RAM](0003-machine-bus-ram.md)。

## IMPLE PLAN

1. 写本规格与 [0015：UART 字符设备](0015-uart-device.md)，交项目所有者审核。
2. 建立 `os/` 顶层目录与构建接入；在共享文件（`src/bus.c`、`src/machine.c`、`include/yan/machine.h`、`CMakeLists.txt`）中一次性落好接入骨架，使 UART 与 transport 的实现可以并行而不修改同一文件。
3. 先写 `tests/test_transport.c` 并注册到 CTest，确认在实现前失败。
4. 实现 `include/yan/transport.h` 与 `src/transport.c`，以及 Host 侧的通知回调接口；`tools/` 侧的适配在控制台打通后接入。
5. 运行 Debug ASan/UBSan 与 Release 全量 CTest，更新 STATUS 与 README。

## VERIFY

- 窗口：未连接时不占用区间；非 4 字节宽度与未实现偏移返回 `YAN_UNMAPPED`；未对齐返回 `YAN_UNALIGNED`。
- 寄存器：`MAGIC` 与 `VERSION` 恒定；对只读寄存器的写入被忽略且不改变后续读数。
- 配置校验：`ring_size` 非 2 的幂、小于 64、环越出 RAM 末尾时返回 `YAN_INVALID_ARGUMENT` 且配置不变。
- 环语义：空与满的判定、`RING_SIZE - 1` 容量上界、偏移回绕、按字节搬运的逐字节正确性。
- 越界检测：Host 记账 API 请求的搬运量超过环内当时可读或可写的字节数时，`STATUS.OVERFLOW_DETECTED` 置位并保留首个错误；`DOORBELL` 本身不判定越界。
- 中断：`IRQ_ENABLE` 关闭时线 deasserted；打开且置 `IRQ_STATUS` 后线 asserted，经平台映射到 PLIC 源并产生 `mip.MEIP`；清除 `IRQ_STATUS` 后线撤销。
- 生命周期：整机复位清空运行状态并保留 Host 配置；两台 Machine 的通道互不影响。
- 分层：`src/transport.c` 不出现上层词汇。
- 变异检查：至少注入 5 个缺陷（空满判定退化为同一条件、容量上界少算一字节、只读寄存器写入未被忽略、doorbell 不触发通知、越界未置位），确认测试逐一报错。
- 命令：Debug 与 Release 下的 `cmake --build` 与 `ctest --output-on-failure`。

实现、验证与审查状态见 [STATUS](../STATUS.md)。
