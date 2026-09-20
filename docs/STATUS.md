# 项目状态

当前阶段：平台设备（PLIC 电平网关、UART、Host 传输通道）与 `os/` 控制台。[0014](specs/0014-host-transport-channel.md) 的 v1 / v2、[0015](specs/0015-uart-device.md) 的 v1 / v2 / v3、[0016](specs/0016-plic-gateway-and-irq-lines.md) 与 [0017](specs/0017-console-and-os-layout.md) 均已由项目所有者审核通过并锁定。PLIC 网关已按电平语义实现并通过独立验证；UART（19 例）与传输通道（14 例）的模块测试在 Debug ASan/UBSan 与 Release 下全绿。`os/` 层已随 0017 落地：`os/platform.h` 与 `os/console.h` 是冻结接口；控制台驱动、Guest 侧控制台自检与 Host terminal backend 已在工作区实现，但**尚未提交**；`yan_run` 也已迁到 `YanMachine` 与 `yan_machine_step`，设备中断线在该执行器下可达。设备实现与 M1a 的提交位于 `feat/m1-device-channel` 与 PR #13，`main` 不含这些提交。上一阶段（Guest 启动与统一 trap 环境）已完成：模块测试、逐指令差分测试、官方 `riscv-tests` 套件与 Sail 签名比对都已接入并在本机通过；ACT4 官方框架尚未接入。M-mode 的 CLINT、PLIC、中断进入 / 返回，以及 Guest 侧的启动入口、陷阱向量与设备访问封装都已实现；中断与陷阱语义仍没有外部参考模型可比较。

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
- UART 字符设备（[0015](specs/0015-uart-device.md) v3）：标准基址 `0x10000000`，`TXDATA`、`RXDATA`、`STATUS`、`CONTROL`、`IRQ_STATUS` 五个寄存器。`STATUS` 的三位是 `TX_READY`、`RX_READY`、`CONNECTED`，`TX_READY` 的语义是"此刻写 `TXDATA` 是否会被接受"，与写入路径用的是同一个判据，因此不会出现"报告就绪却拒绝写入"；没有 backend 时三位全 0，两个方向都以 `YAN_UNAVAILABLE` 拒绝（`include/yan/status.h` 为此新增该状态码），驱动据此可以 headless 运行而不是一直空转。单字节接收缓冲，`IRQ_STATUS` 的 `RX_IRQ_PENDING` 写 1 清除，接收线经平台映射到 PLIC source 2。Host 侧 backend 是 `YanUartTerminal`（`tx_ready` / `tx_write` 两个回调），`yan_uart_push_rx` 注入接收字节；Guest 侧的控制台驱动放在 `os/console.c`，已在工作区实现（尚未提交）。
- Host 传输通道（[0014](specs/0014-host-transport-channel.md)）：标准基址 `0x10001000`，标识寄存器、两个共享 `RING_BASE` 的字节环、门铃与中断寄存器。`yan_transport_configure` 校验环的大小与落位，`G2H_TAIL` / `H2G_HEAD` 由 Host 驱动；Host 侧 `yan_transport_host_consume` / `host_publish` 在未配置时返回 `YAN_UNAVAILABLE`。越界不由 `DOORBELL` 判定，而由 Host 记账 API 在搬运时判定并置 `STATUS.OVERFLOW_DETECTED`，`DOORBELL` 只做通知。Host→Guest 中断线经平台映射到 PLIC source 1。
- `os/` 层与控制台：`os/platform.h` 重述 Guest 侧 UART 与 PLIC 源号常量，`os/console.h` 冻结控制台接口（`connected` / `putc` / `puts` / `getline`，没有阻塞入口），`os/console.c` 已在工作区实现（尚未提交）。依赖方向单向：`os/` 不引用 `tests/`（本机核对 `grep -rn '#include' os/` 只命中 `console.h`、`platform.h` 与 `<stdint.h>`）。见 [控制台与 `os/` 层规格](specs/0017-console-and-os-layout.md)。
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

详细分层、覆盖边界与退出码见 [CPU 验证规格](specs/0011-cpu-validation.md)；该规格的「CTest 汇总」已按本机实测改为 21 / 22 / 28 / 29 组，并记下「外部验证层的测试只在 `YAN_BUILD_TOOLS=ON` 时注册」这条前提。各组的通过情况以本页「验证」一节为准。

## 验证

GCC 11.4、CMake 3.22.1 下的 Debug 与 Release 构建注册 **21 组 Unity 套件**：RAM、Bus、Machine、CPU、Fetch、Step、ALU、Control、Memory、CSR、Trap、M、Snapshot、Image、DiffTest、Interrupt、CLINT、PLIC、Boot、UART、Transport，共 **172 个用例**（`uart` 19、`plic` 15、`transport` 14，其余为既有分组；`boot` 由 4 增至 6，新增两条是 0017 的 A1 / A2）。启用 Guest 工具链与 `yan_run` 时默认另有 4 组 Guest 侧测试：`guest_trap_env`、`guest_terminal`、`guest_console` 与 `guest_golden`，合计 **25 组 CTest**，全新 Debug ASan/UBSan 构建下本机实测 **25/25 通过**（13.6 s）。加 `-DYAN_ENABLE_MUTATION_TESTS=ON` 时再注册 `guest_console_mutation` 与 `device_mutation`，共 **27 组**，本机三轮实测均 **27/27 通过**（总耗时 95 s / 231 s / 435 s，`device_mutation` 单组 35～353 s，差异来自机器负载与并行构建）。两个变异套件默认不注册，只为不让慢证据主导默认套件；合并前必须显式跑一次，见 [CONTRIBUTING](../CONTRIBUTING.md) 的合并前检查清单。Release 尚未按这个口径复跑。

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

控制台与 `os/` 层：`os/console.c` 已在工作区实现（**尚未提交**），[0017](specs/0017-console-and-os-layout.md) 的 B 组自检（`tests/guest/console_check.c` 与 `run_console.sh`：11 个 scripted 场景加 6 个 `yan-run` 场景）与 C 组变异检查（10 个植入缺陷，规格要求至少 6 个）都已落地，Host terminal backend 一并完成，三组分别注册为 `guest_console` / `guest_console_mutation` / `guest_terminal`，都通过；`guest_console` 的 17 个场景全绿，`guest_console_mutation` 报 10 detected / 0 survived。0017 的 **A1 / A2 也已落地**：`tests/test_boot.c` 的 `guest_uart_view_matches_the_host` 与 `guest_plic_sources_match_the_host` 逐条比对 `os/platform.h` 与 `include/yan/uart.h` / `include/yan/machine.h` 的 UART 基址、五个偏移、三个状态位、两个中断位与两个 PLIC 源号（该文件现在 6 个用例）。

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

完成本阶段：把工作区的控制台、Host terminal backend、`yan_run` 端口与 golden 基准提交上来，并在提交前把默认 25 组与开关下的 27 组各在 Release 复跑一遍（本轮只跑了 Debug ASan/UBSan）。收口时按 [CONTRIBUTING](../CONTRIBUTING.md) 的合并前检查清单执行，提交后把本页的 CTest 计数按最终口径定稿。

接入 ACT4 自带的构建系统（`testplans`、UDB 配置与框架的 RVMODEL 生成），让参考模型的期望结果在构建期编译进自检 ELF，从而把当前依赖签名比对与 SKIP 的 7 个异常相关测试也纳入；这需要先补齐 `medeleg` / `mideleg` 与异常委托，或为框架提供一条不经过它们的 M-mode 启动路径。CLINT / PLIC 的寄存器与 Guest 侧陷阱环境已经存在，地址映射也已与框架核对过；剩下的是框架启动路径依赖的委托与 PMP 状态，以及语料中尚不存在的中断用例。

已实现 40 条 RV32I 基础指令的功能路径、八条 RV32M 指令、CSR 指令和 MRET。单步返回 YAN_OK 表示正常完成，YAN_TRAP 表示已进入 Guest 异常或中断；Host 参数和对象状态错误仍直接返回。Bus、RAM 与独立取指接口保留原有错误语义。

中断与陷阱目前只覆盖单 hart、M-mode、直接入口与非抢占的固定优先级：没有 S/U 模式，没有 `medeleg` / `mideleg` 委托，没有 `mtvec` 向量模式，没有中断嵌套，PLIC 只有一个 M-mode context，CLINT 只有 hart 0 的 msip / mtimecmp。Guest 环境是最小运行时，不是操作系统：无系统调用 ABI、无进程、无页表、无调度。也未完成完整 ISA / 特权架构符合性验收。陷阱与中断语义没有参考模型可比较，证据来自本仓库的模块测试与 Guest 自检，属于已知边界而非已验收能力。两个设备（UART、传输通道）同样没有外部参考模型可比，证据只来自本仓库的模块测试；控制台与 `os/` 层只有 Guest 侧自检与变异检查，同样没有外部参考模型。

## 待定设计

- 后续 RISC-V 指令、特权机制与设备的分阶段支持范围。
- ACT4 的 DUT 配置与 RVMODEL 宏的落地方式。
- YanFS 的数据语义。
- 项目许可证。
