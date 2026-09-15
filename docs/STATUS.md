# 项目状态

当前阶段：CPU 状态与取指。实现与本地验证已完成，用户审查待完成。阶段 0 已合入主分支。

## 已完成

- RAM 生命周期、小端读写、宽度与范围检查。
- Bus 的单 RAM 映射、对齐检查及错误信息。
- Machine 初始化、整机复位和内存镜像装载。
- CPU 的 32 个通用寄存器、x0 语义、PC 和独立复位。
- CPU 经 Bus 读取 32 位机器码；取指保留 PC、寄存器和内存。
- C17 / CMake 构建、Unity 单元测试、CTest 及 GitHub Actions。

## 验证

GCC 11.4、CMake 3.22.1 下的 Debug 与 Release 构建均通过五组 CTest：RAM、Bus、Machine、CPU、Fetch，共 16 个 Unity 测试用例。

Debug 启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。内存分配失败分支尚未通过故障注入验证。

CI 覆盖 Linux Debug 检测构建、Linux Release 和 Windows Debug。当前分支的运行结果与审查状态见 [CPU 状态与取指 PR](https://github.com/YanBo-maker/YanOS/pull/4)。

## 下一步

审查 [CPU 状态与取指规格](specs/0004-cpu-state-fetch.md) 及配套实现，随后确定首批指令的解码与执行规格。

当前取指只返回机器码，尚未实现指令执行、CSR、异常、中断和设备。Machine 装载调用者提供的内存缓冲区，文件读取及 Guest 启动仍待实现。

## 待定设计

- 后续 RISC-V 指令、特权机制与设备的分阶段支持范围。
- Guest 工具链与程序加载方式。
- YanFS 的数据语义。
- 项目许可证。
