# 项目状态

当前阶段：机器模式中断。模块测试、逐指令差分测试、官方 `riscv-tests` 套件与 Sail 签名比对都已接入并在本机通过；ACT4 官方框架尚未接入。M-mode 的 CLINT、PLIC 与中断进入 / 返回已实现并通过单元测试，中断仍没有外部参考模型可比较。Load / Store 阶段已通过用户审查并合入主分支。

## 已完成

- RAM 生命周期、小端读写、宽度与范围检查。
- Bus 的单 RAM 映射、对齐检查及错误信息。
- Machine 初始化、整机复位和内存镜像装载。
- CPU 的 32 个通用寄存器、x0 语义、PC 和独立复位。
- CPU 经 Bus 读取 32 位机器码；取指保留 PC、寄存器和内存。
- 21 条 RV32I 整数计算指令的单步执行，支持清单见 [整数计算规格](specs/0006-integer-alu.md)。
- 六条条件分支与 JAL、JALR，支持目标对齐检查和返回地址写回，见 [控制转移规格](specs/0007-control-flow.md)。
- LB、LBU、LH、LHU、LW、SB、SH、SW，经 Bus 访问 RAM，见 [Load / Store 规格](specs/0008-load-store.md)。
- 符号扩展、32 位回绕、移位量屏蔽及编码拒绝后的状态保持。
- 同步异常进入、mepc / mcause / mtval、MIE / MPIE 状态保存与 MRET 返回。
- 六种 CSR 指令与机器模式 CSR 集合；ECALL、EBREAK，以及 FENCE 与 FENCE.I（单 hart、每次取指重读 RAM，两者都只推进 PC）。
- 机器模式中断：`mie` / `mip` 的 MSIP、MTIP、MEIP 三位，全局 `mstatus.MIE`，中断进入（mcause 最高位置 1、mepc 指向未执行指令、mtval 为 0）与 MRET 返回，原因码优先级 MEIP > MTIP > MSIP，见 [机器模式中断规格](specs/0012-machine-interrupts.md)。
- 单 hart CLINT：标准基址 `0x02000000`，msip、mtimecmp、mtime 的 32 位半字读写；mtime 只由 Host 的离散 tick 推进，不读宿主墙上时钟。
- 单 context M-mode PLIC：标准基址 `0x0c000000`，priority、pending、enable、threshold、claim / complete，source 0 保留，只有 pending、enabled 且 priority 大于 threshold 的源拉高 MEIP；Host 通过 `yan_plic_raise` 驱动外部源。
- RV32M 的 MUL、MULH、MULHSU、MULHU、DIV、DIVU、REM、REMU，包含除零和有符号溢出规则。
- 面向外部验证模型的只读 CPU 架构状态快照接口。
- RV32 ELF32 镜像加载与符号表解析；`tohost` 与签名区都由符号决定，不依赖固定地址。
- 只读的差分比较模块，输出出错指令、首个不同寄存器与两侧完整寄存器表。
- `yan_run` Guest 执行器：最大步数、`tohost` 退出、签名区导出、JSONL 架构状态记录，以及区分「通过 / Guest 报错 / 未终止 / Host 错误 / 已导出签名」的退出码。
- `yan_difftest` 差分执行器：加载外部参考模型，逐条比较架构状态，结束时比较整段 RAM。
- `yan_gen` 随机 RV32IM 指令流生成器（给定种子确定输出），以及 5 个手写 Guest 用例。
- C17 / CMake 构建、Unity 单元测试、CTest 及 GitHub Actions。

详细分层、覆盖边界与退出码见 [CPU 验证规格](specs/0011-cpu-validation.md)。

## 验证

GCC 11.4、CMake 3.22.1 下的 Debug 与 Release 构建均通过 **18 组 CTest**：RAM、Bus、Machine、CPU、Fetch、Step、ALU、Control、Memory、CSR、Trap、M、Snapshot、Image、DiffTest、Interrupt、CLINT、PLIC，共 **129 个 Unity 测试用例**。

立即数测试覆盖全部 4096 种编码；移位覆盖 0～31 及寄存器移位量高位屏蔽。十条寄存器运算各覆盖 32768 种 rd / rs1 / rs2 组合。固定向量和边界矩阵检查符号比较、算术回绕、逻辑运算与 AUIPC 当前 PC 语义。

控制转移测试覆盖 B 型全部 4096 种偏移、J 型各偏移位与边界、JALR 全部 4096 种立即数及目标低两位。检查条件真值、寄存器字段、目标对齐、地址回绕、返回地址和延迟到下一次取指的映射错误。

访存测试覆盖八条指令的全部 4096 种偏移、寄存器字段组合、符号边界、小端读写与截断、地址和 PC 回绕、RAM 首尾及错误状态保持。取指失败时无数据访问，加载到 x0 仍检查错误；Store 可覆盖已经取出的当前指令。

异常测试覆盖各原因码、mtval、入口状态、Host 错误隔离、嵌套异常、无效入口、六种 CSR 指令的寄存器组合和读写抑制、MRET 恢复及 Machine 复位。Guest 集成测试通过 CSR 指令配置入口，在 ECALL 后调整 mepc 并返回继续执行。

M 扩展测试覆盖四种乘法结果、四种除法和余数结果、零除数、`INT_MIN / -1`、全部寄存器字段及非法 `funct7` 编码。

镜像测试用手工构造的 ELF32 覆盖段映射、`.bss` 清零、符号查找、越界段与各种畸形头；差分比较测试覆盖相等、仅 PC 不同、首个不同寄存器定位、x0 归一化与报告格式。

中断测试分三层。CLINT 层覆盖复位值（mtimecmp 复位为 `UINT64_MAX`，避免复位即触发）、msip 位宽、mtimecmp 与 mtime 的高低半字独立写、离散 tick、`UINT64_MAX` 不触发、`mtime == mtimecmp` 恰好触发、deadline 为 `mtime + 1` 不触发、64 位加法回绕、区域边界与未对齐 / 未映射错误。PLIC 层覆盖 priority、pending（只读）、enable、threshold 的 MMIO 往返，source 0 保留，`priority > threshold` 的严格比较，priority 为 0 永不触发，claim 清除 pending 并置 in-service、claim 仲裁（最高优先级、同优先级取最小号）、claim 后 MEIP 撤销、complete 清除 in-service。CPU 层覆盖三种源写入 `mip` 的采样、`mie` 掩码截断、`mstatus.MIE` 与 `mie` 的联合屏蔽、原因码优先级、mcause 最高位、mepc 指向未执行的下一条指令、mtval 为 0、入口 mstatus 与 MRET 按 MPIE 恢复 MIE、中断在取指前判定因而优先于同一条指令的同步异常，并用固定机器码跑完整的设备 → 中断 → 处理程序 → MRET → 继续执行回路。PLIC 层另有一例固定简化 gateway 的边界：in-service 期间再次 raise 会立刻重新拉起 MEIP，complete 只清 in-service 而不再采样电平。Machine 层覆盖设备连接 / 清理、未连接设备时拒绝服务、整机复位同时复位设备、两台机器设备互不影响、每步只推进一 tick；另有一例检查 MMIO 窗口只是数据映射，指令取指仍只认 RAM。

中断测试另做了一轮变异检查：在副本里注入 7 个缺陷——中断原因码优先级改成软件优先、中断的 mtval 填 PC、MTIP 的判据改成严格大于、PLIC 阈值比较改成不小于、claim 不清 pending、source 0 可 raise、`mie` 写入不做掩码截断——`clint` / `plic` / `interrupt` 三组测试对每一个都报告失败，没有存活者。

### 外部验证层

以下结果来自外部工具链与参考模型，依赖缺失时对应的 CTest **不注册或报 SKIP**，从不显示为通过。

- **逐指令差分测试**：18 个 Guest 镜像（5 个手写源 × -O0 / -O2，加 8 个随机生成程序）与 NEMU riscv32 参考模型逐条比较全部一致，结束时整段 RAM 一致。
- **变异测试（DUT 侧）**：向副本注入 4 个已知缺陷——SLTI 有符号比较改无符号、JALR 不清 bit 0、DIV 除零结果改 0、LB 去符号扩展——差分测试对每一个都报告了不一致，并给出出错指令的 pc 与机器码。测试本身若放过任何一个即判失败。
- **变异测试（参考模型侧）**：把 NEMU 参考模型的 `slt` 改成无符号比较、重新构建共享对象，差分测试仍然报告不一致，而未变异时同一镜像通过。只改 DUT 只能证明工具有检测力；改参考模型才能证明比较真的读取了参考状态。
- **官方 riscv-tests**：`rv32ui` 与 `rv32um` 共 50 例，49 例通过，1 例显式 SKIP（`rv32ui-p-ma_data` 要求非对齐访存成功，而 Yan 平台按设计拒绝）。
- **官方 ACT4 测试语料**：`riscv-arch-test` 的 `rv32i/I` 与 `rv32i/M` 共 47 例，其中 40 例的 DUT 签名与 Sail 参考模型的签名逐字节相同，0 例不同，7 例 SKIP（都需要异常处理器，而框架的标准 M-mode 启动路径依赖 YanOS 仍没有的 `medeleg` / `mideleg`；`mie` / `mip` 已实现）。这一层使用官方测试源与官方参考模型，但不使用 ACT4 自带的构建系统。
- **签名比对**：8 个镜像的签名与 Sail（第三方参考模型）逐字节相同，签名区大小 256～17184 字节。

### 覆盖边界

- 差分测试比较 PC、32 个通用寄存器与结束时的整段 RAM；NEMU riscv32 参考状态里没有 CSR 字段，因此 `mstatus`、`mtvec`、`mscratch`、`mepc`、`mcause`、`mtval`、`mie`、`mip` 与 trap / MRET / 中断语义**不在差分测试覆盖范围内**，含这些指令的 Guest 不适用于该层。差分执行器不连接 CLINT / PLIC，中断线恒为低。
- 参考模型由本仓库补全（上游对应文件是留白桩），与 DUT 同作者，证明的是两份实现一致，不等同第三方模型提供的证据；第三方证据来自官方 `riscv-tests` 与 Sail 签名比对，二者都只覆盖用户态 RV32IM。

Debug 启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。内存分配失败分支尚未通过故障注入验证。

CI 覆盖 Linux Debug 检测构建、Linux Release 和 Windows Debug。外部验证层需要 NEMU 参考模型与 RISC-V 工具链，暂不在 CI 中运行，因此不对外部层做持续集成结论。各任务的运行结果与审查状态见 [Pull Requests](https://github.com/YanBo-maker/YanOS/pulls)。

## 下一步

接入 ACT4 自带的构建系统（`testplans`、UDB 配置与框架的 RVMODEL 生成），让参考模型的期望结果在构建期编译进自检 ELF，从而把当前依赖签名比对与 SKIP 的 7 个异常相关测试也纳入；这需要先补齐 `medeleg` / `mideleg` 与异常委托，或为框架提供一条不经过它们的 M-mode 启动路径。CLINT / PLIC 的寄存器已经存在，把框架的中断流程（`RVMODEL_*` 地址与 `mtvec` 安装）接到这两个设备上，是后续让 ACT4 中断用例可运行的前置工作。

已实现 40 条 RV32I 基础指令的功能路径、八条 RV32M 指令、CSR 指令和 MRET。单步返回 YAN_OK 表示正常完成，YAN_TRAP 表示已进入 Guest 异常或中断；Host 参数和对象状态错误仍直接返回。Bus、RAM 与独立取指接口保留原有错误语义。

中断目前只覆盖单 hart、M-mode、直接入口与非抢占的固定优先级：没有 S/U 模式，没有 `medeleg` / `mideleg` 委托，没有 `mtvec` 向量模式，没有中断嵌套，PLIC 只有一个 M-mode context，CLINT 只有 hart 0 的 msip / mtimecmp。也未完成完整 ISA / 特权架构符合性验收。中断与特权语义既没有参考模型可比较，因此证据只来自本仓库的模块测试，属于已知边界而非已验收能力。

## 待定设计

- 后续 RISC-V 指令、特权机制与设备的分阶段支持范围。
- ACT4 的 DUT 配置与 RVMODEL 宏的落地方式。
- YanFS 的数据语义。
- 项目许可证。
