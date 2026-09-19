# 项目状态

当前阶段：CPU 架构验证基础设施。架构状态快照已实现，ELF 执行器、官方架构测试和参考模型差分仍在开发。Load / Store 阶段已通过用户审查并合入主分支。

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
- 六种 CSR 指令与有限的机器模式 CSR 集合；ECALL、EBREAK 和当前 RAM 平台下的 FENCE。
- RV32M 的 MUL、MULH、MULHSU、MULHU、DIV、DIVU、REM、REMU，包含除零和有符号溢出规则。
- 面向外部验证模型的只读 CPU 架构状态快照接口。
- `yan_run` Guest ELF 执行器基础版本，支持最大步数、tohost 退出、签名区导出和 JSONL 架构状态记录。
- C17 / CMake 构建、Unity 单元测试、CTest 及 GitHub Actions。

## 验证

GCC 11.4、CMake 3.22.1 下的 Debug 与 Release 构建均通过十三组 CTest：RAM、Bus、Machine、CPU、Fetch、Step、ALU、Control、Memory、CSR、Trap、M、Snapshot，共 83 个 Unity 测试用例。

立即数测试覆盖全部 4096 种编码；移位覆盖 0～31 及寄存器移位量高位屏蔽。十条寄存器运算各覆盖 32768 种 rd / rs1 / rs2 组合。固定向量和边界矩阵检查符号比较、算术回绕、逻辑运算与 AUIPC 当前 PC 语义。

控制转移测试覆盖 B 型全部 4096 种偏移、J 型各偏移位与边界、JALR 全部 4096 种立即数及目标低两位。检查条件真值、寄存器字段、目标对齐、地址回绕、返回地址和延迟到下一次取指的映射错误。

访存测试覆盖八条指令的全部 4096 种偏移、寄存器字段组合、符号边界、小端读写与截断、地址和 PC 回绕、RAM 首尾及错误状态保持。取指失败时无数据访问，加载到 x0 仍检查错误；Store 可覆盖已经取出的当前指令。

异常测试覆盖各原因码、mtval、入口状态、Host 错误隔离、嵌套异常、无效入口、六种 CSR 指令的寄存器组合和读写抑制、MRET 恢复及 Machine 复位。Guest 集成测试通过 CSR 指令配置入口，在 ECALL 后调整 mepc 并返回继续执行。

M 扩展测试覆盖四种乘法结果、四种除法和余数结果、零除数、`INT_MIN / -1`、全部寄存器字段及非法 `funct7` 编码。

新增异常与系统指令执行测试均确认在实现前失败，实现后通过；原有单步错误用例已迁移到 Guest 异常语义。

Debug 启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。内存分配失败分支尚未通过故障注入验证。

CI 覆盖 Linux Debug 检测构建、Linux Release 和 Windows Debug。各任务的运行结果与审查状态见 [Pull Requests](https://github.com/YanBo-maker/YanOS/pulls)。

## 下一步

审查 [CPU 验证规格](specs/0011-cpu-validation.md)，随后实现 ELF 加载与受控执行器，再接入 NEMU 差分和 RISC-V 官方架构测试。

已实现 40 条 RV32I 基础指令的功能路径、八条 RV32M 指令、六种 CSR 指令和 MRET。单步返回 YAN_OK 表示正常完成，YAN_TRAP 表示已进入 Guest 异常；Host 参数和对象状态错误仍直接返回。Bus、RAM 与独立取指接口保留原有错误语义。架构状态快照可供外部参考模型逐条比较。

当前只覆盖 M-mode 同步异常及规格列出的 CSR，尚未实现中断、U/S 模式、分页和设备，也未完成完整 ISA / 特权架构符合性验收。官方架构测试和 NEMU 差分测试尚未接入。Machine 装载调用者提供的内存缓冲区，Host 文件加载与 Guest 工具链集成仍待实现。

## 待定设计

- 后续 RISC-V 指令、特权机制与设备的分阶段支持范围。
- Guest 工具链与程序加载方式。
- YanFS 的数据语义。
- 项目许可证。
