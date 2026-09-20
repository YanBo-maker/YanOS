# 0012：机器模式中断

## INTENTION

上一阶段只实现了 M-mode 同步异常：`yan_cpu_step` 在取指前不观察任何设备状态，`mie` / `mip` 固定读零，平台没有计时器和外部中断控制器。这样 Guest 无法用中断驱动 I/O，也无法把 sleep 一类的等待写成一次 WFI 或一次定时唤醒。

本阶段的目标是让**一条中断链真正闭合**：设备产生 pending → CPU 在下一条指令边界采样 → 满足 `mstatus.MIE` 与 `mie` 时进入 M-mode 中断陷阱 → 处理程序用 `mret` 返回被中断的指令。范围限定在单 hart、M-mode、直接入口、固定优先级，不涉及 S/U 模式、委托、向量模式与抢占。

## SPEC

### 中断状态

新增两个 CSR，位定义与 RISC-V 特权规范一致（位号即中断原因码）：

| 位 | mie / mip | 原因码 | 来源 |
| --- | --- | --- | --- |
| 3 | MSIP | 3 | CLINT `msip`（机器软中断） |
| 7 | MTIP | 7 | CLINT `mtime >= mtimecmp`（机器定时器中断） |
| 11 | MEIP | 11 | PLIC 有源满足 pending、enabled 且 priority > threshold |

| CSR | 行为 |
| --- | --- |
| 0x304 mie | 仅保留上述三位，写入按掩码截断；复位 0 |
| 0x344 mip | 只读，由设备线采样得到；写入被忽略（硬件驱动位） |

`mip` 不是独立寄存器：CPU 在指令边界通过 Bus 查询设备，把结果锁存进 `csr.mip`。Guest 用 `csrr` 读到的是该边界采样值；Host 用 `yan_cpu_poll_interrupts` 手动采样，用于状态检查。

进入中断的条件是 `mstatus.MIE == 1` 且 `mip & mie & {MSIP,MTIP,MEIP} != 0`。二者缺一时中断保持 pending，不改变 PC、寄存器和 CSR。

### 中断进入

中断在**取指之前**判定，因此在同一条边界上优先于该指令可能产生的同步异常。触发时：

| 字段 | 值 |
| --- | --- |
| mcause | `0x80000000 \| 原因码`，最高位置 1 |
| mepc | 被中断指令的地址（低两位固定零）；该指令尚未执行 |
| mtval | 0 |
| mstatus | MPP=3，MPIE=MIE，MIE=0 |
| pc | `mtvec & 0xfffffffc`（Direct 入口） |

`yan_cpu_step` / `yan_machine_step` 返回 `YAN_TRAP`，本步不执行处理程序首条指令。同时多个源 pending 时按 **MEIP > MTIP > MSIP** 选择原因码；本阶段不支持中断嵌套，进入后 MIE=0，处理程序未返回前不再接受中断。`mret` 沿用同步异常的语义：PC=mepc，MIE=MPIE，MPIE=1，MPP=3。

### CLINT

单 hart，标准基址 `0x02000000`，大小 `0x10000`。只实现三个寄存器，全部按 32 位字访问；64 位寄存器拆成低半字与高半字两个地址。

| 偏移 | 寄存器 | 读写 |
| --- | --- | --- |
| 0x0000 | msip | 读写，仅 bit0 保留（写其他位被丢弃） |
| 0x4000 / 0x4004 | mtimecmp 低 / 高 | 读写，复位 `UINT64_MAX` |
| 0xbff8 / 0xbffc | mtime 低 / 高 | 读写，复位 0 |

`mtime` 只通过离散 tick 前进：`yan_machine_tick(machine, ticks)` 与 `yan_machine_step`（每条指令前进 1）。实现不读取宿主墙上时钟，因此同一输入序列总能得到同一时间线。允许写入 `mtime`，用于 Host 构造确定的起始状态；计时器的权威推进路径仍是 tick。mtimecmp 复位为 `UINT64_MAX`，使复位后 MTIP 保持为低，避免平台刚启动就产生定时器中断。

### PLIC

单 M-mode context，标准基址 `0x0c000000`，大小 `0x400000`。支持 32 个源编号，其中 **source 0 保留**（不可 raise、enable 位与 priority 恒为 0），实际可用 1～31。

| 偏移 | 寄存器 | 读写 |
| --- | --- | --- |
| 0x0000 + 4*i | priority[i] | 读写，source 0 忽略写入；复位 0 |
| 0x1000 | pending | 只读；由 raise 置位、由 claim 清除 |
| 0x2000 | enable（M-mode） | 读写，bit0 固定 0 |
| 0x200000 | threshold（M-mode） | 读写 |
| 0x200004 | claim / complete | 读=claim，写=complete |

仲裁规则：在所有 `pending & enabled` 且 `priority > threshold` 的源里选优先级最高者，同优先级取编号最小者；没有满足条件的源时 claim 返回 0。claim 读会清除该源的 pending 并把它记为 in-service；complete 写清除 in-service。只要存在满足三条件的源，`mip.MEIP` 就为高。

简化（与完整 PLIC 的差异，必须在验证中声明）：没有实现 gateway 的电平/边沿锁存，in-service 期间的再次 raise 仍会立刻置 pending 并再次拉起 MEIP；complete 不重新采样源电平。Host 的 `yan_plic_raise(plic, source)` 是唯一的外部源接口。

### Bus 与 Machine

Bus 在 RAM 译码之前按地址区间把 32 位访问分派给 CLINT 与 PLIC，设备的错误码沿用 Bus 既有语义：

- 设备只回答 4 字节访问；其他宽度的访问不进入设备，按原路径返回 `YAN_UNMAPPED`。
- 设备区内未实现的偏移返回 `YAN_UNMAPPED`，未对齐的字访问返回 `YAN_UNALIGNED`。
- `bus.clint == NULL` 或 `bus.plic == NULL` 时该区间不被设备占用，访问按原有 RAM 路径返回错误，因此未连接设备的 Bus（如差分执行器与 `yan_run` 使用的 Bus）上 CLINT / PLIC 地址仍是 `YAN_UNMAPPED`。
- RAM 读写、`YAN_INVALID_WIDTH`、`YAN_INVALID_STATE` 等既有错误语义不变。

`YanMachine` 持有 `YanClint` 与 `YanPlic`。`yan_machine_init` 复位设备并建立 `bus.clint` / `bus.plic` 指向；整机复位 `yan_machine_reset` 同时复位 CPU 与设备；`yan_machine_destroy` 断开指针并清零设备。`check_machine` 校验连接关系，未连接设备的 Machine 返回 `YAN_INVALID_STATE`。

### 明确不在范围内

单 hart；仅 M-mode；无 S/U 模式、无 `medeleg`/`mideleg` 委托、无 `mtvec` 向量模式、无中断嵌套与优先级抢占；PLIC 只有一个 M-mode context，不支持 S-mode context 与多 hart；CLINT 只有 hart 0 的 msip / mtimecmp。本规格不声明完整的 RISC-V 特权架构符合性。

### 规范依据

- 机器模式 CSR、中断原因码、进入与 `mret` 语义：[特权架构 20240411，Machine-Level ISA 1.13](https://docs.riscv.org/reference/isa/v20240411/_attachments/riscv-privileged.pdf)。
- CLINT 地址布局与 msip / mtimecmp / mtime 语义：[SiFive FU540-C000 手册](https://www.sifive.com/documentation) 的 Core Local Interruptor 章节（本实现只取单 hart 子集）。
- PLIC 寄存器布局与仲裁规则：[RISC-V PLIC 规格](https://github.com/riscv/riscv-plic-spec)。

## IMPLE PLAN

1. 写本规格，修正 0009 中 `mie` / `mip` 固定读零的旧约定。
2. 先写 `test_clint` / `test_plic` / `test_interrupt` 并注册到 CTest，确认在实现前失败。
3. 实现 `interrupt.h` / `interrupt.c` 的 CLINT 与 PLIC。
4. 实现 CPU 的 `mie` / `mip`、设备采样与中断进入，并接入 Bus 分派与 Machine 生命周期。
5. 运行 Debug ASan/UBSan 与 Release 全量 CTest，更新 STATUS 与 README。

## VERIFY

- CLINT：复位值、msip 位宽、mtimecmp 高低半字、mtime 高低半字、离散 tick、mtimecmp 边界（相等即触发、`UINT64_MAX` 不触发）、64 位加法回绕、区间边界与错误码。
- PLIC：priority / pending / enable / threshold / claim / complete 的 MMIO 读写、source 0 保留、pending 只读、claim 仲裁（最高优先级、同优先级最小号）、claim 后 MEIP 撤销、complete 清除 in-service。
- CPU：`mip` 采样三种源、`mie` 掩码、`mstatus.MIE` 屏蔽、原因码优先级、mcause 最高位、mepc 指向未执行指令、mtval=0、入口状态、MRET 恢复 MIE。
- Machine：设备连接与清理、整机复位、两台机器设备互不影响、每步 tick 一条指令。
- 命令：`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DYAN_ENABLE_SANITIZERS=ON` 与 Release 构建下的 `ctest --output-on-failure`。

实现、验证与审查状态见 [STATUS](../STATUS.md)。
