# 项目状态

当前阶段：M2a 的实现与验证进行中。[0018](specs/0018-block-protocol.md) 与 [0019](specs/0019-cooperative-runtime.md) 的规格已锁定；架构前置（`os/platform.h` 的通道与 PLIC 视图、`test_boot.c` 的防漂移比对）已落地并验证；协作式运行时已实现，实现侧经独立验证**未能推翻**（等价变异体、陷阱帧、注入时点三项都拿到独立证据），但同一轮验证**打回了验证门禁本身**（见下）；块协议已交付（13 个变异体、自称 0 存活），**其独立验证仍在进行，本文不把它记为已验证**。M1a 的平台设备M1a 的平台设备（PLIC 电平网关、UART、Host 传输通道）与 `os/` 控制台已随 11 条提交落到 `feat/m1-device-channel`（尚未进 `main`）：[0014](specs/0014-host-transport-channel.md) 的 v1 / v2、[0015](specs/0015-uart-device.md) 的 v1 / v2 / v3、[0016](specs/0016-plic-gateway-and-irq-lines.md)、[0017](specs/0017-console-and-os-layout.md) 与 M2a 的 [0018](specs/0018-block-protocol.md)（块请求协议）与 [0019](specs/0019-cooperative-runtime.md)（协作式运行时）均已由项目所有者审核通过并锁定；后两份对应的实现与它们要求的前置改动（扩展 `os/platform.h`、`test_boot.c` 的防漂移比对）**都还没有开始**，见「下一步」。PLIC 网关已按电平语义实现并通过独立验证；UART（19 例）与传输通道（14 例）的模块测试在 Debug ASan/UBSan 与 Release 下全绿。`os/` 层已随 0017 落地：`os/platform.h` 与 `os/console.h` 是冻结接口；控制台驱动、Guest 侧控制台自检与 Host terminal backend 已随 11 条提交落到 `feat/m1-device-channel`（尚未进 `main`）；`yan_run` 也已迁到 `YanMachine` 与 `yan_machine_step`，设备中断线在该执行器下可达。`main` 目前停在 `fa7af56`，因此本文记录的设备、控制台与 golden 都还只在任务分支上。上一阶段（Guest 启动与统一 trap 环境）已完成：模块测试、逐指令差分测试、官方 `riscv-tests` 套件与 Sail 签名比对都已接入并在本机通过；ACT4 官方框架尚未接入。M-mode 的 CLINT、PLIC、中断进入 / 返回，以及 Guest 侧的启动入口、陷阱向量与设备访问封装都已实现；中断与陷阱语义仍没有外部参考模型可比较。

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
- 单 context M-mode PLIC：标准基址 `0x0c000000`，priority、pending、enable、threshold、claim / complete，source 0 保留，只有 pending、enabled 且 priority 大于 threshold 的源拉高 MEIP。网关按电平语义工作：设备线 asserted 且该源未被 claim 时置 pending，源被 claim 后在 complete 之前不再重新 pending，complete 时线仍 asserted 才重新 pending，线 deassert 则撤销尚未 claim 的请求；平台用 `yan_plic_set_level(plic, source, asserted)` 双向驱动设备线，旧的 `yan_plic_raise` 已移除，见 [PLIC 网关与设备中断线规格](specs/0016-plic-gateway-and-irq-lines.md)。网关另经独立验证：对抗用例加 40000 步随机差分参考模型，唯一存活的变异由新增的边界用例闭合。
- UART 字符设备（[0015](specs/0015-uart-device.md) v3）：标准基址 `0x10000000`，`TXDATA`、`RXDATA`、`STATUS`、`CONTROL`、`IRQ_STATUS` 五个寄存器。`STATUS` 的三位是 `TX_READY`、`RX_READY`、`CONNECTED`，`TX_READY` 的语义是"此刻写 `TXDATA` 是否会被接受"，与写入路径用的是同一个判据，因此不会出现"报告就绪却拒绝写入"；没有 backend 时三位全 0，两个方向都以 `YAN_UNAVAILABLE` 拒绝（`include/yan/status.h` 为此新增该状态码），驱动据此可以 headless 运行而不是一直空转。单字节接收缓冲，`IRQ_STATUS` 的 `RX_IRQ_PENDING` 写 1 清除，接收线经平台映射到 PLIC source 2。Host 侧 backend 是 `YanUartTerminal`（`tx_ready` / `tx_write` 两个回调），`yan_uart_push_rx` 注入接收字节；Guest 侧的控制台驱动放在 `os/console.c`，已随 `feat/m1-device-channel` 提交（尚未进 `main`）。
- Host 传输通道（[0014](specs/0014-host-transport-channel.md)）：标准基址 `0x10001000`，标识寄存器、两个共享 `RING_BASE` 的字节环、门铃与中断寄存器。`yan_transport_configure` 校验环的大小与落位，`G2H_TAIL` / `H2G_HEAD` 由 Host 驱动；Host 侧 `yan_transport_host_consume` / `host_publish` 在未配置时返回 `YAN_UNAVAILABLE`。越界不由 `DOORBELL` 判定，而由 Host 记账 API 在搬运时判定并置 `STATUS.OVERFLOW_DETECTED`，`DOORBELL` 只做通知。Host→Guest 中断线经平台映射到 PLIC source 1。
- `os/` 层与控制台：`os/platform.h` 重述 Guest 侧 UART 与 PLIC 源号常量，`os/console.h` 冻结控制台接口（`connected` / `putc` / `puts` / `getline`，没有阻塞入口），`os/console.c` 已实现并提交在 `feat/m1-device-channel`（尚未进 `main`）。依赖方向单向：`os/` 不引用 `tests/`（本机核对 `grep -rn '#include' os/` 只命中 `console.h`、`platform.h` 与 `<stdint.h>`）。见 [控制台与 `os/` 层规格](specs/0017-console-and-os-layout.md)。
- 最小 M-mode Guest 环境：`tests/guest/mtrap_entry.S` 提供入口与 Direct 陷阱向量（固定 20 字帧、保存调用者保存寄存器与被中断的 `sp`），`mtrap.c` 提供统一调度与 CSR 访问，`guest_devices.h` 提供 Guest 可见的 CLINT / PLIC 地址与访问封装，见 [Guest 启动与统一 trap 环境规格](specs/0013-guest-trap-environment.md)。
- `yan_run` 已经改用 `yan_machine_init_with` 组装 `YanMachine` 并逐条调用 `yan_machine_step`：四个设备窗口（CLINT、PLIC、UART、transport）都解码，设备中断线由平台每步采样进 PLIC，`mtime` 每条指令推进一次。UART 接收中断因此在该执行器下可达（`guest_terminal` 的接收中断场景由红转绿），transport 窗口的 `MAGIC` 也能读到；`-h` / `--help` 打印用法到标准输出并返回 0；这两种拼写、未知选项与无参数的退出码由 `guest_terminal` 的四条 CLI 断言固定，它们在缺交叉工具链时也先于 SKIP 门槛执行（`tests/guest/run_terminal.sh`）。`yan_difftest` 保持不连接设备。
- RV32M 的 MUL、MULH、MULHSU、MULHU、DIV、DIVU、REM、REMU，包含除零和有符号溢出规则。
- 面向外部验证模型的只读 CPU 架构状态快照接口。
- RV32 ELF32 镜像加载与符号表解析；`tohost` 与签名区都由符号决定，不依赖固定地址。
- 只读的差分比较模块，输出出错指令、首个不同寄存器与两侧完整寄存器表。
- `yan_run` Guest 执行器：最大步数、`tohost` 退出、签名区导出、JSONL 架构状态记录，以及区分「通过 / Guest 报错 / 未终止 / Host 错误 / 已导出签名」的退出码。
- `yan_difftest` 差分执行器：加载外部参考模型，逐条比较架构状态，结束时比较整段 RAM。
- `yan_gen` 随机 RV32IM 指令流生成器（给定种子确定输出），以及 5 个手写 Guest 用例。
- C17 / CMake 构建、Unity 单元测试、CTest 及 GitHub Actions。

详细分层、覆盖边界与退出码见 [CPU 验证规格](specs/0011-cpu-validation.md)；该规格的「CTest 汇总」记的是 M1a 阶段的 21 / 22 / 28 / 29 组（已在规格里标注为当时快照）；**当前实测是默认 28 组、开变异开关 33 组**，见本页「验证」一节。该规格另记下「外部验证层的测试只在 `YAN_BUILD_TOOLS=ON` 时注册」这条前提；各组的通过情况以本页「验证」一节为准。

## 验证

GCC 11.4、CMake 3.22.1 下的 Debug 与 Release 构建注册 **21 组 Unity 套件**（共 **172 个用例**：`uart` 19、`plic` 15、`transport` 14，其余为既有分组；`boot` 由 4 增至 6，新增两条是 0017 的 A1 / A2）。启用 Guest 工具链与 `yan_run` 时默认另有 6 组 Guest 侧测试：`guest_trap_env`、`guest_terminal`、`guest_console`、`guest_golden`、`guest_runtime` 与 `guest_block`，合计 **28 组 CTest**；加 `-DYAN_ENABLE_MUTATION_TESTS=ON` 时为 **33 组**（再注册 `guest_console_mutation`、`device_mutation`、`guest_runtime_mutation`、`guest_block_mutation`、`guest_m2a_combined_mutation`）。**以下数字以本段为准**，本页其它段落若残留旧口径以本段覆盖。**两次独立测量**：总审核角色在全新目录实测默认 **28/28 passed，25.21 s**、`ctest -R mutation` **5/5 passed，281.19 s**；主 Agent 在开关下实测整套 **33/33 passed，317.39 s**（`guest_console_mutation` 45.4 s、`device_mutation` 40.5 s、`guest_runtime_mutation` 48.2 s、`guest_block_mutation` 151.0 s、`guest_m2a_combined_mutation` 3.7 s）——两次测量量级一致，差异来自机器负载。变异检查默认不注册，只为不让慢证据主导默认套件；合并前必须显式跑一次，见 [CONTRIBUTING](../CONTRIBUTING.md) 的「合并前检查清单」与「提交前总审核检查清单」。

变异检查的耗时有一半花在文件系统上：它的 `--work` 一度放在构建目录（WSL 的 Windows 挂载盘 drvfs），同一套变异体实测 366.7 s；把 `--work` 移到原生文件系统（`/tmp/yan-mutation`）后降到约 35～45 s。这是 drvfs 逐文件操作的代价，不是被测代码的差异。

立即数测试覆盖全部 4096 种编码；移位覆盖 0～31 及寄存器移位量高位屏蔽。十条寄存器运算各覆盖 32768 种 rd / rs1 / rs2 组合。固定向量和边界矩阵检查符号比较、算术回绕、逻辑运算与 AUIPC 当前 PC 语义。

控制转移测试覆盖 B 型全部 4096 种偏移、J 型各偏移位与边界、JALR 全部 4096 种立即数及目标低两位。检查条件真值、寄存器字段、目标对齐、地址回绕、返回地址和延迟到下一次取指的映射错误。

访存测试覆盖八条指令的全部 4096 种偏移、寄存器字段组合、符号边界、小端读写与截断、地址和 PC 回绕、RAM 首尾及错误状态保持。取指失败时无数据访问，加载到 x0 仍检查错误；Store 可覆盖已经取出的当前指令。

异常测试覆盖各原因码、mtval、入口状态、Host 错误隔离、嵌套异常、无效入口、六种 CSR 指令的寄存器组合和读写抑制、MRET 恢复及 Machine 复位。Guest 集成测试通过 CSR 指令配置入口，在 ECALL 后调整 mepc 并返回继续执行。

M 扩展测试覆盖四种乘法结果、四种除法和余数结果、零除数、`INT_MIN / -1`、全部寄存器字段及非法 `funct7` 编码。

镜像测试用手工构造的 ELF32 覆盖段映射、`.bss` 清零、符号查找、越界段与各种畸形头；差分比较测试覆盖相等、仅 PC 不同、首个不同寄存器定位、x0 归一化与报告格式。

中断测试分三层。CLINT 层覆盖复位值（mtimecmp 复位为 `UINT64_MAX`，避免复位即触发）、msip 位宽、mtimecmp 与 mtime 的高低半字独立写、离散 tick、`UINT64_MAX` 不触发、`mtime == mtimecmp` 恰好触发、deadline 为 `mtime + 1` 不触发、64 位加法回绕、区域边界与未对齐 / 未映射错误。PLIC 层覆盖 priority、pending（只读）、enable、threshold 的 MMIO 往返，source 0 保留，`priority > threshold` 的严格比较，priority 为 0 永不触发，claim 清除 pending 并置 in-service、claim 仲裁（最高优先级、同优先级取最小号）、claim 后 MEIP 撤销、complete 清除 in-service。CPU 层覆盖三种源写入 `mip` 的采样、`mie` 掩码截断、`mstatus.MIE` 与 `mie` 的联合屏蔽、原因码优先级、mcause 最高位、mepc 指向未执行的下一条指令、mtval 为 0、入口 mstatus 与 MRET 按 MPIE 恢复 MIE、中断在取指前判定因而优先于同一条指令的同步异常，并用固定机器码跑完整的设备 → 中断 → 处理程序 → MRET → 继续执行回路。PLIC 层另有电平网关的用例：线在 claim 之后保持 asserted 也不会重新 pending（in-flight 抑制），complete 时线仍 asserted 才重新 pending，线 deassert 则撤销尚未 claim 的请求。Machine 层覆盖设备连接 / 清理、未连接设备时拒绝服务、整机复位同时复位设备、两台机器设备互不影响、每步只推进一 tick；另有一例检查 MMIO 窗口只是数据映射，指令取指仍只认 RAM。

中断测试另做了一轮变异检查：在副本里注入 7 个缺陷——中断原因码优先级改成软件优先、中断的 mtval 填 PC、MTIP 的判据改成严格大于、PLIC 阈值比较改成不小于、claim 不清 pending、source 0 可被驱动电平、`mie` 写入不做掩码截断——`clint` / `plic` / `interrupt` 三组测试对每一个都报告失败，没有存活者。这一轮的 7 个缺陷针对的是旧网关；电平网关那一轮的变异检查（[0016](specs/0016-plic-gateway-and-irq-lines.md) E 节）由独立验证完成：对抗用例加 40000 步随机差分参考模型，唯一存活的变异由新增的边界用例闭合（该用例已进入 `plic` 组，见 [0016](specs/0016-plic-gateway-and-irq-lines.md) 测试计划的 A7）。这轮验证在仓库之外执行，仓库里没有对应脚本。工作区新增的 `tests/guest/run_device_mutation.sh`（注册为 `device_mutation`）对 `plic` 注入 5 个缺陷，全部被检出、没有存活者。

UART 组覆盖 [0015](specs/0015-uart-device.md) v3 的寄存器表与语义：复位值、保留位读 0、只读寄存器忽略写入、未连接时设备完全惰性（三位全 0、两个方向都返回 `YAN_UNAVAILABLE`）、已连接时正常收发、`TX_READY` 与写入接受判据一致（含"读到就绪之后 backend 撤销"这条竞态用例）、backend 缺任一回调即被拒、断开连接清接收状态、整机复位保留 backend、单字节接收缓冲（读走清 `RX_READY`、读空不改变状态、缓冲满时不覆盖旧值）、`IRQ_STATUS` 写 1 清除、窗口边界与未映射偏移、空指针与参数错误、两台 Machine 互不影响。另有两条走平台接线：`connected_uart_reports_and_transmits` 覆盖逐字节发送，`receive_interrupt_reaches_plic_source_two` 覆盖 `push_rx` → 采样 → PLIC source 2 → MEIP。该组 19 例在 Debug ASan/UBSan 与 Release 下全部通过。工作区另落了设备侧变异脚本 `tests/guest/run_device_mutation.sh`（注册为 `device_mutation`），对本组注入 10 个缺陷，全部被检出、没有存活者。

传输通道组覆盖 [0014](specs/0014-host-transport-channel.md)：标识寄存器恒定、`HOST_READY` 由配置推导、只读寄存器忽略写入、配置落位校验（2 的幂、下限、越出 RAM）、空环往返、满 / 空边界、环回绕与字节序、越界在 Host 记账处锁存首个错误、doorbell 通知已注册的 Host、`IRQ_STATUS` 写 1 清除、窗口与 Host 参数错误、中断线驱动 PLIC source 1、整机复位保留 Host 配置、两台 Machine 的通道互不影响。该组 14 例在 Debug ASan/UBSan 与 Release 下全部通过（上一轮 ASan 下两条用例未释放整机 RAM 的泄漏已随用例修复消失）。同一个 `device_mutation` 脚本对本组注入 12 个缺陷，全部被检出、没有存活者。

控制台与 `os/` 层：`os/console.c` 已实现并提交在 `feat/m1-device-channel`（尚未进 `main`），[0017](specs/0017-console-and-os-layout.md) 的 B 组自检（`tests/guest/console_check.c` 与 `run_console.sh`：11 个 scripted 场景加 6 个 `yan-run` 场景）与 C 组变异检查（10 个植入缺陷，规格要求至少 6 个）都已落地，Host terminal backend 一并完成，三组分别注册为 `guest_console` / `guest_console_mutation` / `guest_terminal`，都通过；`guest_console` 的 17 个场景全绿，`guest_console_mutation` 报 10 detected / 0 survived。0017 的 **A1 / A2 也已落地**：`tests/test_boot.c` 的 `guest_uart_view_matches_the_host` 与 `guest_plic_sources_match_the_host` 逐条比对 `os/platform.h` 与 `include/yan/uart.h` / `include/yan/machine.h` 的 UART 基址、五个偏移、三个状态位、两个中断位与两个 PLIC 源号（该文件现在 6 个用例）。

**默认路径的等价性与 golden 基准**。`yan_run` 迁到 `yan_machine_step` 后，默认路径（无 `--terminal`、transport 未配置）与迁移前逐步等价，理由不是一次性的字节比对，而是设备线的取值：`yan_uart_pending` 在未接 backend 时返回 false、`yan_transport_pending` 在未配置时返回 false，采样把两个源驱动为 deasserted，而 deasserted 只清 pending、不产生新中断（[0016](specs/0016-plic-gateway-and-irq-lines.md) 的网关表）。这条性质现在由 `guest_golden` 长期守住：它用固定探针跑默认路径，把 `--trace` 与 `--signature` 与基准工件逐字节比较（本机实测 trace 656 行、signature 16420 字节），默认行为一旦变化就必须被看见并复核，而不是事后发现。基准工件的生成方式与重生成条件见 [`tests/guest/golden/README.md`](../tests/guest/golden/README.md)。

**基准差点进不了提交**。`.gitignore` 里"验证产物不提交"的 `*.hex` / `*.jsonl` 与基准工件的扩展名撞车，两个基准文件一度被静默忽略，只剩 `README.md` 可提交；那样的后果是基准进不了仓库，`run_golden.sh` 在别的机器上因缺基准直接 `SKIP(77)`——一个看起来在守着、实际什么都没守的检查。`.gitignore` 现已用取反规则显式放行这两个文件。判断"某文件能否提交"要看 `git add --dry-run` 是否列出它，**不能只看 `git check-ignore` 的返回码**：命中取反规则时它也返回 0。

Guest 环境分两层。Host 层（`Boot`，始终运行、不需要交叉工具链）逐条比对 Guest 与 Host 的 CLINT / PLIC 设备常量、CSR 编号与 `mie` / `mstatus` 位（UART 常量比对属于 [0017](specs/0017-console-and-os-layout.md) 的 A1 / A2，尚未加入），检查陷阱帧的 17 个槽位互不重叠且帧长为 16 字节倍数，并让 CPU 执行 Guest 的 `sw` / `lw` 真正读写 CLINT 与 PLIC；另有启动契约：复位后无向量、`csrw mtvec` 只保留 31:2 位、`mstatus` 掩码行为与 Guest 启动假设一致。Guest 层（`guest_trap_env`）编译并运行一段自检程序，覆盖启动状态、向量安装、ECALL / EBREAK / 非法指令 / 非对齐访存四种同步异常走同一向量、MSIP 与 MTIP 的中断进入与返回、调用者保存寄存器跨陷阱不变、被中断指令按处理程序的决定重放或跳过，以及 CLINT / PLIC 的 Guest 访问。

Guest 层同样做了变异检查：在副本里注入 10 个缺陷（不清 MSIP、不 disarm MTIP、MTIP 未使能、不改 `mepc`、不装 `mtvec`、帧只压 4 字节、帧槽位与 `a4` 重叠、错误 CLINT 基址、错误 PLIC enable 偏移、`mtimecmp` 只写低半字），Guest 自检或 Host 测试对每一个都报告失败，没有存活者。第一轮还暴露出自检自身的漏洞：失败码 1 与「通过」的 `tohost = 1` 撞车，使 `no-mtvec` 变异体存活，修正为失败码最高位置 1 后才被检出。

### 外部验证层

以下结果来自外部工具链与参考模型，依赖缺失时对应的 CTest **不注册或报 SKIP**，从不显示为通过。

- **逐指令差分测试**：18 个 Guest 镜像（5 个手写源 × -O0 / -O2，加 8 个随机生成程序）与 NEMU riscv32 参考模型逐条比较全部一致，结束时整段 RAM 一致。
- **变异测试（DUT 侧）**：向副本注入 4 个已知缺陷——SLTI 有符号比较改无符号、JALR 不清 bit 0、DIV 除零结果改 0、LB 去符号扩展——差分测试对每一个都报告了不一致，并给出出错指令的 pc 与机器码。测试本身若放过任何一个即判失败。
- **变异测试（参考模型侧）**：把 NEMU 参考模型的 `slt` 改成无符号比较、重新构建共享对象，差分测试仍然报告不一致，而未变异时同一镜像通过。只改 DUT 只能证明工具有检测力；改参考模型才能证明比较真的读取了参考状态。
- **官方 riscv-tests**：`rv32ui` 与 `rv32um` 共 50 例，49 例通过，1 例显式 SKIP（`rv32ui-p-ma_data` 要求非对齐访存成功，而 Yan 平台按设计拒绝）。
- **官方 ACT4 测试语料**：`riscv-arch-test` 的 `rv32i/I` 与 `rv32i/M` 共 47 例，其中 40 例的 DUT 签名与 Sail 参考模型的签名逐字节相同，0 例不同，7 例 SKIP。7 例全部是 `I-beq/bge/bgeu/blt/bltu/bne/jal` 的分支与跳转用例，它们故意构造未对齐目标并期望陷阱处理器接管；框架把陷阱处理器的实例化放在 `STANDARD_SM_SUPPORTED` 之内，而该开关的前提是 `medeleg` / `mideleg` 委托、PMP 与 S-mode，YanOS 都没有，所以两侧都落到地址 0 并报「possible trap loop」，脚本据此报 SKIP 而不报通过。`mie` / `mip` 与 CLINT / PLIC 已实现，但语料里**没有任何用例调用中断宏**（`RVMODEL_SET/CLR_MEXT_INT`、`RVMODEL_SET/CLR_MSW_INT`、`RVMODEL_MSIP_ADDRESS`、`RVMODEL_MTIME_ADDRESS` 在 `tests/rv32i/I` 与 `tests/rv32i/M` 中出现 0 次），因此本阶段交付的是接口而不是 ACT4 中断通过数。脚本另有两项配置检查：DUT 头声明的 CLINT 地址必须与框架 `sail_macros.h` 的有效地址一致（探针汇编失败即 FAIL），以及 DUT 中断宏守卫确实被框架覆盖。这一层使用官方测试源与官方参考模型，但不使用 ACT4 自带的构建系统。
- **签名比对**：8 个镜像的签名与 Sail（第三方参考模型）逐字节相同，签名区大小 256～17184 字节。
- **Guest trap 环境**：编译 `tests/guest/mtrap_entry.S`、`mtrap.c`、`trap_env.c` 并由 `yan_run` 执行，Guest 自检的每一项都通过（退出码 0）。该层只用交叉工具链与 DUT，不需要参考模型，因为陷阱与中断语义不在参考模型范围内。

### 覆盖边界

- 差分测试比较 PC、32 个通用寄存器与结束时的整段 RAM；NEMU riscv32 参考状态里没有 CSR 字段，因此 `mstatus`、`mtvec`、`mscratch`、`mepc`、`mcause`、`mtval`、`mie`、`mip` 与 trap / MRET / 中断语义**不在差分测试覆盖范围内**，含这些指令的 Guest 不适用于该层。差分执行器不连接 CLINT / PLIC，中断线恒为低。
- 参考模型由本仓库补全（上游对应文件是留白桩），与 DUT 同作者，证明的是两份实现一致，不等同第三方模型提供的证据；第三方证据来自官方 `riscv-tests` 与 Sail 签名比对，二者都只覆盖用户态 RV32IM。
- Guest trap 环境由自检程序验证，证据来自本仓库：它证明陷阱路径在**这台**实现上按规格工作，不构成与外部模型的一致性证据。外部中断（MEIP）无法从 Guest 侧驱动——PLIC 没有 Guest 可写的源寄存器，源线只能由平台按设备电平用 `yan_plic_set_level` 驱动，而两条设备线的置位都由 Host 侧动作触发（`yan_uart_push_rx`、Host 写入传输环）——因此 Guest 自检只覆盖 MSIP 与 MTIP 的进入 / 返回。Host 触发的 MEIP 投递由 `Interrupt` 那组 Host 测试覆盖；UART 接收线的整条路径（`push_rx` → 采样 → PLIC source 2 → MEIP）另由 UART 组的 `receive_interrupt_reaches_plic_source_two` 覆盖。
- `yan_run` 现在按平台语义映射四个设备窗口并每条指令推进一次 `mtime`，`yan_difftest` 不映射设备、不推进时间。同一镜像在两个执行器下若读取 `mtime` 会得到不同结果，因此含设备访问的 Guest 只适用于 `yan_run`，这一条已在工具注释与规格里写明。

Debug 启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。内存分配失败分支尚未通过故障注入验证。

CI 覆盖 Linux Debug 检测构建、Linux Release 和 Windows Debug。外部验证层需要 NEMU 参考模型与 RISC-V 工具链，暂不在 CI 中运行，因此不对外部层做持续集成结论。各任务的运行结果与审查状态见 [Pull Requests](https://github.com/YanBo-maker/YanOS/pulls)。

## 下一步

完成 M1a 的收口：`feat/m1-device-channel` 上的 11 条提交（`d612bec..8a8c8c1`）已经推送，但它们**不在任何 PR 里**——PR #13 在 2026-09-20 19:06 以 `fa7af56`（`os/` 骨架那一提交）为头被合入 `main`（merge commit `03f5b80`），GitHub 因此不再推进 `refs/pull/13/head`。下一步是**为这 11 条提交开一个新的 PR** 交项目所有者审阅；`main` 目前只含到 `fa7af56`，本文记录的设备、控制台与 golden 都还只在任务分支上。

开始 M2a：[0018](specs/0018-block-protocol.md) 与 [0019](specs/0019-cooperative-runtime.md) 的规格已经锁定，实现按它们的 IMPLE PLAN 推进——先扩展 `os/platform.h`（通道寄存器与 PLIC 访问器）与 `tests/test_boot.c` 的防漂移比对，再做块协议的帧层与协作式运行时。0019 里记录了一笔已知债务：树里会同时存在 `tests/guest/mtrap_entry.S` 与 `os/trap_entry.S` 两份陷阱入口，合并成一份的条件写在那一节。

架构前置第一块（`os/platform.h` 的通道与 PLIC 视图 + `test_boot.c` 的比对）已经落地并验证：常量与 `include/yan/transport.h` / `include/yan/interrupt.h` 一一对应（只差 Host 侧的 `YAN_TRANSPORT_MIN_RING_SIZE`，那是配置校验用的常量，Guest 侧不需要）；只有 `G2H_HEAD`（Guest 生产）与 `H2G_TAIL`（Guest 消费）带 setter，Host 驱动的 `G2H_TAIL` / `H2G_HEAD` **故意没有 setter**，让"写了被忽略"这件事在接口上就不存在；流控封装在两个方向都是全有全无。实测 `test_boot` 8 个用例、全量 27/27。

同期登记两处**规格空档**（不改规格，按 IMPLE 级记录，供实现与验证遵循）：
- **`mie.MEIE` 由谁置位**：0019 把 IRQ route 列为调用方责任，但只枚举了设备 `IRQ_ENABLE` 与 PLIC 的 enable / priority / threshold 两级，漏了 CPU 一级。主 Agent 的裁定是：`yan_os_sched_run()` 入口置 `mie |= MEIE` 并使能 `mstatus.MIE`——依据是那一节的枚举**没有**把 CPU 一级交给调用方。这是填补空档、不动接口与不变量。**实现已经依赖这条裁定**：`os/task.c:80` 定义 `YAN_OS_MIE_MEIE`（`0x800`）、`:212` 在调度器入口 `csrs mie`、`tests/guest/runtime_check.c:66` 还有 `RT_FAIL_MEIE_OFF` 断言。**若项目所有者否决，运行时代码与测试都要改**，不是只改文档。
- **错误响应的集成不变量**：0018 要求 `status != 0` 时 `count = 0`，Guest 帧层又按 `count` 计算期望字节数；两者只有 Host 真的写 `count = 0` 时才自洽。**不变量本身成立且已被实现、也被两个 drive 的断言覆盖**：Host 不得发出 `status != 0` 且 `count != 0` 的响应。**但它的后果不是 Guest 挂起**：`os/block.c:229-238` 对非 OK 状态把期望载荷置 0，因此 Guest 会正常交付那个失败头、由调用方自己的检查抓到（实测 `:FAIL: fault: a failed read completes no blocks: expected 0, got 1`）。原文的"挂起"推断有误（主 Agent 用推断代替了实测，已认领并更正）；**验证方仍必须断言 Host 从不发出 `status != 0` 且 `count != 0` 的响应**，这不是规格改动，是验证要求。

协作式运行时的实现已落地并自测通过，本机独立复跑（我另建工作目录）结果一致：自检 **11/11 场景通过**，变异检查 **6 detected / 0 survived**，且 `os/task.c` 的 md5 在变异前后相同（脚本自己复原）。随后一轮**全新上下文、证伪导向**的独立验证在实现侧**未能推翻任何结论**：等价变异体（私下植入两支、11 场景全 PASS，且 `objcopy -O binary --only-section=.text` 后变异体与原件的 `.text` md5 相同——编译器已折叠不可达分支）、陷阱帧（自写探针逐寄存器确认 16 个 caller-saved 全保住；逐个删除 `sw` 的负控 16/16 被抓）、注入时点（反汇编确认落点并做 2×2 对照：窗口内变异体死、窗口外存活）。该轮同时**打回了验证门禁**，见下面那一条。块协议侧：门禁经发现者复验通过（六条打回项 CONFIRMED FIXED、14/14 变异体带 `:FAIL:` 证据），但**随后新挖出的 5 条未修完**，且 `SKIP_RETURN_CODE 77` 的洞正在全仓统一修；因此**本文不记为已验证，也不引用任何"存活 0"**。运行时侧门禁修复已交付并复验，组合面另发现并修复了 F1 死锁。**计数**：`guest_m2a_combined` 与 `guest_m2a_combined_mutation` 已注册（前者在第二个任务腾空响应环、全程不响铃，因此它同时是 `yan_run` 门铃处理的回归；后者的 `fake-wait` 变异能通过全部功能检查，只有阻塞窗口那条断言能把它分开，见 `CMakeLists.txt` 的两处注释）。实测：默认套件 **28 组**（`guest_m2a_combined` = #28，本机 1.86 s 绿）、开 `-DYAN_ENABLE_MUTATION_TESTS=ON` **33 组**；我在自己的全新构建目录里复跑 `ctest -N` 得到同样的 28 / 33，**先前写的 29 是把两个新用例都算进了默认套件，已更正**。**口径可复现**：本机 `build/` 是各构建目录的容器（`build/arch-m2a` 是主 Agent 的隔离目录，其余为各 Agent 的目录），上面的数字在隔离目录与我这边的全新目录中各得一次，结论一致。以下条目是需要留档的裁定与实测：

- **等价变异体（不可杀，必须证明而不是声称）**：0019 的 C 组第 4 条把"`switch_to` 之后不恢复中断状态"写成"无条件置 1 **或** 无条件清 0"，但**"无条件置 1"是等价变异体**：`os/task.c:396` 的 `previous` 是 `const`（`csrrci` 读并清之后**紧接** `if (previous == 0) panic`），因此此后它恒为 `YAN_OS_MSTATUS_MIE`（`0x8`）；两处 `yan_os_irq_restore(previous)`（`:417` 提前返回、`:439` 正常路径）只有置位分支可达，于是"恢复保存值"与"无条件置 1"生成完全相同的指令序列。**不存在**能区分二者的合规测试，除非删掉规格强制的前置条件检查——而 0019 自己已经写明"有了这条，save/restore 的两端就是同一个值"。**裁定**：只构造可杀的那一支（脚本里的 `restore-after-switch-clears`），不构造违背后置条件的测试去凑杀掉数、不削弱前置条件检查；存活者仍为 **0**，但该条**单列**为"等价变异体 + 证明"。口径：**每个非等价变异体都必须被杀死；"等价"必须被证明。** 规格措辞的修改（C 组第 4 条只列可杀的那一支）属语义性修改，**待项目所有者裁定后**由总审核角色落笔，裁定前不改 0019。
- **运行时并不依赖 `os/console.c`**（更正实现方报告里"必须追加 `console.c` 否则无法链接"的说法）：`os/task.c:131-133` 对 `tohost` 与 `yan_os_console_puts` / `putc` 都是 weak 声明（`:5` 先包含 `console.h` 再重声明，**顺序必需**——重声明若挪到首次调用之后，引用会退回强引用、静默失去该性质）。本机独立复现：镜像**不含** `console.c` 时链接 rc=0，运行得到 `the Guest reported failure code 2147483650`（= `0x80000000|2`，`INTERRUPTS_DISABLED`）after 36 instructions。`console.c` 的唯一作用是让 panic 的诊断行可观测，与 0019 panic 一节的措辞一致。
- **验证门禁的判据（项目级教训，本轮由独立验证打回）**：变异检查**只认断言失败**。脚本层的 timeout、信号死亡、sanitizer 报告、构建失败一律归 harness error / INCONCLUSIVE，不能计成"检出"——否则一个把测试挂死或打崩的变异体会被记成被杀掉，门禁就成了空转。同仓 `tests/guest/run_device_mutation.sh:1174` 的 `failure_kind()` 是既有正确形态（`:1254` 只把 `assertion` 记为检出）；`run_runtime.sh` 一度在 `case "$status"` 之前先比对注入计数，于是"纯挂起/纯崩溃"也能落进检出——已打回修正，并要补 baseline 守卫。**注意：当前 6 个运行时变异体的裁定仍成立**（验证方逐个核过每个至少有一条真断言型检出），被推翻的是门禁本身。**在门禁修好并重跑之前，不要引用任何"存活 0"作为证据。**
- **附带事实（影响 SKIP 的可达性）**：`CMakeLists.txt:101-102` 先 `unset(YAN_RISCV_GCC CACHE)` 再 `find_program`，所有 Guest 侧测试的注册都挂在 `YAN_RISCV_GCC` 上，因此**缺工具链时测试根本不注册**；`SKIP_RETURN_CODE 77` 只在"配置时有工具链、运行时缺依赖"时才可达。
**规格锁定后被修正（三项，均经项目所有者批准）**：

1. **0018 §整帧原子性 规则 2 的取模基准**：原文 `(H2G_HEAD + i) % RING_SIZE`，**改为 `(H2G_TAIL + i) % RING_SIZE`**，并补上理由（消费侧的读位置是消费者的读指针）。证据：三方独立确认（实现方提出、总监督复核 `os/block.c:45` 与 `tools/host_block.c:285` 两侧都按消费位置取模、独立验证 Agent 复现）；反例是 `RING = 8192`、`TAIL = 0`、发布完整 16 字节后 `HEAD = 16`，字面公式会读 16..31 而不是 0..15，半帧（`HEAD = 8`）时更会读到 8 个未发布字节；实测按字面实现后 **12/18 行变红**（capacity 行 `:FAIL: … expected 0, got 4294967293`）。**归属：主 Agent 在锁定前的漏检**（他逐行读过 0018 却没发现这一处）。
2. **0018 §发送前流控 补读侧上界**：读的响应帧是 `16 + count × 4096`，因此 `RING_SIZE = 8192` 时读与写的 `count` 上界都是 1；不补写会让读者以为读没有上界。实现取显式失败（Guest 层 `max_count` 拒绝且不写一字节；Host 对"响应放不下"的读回 status 1）。
3. **0019 VERIFY C 组第 4 条只列可杀的那一支**：清单里保留"无条件清 0"，"无条件置 1"移出并指向本页的等价性证明。依据是**双人独立确认**——实现方植入两支跑 11 场景全 PASS；独立验证 Agent 用更强证据：`objcopy -O binary --only-section=.text` 后**变异体与原件的 `.text` md5 完全相同**（GCC 已证明 `previous == 8` 并折叠掉不可达分支），8 个攻击面全部失败；规格 0019 自身在 save/restore 一节末句已自认"两端就是同一个值"。

- **门禁纪律一：检出必须指向至少一条 `:FAIL:`**（块协议门禁被 4 个元变异体证伪）。最严重的一条是**零行为变化**：只在 pump 里加一行 `fputs(..., stderr)`，就被判成 `detected by unit (wrote to standard error)`，而 unit 日志是 `266 check(s), 0 failed`——**一个什么都不改的变异体会被记成被杀掉，而 sanitizer 报告走的正是这条路径**。另三条：纯崩溃（exit 139）、fixture abort（exit 2）、Guest 陷阱晚于全部检查通过（`tohost != 1` 被一律映射成"Guest check failed"）。**纪律**：非零退出码、stderr 非空、信号死亡、sanitizer 报告、fixture abort **都不得**产出检出；Guest 侧必须把"陷阱"与"检查失败"分开。附带事实：Host unit 这条路径**没有内建超时**，会挂死，唯一上界是 CTest 的 900 s。
- **门禁纪律二：`SKIP_RETURN_CODE 77` 不得由"被测实现缺失"可达**。实测：把 `os/block.c` 移走 → 两个块脚本都退 **77**，CTest 记 `***Skipped` 并报 `100% tests passed, 0 tests failed`——**删掉被测实现反而是全绿**。对照 `run_runtime.sh` 在移走 `os/task.c` 时退 **1**（正确）。**纪律**：77 只允许表示"缺工具链或宿主依赖"；被测实现缺失必须硬失败。全仓 Guest 脚本的 77 语义审计正在进行，**结论出来之前不得声称"全部套件的 77 语义已核对"**。
- **实现缺陷（修复中，不要写成已修复）**：`tools/host_block.c:326-330` 在"回复放得下"检查与 publish **之前**就清了 `fail_next`，于是"环里放不下回复"时**注入的 status 2 被吞、下一次请求反而成功**（探针：`call 1: answered=0 served=0 fail_next=0` → `call 2: … status=0`），违反 `tools/host_block.h` 的文档语义。已打回，要求把清除时机移到 publish 成功之后并加用例钉住。
- **两处低优先项**：`os/block.h` 关于 `__ashldi3` 的注释把"依赖优化档"写成了无条件事实（实测本项目的 Guest 口径 `-O2` 下 GCC 开放编码 64 位变量位移，`-Os` 才引 `__ashldi3`）；以及一处**接口缺口（非违规）**——`os/platform.h` 只有 `push` / `pop`、没有非消费式的 peek，因此环内取模与基址算术在 `os/block.c:45` 与 `tools/host_block.c:285` **各写了一份**，不违反 0014 的"位置推进只走 push/pop"，但两份算术是重复的，登记为已知缺口。
- **组合面才发现的一个真缺陷（F1 死锁，已修）**：门铃是**边沿**，而"我有一个答不了的请求"是**电平**。泵在回复放不下时 `break` 且不消费请求，但边沿已被消费；此时 Guest 已阻塞在 `wait` 里、不会再次响铃 ⇒ **已 accept 的请求被永久搁置**。修法是把服务条件由 `doorbell_rung` 改成 `doorbell_rung || yan_transport_host_readable(&machine.transport) > 0`（请求环里还有未消费字节就重试），让门铃回到"通知"的本义。**关键点：规格里没有任何一条禁止它，两个组件各自测也都测不出来**——只有组合用例能发现，这是"集成必须真的做"的实证。
- **修好之后组合用例反而红了（反直觉但正确）**：它原来的"阻塞窗口"建立在那个 bug 之上（"A 取走第一个响应腾出空间 → 立刻 wait"，旧边沿行为下泵不会自己重试，predicate 必假）。修复后泵会自己补答，A 走到 `wait` 时 predicate 已真，走规格第 2 步的快速路径。**合法的窗口**是"A 在回复放不下时就阻塞、空间由**另一个任务**在 A 已阻塞之后腾出"；用例已按此重构，并做反向验证：把泵改回边沿触发 ⇒ 新用例 `INCONCLUSIVE combined case: no verdict within 3000000 instructions` ⇒ **证明它确实是该修复的回归测试**。重构后的 Guest 镜像里 `yan_os_transport_doorbell` 出现 **0** 次（窗口释放全程不靠响铃），`polls == 1` 判据保留，`fake-wait` 变异仍被杀死（`polls = 34`，且**全部功能检查都通过**——只有这条判据能区分）。
- **组合面的两条覆盖事实（供将来别高估覆盖）**：严格交替的驱动在生产泵下**永远不会真正阻塞**（响应在门铃后一条指令内 publish，打印 `sequential-waits idle_ticks=0`，另一个任务一次都没被调度），阻塞路径只能靠流控背压或更慢的 host 覆盖；**残余（非 API 可达）**——Guest 若绕过 framing 层直接用 `yan_os_transport_h2g_pop` 弹出非帧对齐的字节数，可把响应环永久顶在 ≥ 4080 已用，使排队的读永远答不出（`take` 不走这条路：要么整帧成功、要么完全不消费，畸形帧按帧长丢弃）。因此登记一条约束：**framing 层是唯一受支持的消费者**。另记一条成本项（非缺陷）：Guest 永不排空时，泵每个指令边界做 O(16) 的无效重试直到步数上限（实测 3M 步约 0.7 s），不饿死终端轮询或 Guest。
- **0018 门禁：发现者复验通过，随后又挖出 5 条（已派单，未修完）**。复验结论：六条打回项全部 CONFIRMED FIXED；14/14 变异体各自带 `:FAIL:` 证据；6 条永久负控（含"零行为变化"那条）全部 behaved；`fail_next` 已修（探针 `call 1: fail_next=1` → `call 2: status=2`）并有用例加变异体双重钉住。新 5 条：门禁 mutant→drive 映射缺口（**一个真的跨回绕缺陷因此 SURVIVED，而同一缺陷用 guest drive 跑就能被杀**——是门禁表格接不到）、`run_block.sh` 在执行器不支持 `--disk` 时仍 exit 0 报 PASS、陷阱级联需要新增 `DETECTED (trap cascade)` 类（陷阱 + Guest 自身 `:FAIL:` 计检出并标注；陷阱 + 0 条 `:FAIL:` 仍 INCONCLUSIVE）、`available < 16` 这条规格正文条款不该有"删掉还全绿"的用例集、`os/platform.h` 加变异接缝。**修复完成前不写成已修**。
- **`SKIP_RETURN_CODE 77` 的洞正在全仓统一修**：口径是——移走被测实现 / 测试程序 / 基线 → **非 0 非 77** 并打印 `FAIL the source under test is missing: <路径>`；移走仓库外依赖 → 仍 77；改完在完好树上必须仍全绿。**正当 77 只允许**：交叉工具链、`timeout(1)` / `nm` / `cmake` / `python3` 等宿主工具、Unity 检出、外部参考模型（NEMU 源与 `.so`）；数据类基线（如 `tests/guest/golden/probe.trace.jsonl`）默认缺失即硬失败。**这条洞自 M1 起就存在**，意味着此前"全绿"的说法对这 12 个用例是不成立的——不是某次改动的回归。
- **77 修复第二轮（清单外的 4 个洞）已交付，主 Agent 已批准其判定顺序**：46 用例的审计结果为 **BEFORE 10×rc0 / 2×rc1 / 34×rc77 → AFTER 11×rc0 / 22×rc1 / 13×rc77**，22 处变化**全部是 77→1**，13 条正当 77 保留。它把判定顺序改成"**外部依赖先判（77）→ 仓内产物后判（1）**"，并明确声明这是**语义性决定**、两个方向都测过；主 Agent 批准的理由是：本机没有 Sail / ACT4 / riscv-tests 时仍必须是 77，而"外部在、仓内产物缺"从 77 变 1——正是要封的洞。
- **一条未被触发的静默通过路径（已修）**：`run_runtime.sh` 的 `unrunnable` 计数器在 PENDING 之后仍打印 PASS 并 exit 0；已改为 FAIL + exit 1。它此前**从未被触发过**，所以既没有掩盖过失败，也没有在日志里留下痕迹——登记它是为了说明"打印 PASS 且 exit 0"本身也需要判据，不能只看输出。
- **规格列举之外的 panic 条件（加法式行为）**：运行时另加了 `UNEXPECTED_TRAP`（同步异常、或非 MEIP 的 cause）、`UNSERVICED_SOURCE`、`NO_TASK`、`EVENT_RANGE`、`PREDICATE`、`EXIT_RETURNED`。0019 那一节的措辞是"下列情形是编程错误"（举例非穷举），因此这些加法式条件站得住，其中 `UNSERVICED_SOURCE` 与 0016 的电平语义一致。**未来影响**：`UNEXPECTED_TRAP` 意味着将来把 `ecall` 用作系统调用入口时必须改这一处。

接入 ACT4 自带的构建系统（`testplans`、UDB 配置与框架的 RVMODEL 生成），让参考模型的期望结果在构建期编译进自检 ELF，从而把当前依赖签名比对与 SKIP 的 7 个异常相关测试也纳入；这需要先补齐 `medeleg` / `mideleg` 与异常委托，或为框架提供一条不经过它们的 M-mode 启动路径。CLINT / PLIC 的寄存器与 Guest 侧陷阱环境已经存在，地址映射也已与框架核对过；剩下的是框架启动路径依赖的委托与 PMP 状态，以及语料中尚不存在的中断用例。

已实现 40 条 RV32I 基础指令的功能路径、八条 RV32M 指令、CSR 指令和 MRET。单步返回 YAN_OK 表示正常完成，YAN_TRAP 表示已进入 Guest 异常或中断；Host 参数和对象状态错误仍直接返回。Bus、RAM 与独立取指接口保留原有错误语义。

中断与陷阱目前只覆盖单 hart、M-mode、直接入口与非抢占的固定优先级：没有 S/U 模式，没有 `medeleg` / `mideleg` 委托，没有 `mtvec` 向量模式，没有中断嵌套，PLIC 只有一个 M-mode context，CLINT 只有 hart 0 的 msip / mtimecmp。Guest 环境是最小运行时，不是操作系统：无系统调用 ABI、无进程、无页表、无调度。也未完成完整 ISA / 特权架构符合性验收。陷阱与中断语义没有参考模型可比较，证据来自本仓库的模块测试与 Guest 自检，属于已知边界而非已验收能力。两个设备（UART、传输通道）同样没有外部参考模型可比，证据只来自本仓库的模块测试；控制台与 `os/` 层只有 Guest 侧自检与变异检查，同样没有外部参考模型。

## 待定设计

- 后续 RISC-V 指令、特权机制与设备的分阶段支持范围。
- ACT4 的 DUT 配置与 RVMODEL 宏的落地方式。
- YanFS 的数据语义。
- 项目许可证。
