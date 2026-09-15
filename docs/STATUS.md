# 项目状态

当前阶段：ADDI 与单步执行。实现与本地验证已完成，用户审查待完成。Machine / Bus / RAM、CPU 状态与取指已通过用户审查并合入主分支。

## 已完成

- RAM 生命周期、小端读写、宽度与范围检查。
- Bus 的单 RAM 映射、对齐检查及错误信息。
- Machine 初始化、整机复位和内存镜像装载。
- CPU 的 32 个通用寄存器、x0 语义、PC 和独立复位。
- CPU 经 Bus 读取 32 位机器码；取指保留 PC、寄存器和内存。
- ADDI 解码与单步执行、12 位立即数符号扩展、32 位回绕及错误状态保持。
- C17 / CMake 构建、Unity 单元测试、CTest 及 GitHub Actions。

## 验证

GCC 11.4、CMake 3.22.1 下的 Debug 与 Release 构建均通过六组 CTest：RAM、Bus、Machine、CPU、Fetch、Step，共 23 个 Unity 测试用例。Step 覆盖全部 4096 种立即数编码和 1024 种源/目标寄存器组合。

ADDI 实现前，Step 的五个执行用例失败；实现后全部通过。

Debug 启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。内存分配失败分支尚未通过故障注入验证。

CI 覆盖 Linux Debug 检测构建、Linux Release 和 Windows Debug。各任务的运行结果与审查状态见 [Pull Requests](https://github.com/YanBo-maker/YanOS/pulls)。

## 下一步

审查 [ADDI 与单步执行规格](specs/0005-addi-step.md) 及配套实现，随后确定下一组整数指令的范围。

当前支持 ADDI（含标准 NOP 编码）；其他编码返回 Host 状态 `YAN_UNSUPPORTED_INSTRUCTION`。尚未实现其他指令、CSR、Guest 异常、中断和设备。Machine 装载调用者提供的内存缓冲区，文件读取及 Guest 启动仍待实现。

## 待定设计

- 后续 RISC-V 指令、特权机制与设备的分阶段支持范围。
- Guest 工具链与程序加载方式。
- YanFS 的数据语义。
- 项目许可证。
