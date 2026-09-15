# 项目状态

当前阶段：RV32I 整数计算。实现与本地验证已完成，用户审查待完成。ADDI 与单步执行阶段已通过用户审查并合入主分支。

## 已完成

- RAM 生命周期、小端读写、宽度与范围检查。
- Bus 的单 RAM 映射、对齐检查及错误信息。
- Machine 初始化、整机复位和内存镜像装载。
- CPU 的 32 个通用寄存器、x0 语义、PC 和独立复位。
- CPU 经 Bus 读取 32 位机器码；取指保留 PC、寄存器和内存。
- 21 条 RV32I 整数计算指令的单步执行，支持清单见 [整数计算规格](specs/0006-integer-alu.md)。
- 符号扩展、32 位回绕、移位量屏蔽及编码拒绝后的状态保持。
- C17 / CMake 构建、Unity 单元测试、CTest 及 GitHub Actions。

## 验证

GCC 11.4、CMake 3.22.1 下的 Debug 与 Release 构建均通过七组 CTest：RAM、Bus、Machine、CPU、Fetch、Step、ALU，共 36 个 Unity 测试用例。

立即数测试覆盖全部 4096 种编码；移位覆盖 0～31 及寄存器移位量高位屏蔽。十条寄存器运算各覆盖 32768 种 rd / rs1 / rs2 组合。固定向量和边界矩阵检查符号比较、算术回绕、逻辑运算与 AUIPC 当前 PC 语义。

立即数、寄存器和高位立即数三组新增执行测试均确认在对应实现前失败，实现后通过。

Debug 启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。内存分配失败分支尚未通过故障注入验证。

CI 覆盖 Linux Debug 检测构建、Linux Release 和 Windows Debug。各任务的运行结果与审查状态见 [Pull Requests](https://github.com/YanBo-maker/YanOS/pulls)。

## 下一步

审查 [整数计算规格](specs/0006-integer-alu.md) 及配套实现，随后进入控制转移指令，定义分支、跳转和目标地址检查。

当前支持整数计算子集；其余编码返回 Host 状态 `YAN_UNSUPPORTED_INSTRUCTION`。控制转移、访存、M 扩展、FENCE、CSR、Guest 异常、中断和设备尚未实现。Machine 装载调用者提供的内存缓冲区，文件读取及 Guest 启动仍待实现。

## 待定设计

- 后续 RISC-V 指令、特权机制与设备的分阶段支持范围。
- Guest 工具链与程序加载方式。
- YanFS 的数据语义。
- 项目许可证。
