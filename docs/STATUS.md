# 项目状态

当前阶段：阶段 0，Machine / Bus / RAM。实现与本地验证已完成，待用户审查。

## 已完成

- 项目说明与开发准则。
- Git 忽略规则、换行配置和 PR 模板。
- 仓库初始化及文档链接检查。
- RAM 生命周期、小端读写、宽度与范围检查。
- Bus 的单 RAM 映射、对齐检查及错误信息。
- Machine 初始化、内存复位和内存镜像装载。
- C17 / CMake / CTest 构建、内存示例及 GitHub Actions 配置。

## 验证

GCC 11.4、CMake 3.22.1 下的 Debug 检测构建与 Release 构建均通过四组 CTest：RAM、Bus、Machine、内存示例。三个单元测试程序共包含九组行为用例。

Debug 启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。内存分配失败分支尚未通过故障注入验证。

Linux Debug 检测构建、Linux Release 和 Windows Debug 三项 CI 已通过，最新结果见 [阶段 0 PR](https://github.com/YanBo-maker/YanOS/pull/3)。

## 下一步

审查 [阶段 0 规格](specs/0003-machine-bus-ram.md) 和 [代码导读](stage0-walkthrough.md)，随后细化 CPU 初始状态与取指规格。

CPU、取指、设备、文件读取及 Guest 启动尚未实现。当前 Machine 装载的是调用者提供的内存缓冲区。

## 待定设计

- RISC-V 官方规范版本及分阶段支持范围。
- Guest 工具链与程序加载方式。
- YanFS 的数据语义。
- 项目许可证。
