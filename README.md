# YanOS

从可理解的计算平台出发，逐步构建 YanCPU、YanOS、YanFS 和 Yan Knowledge System。

这是一个学习驱动、需求驱动的长期项目。每次增加一个能够解释、运行和验证的能力；代码进度应与项目所有者的理解进度相匹配。

## 技术方向

- YanCPU：C17 编写的 RISC-V 功能模拟器，先在本机命令行运行。
- 初始 CPU 目标：RV32IM_Zicsr、32 位、小端、单核、仅 M-mode。
- YanOS 与 Guest 程序：C + 必要的 RISC-V 汇编。
- 浏览器：未来按需求将 Host 核心编译为 WebAssembly，并增加界面与设备适配。
- RTL、电路时序、流水线、MMU、多核和自定义指令不属于当前阶段。

这些是分阶段实现的目标，不代表当前已经支持。C 功能模拟器描述指令行为，并不模拟 CPU 内部电路。

## 架构边界

```text
Yan Knowledge System
        ↓
YanFS
        ↓
YanOS / Drivers
        ↓
Yan Machine Platform（Bus、RAM、设备）
        ↓
YanCPU 执行 Guest 指令并经 Bus 访问平台

Host Runtime 承载模拟器及设备的外部适配。
```

上图表示职责层次，CPU 与平台通过明确接口协作。CPU 不理解文件和知识对象；Guest 对外通信通过虚拟硬件接口。Host C 与 Guest C 分别编译、分别运行，不共享指针或直接函数调用。

## 当前进度

处于 Phase 0：建立项目准则与 Git 工作流。尚无 CPU 实现、构建系统或自动化测试。

下一项候选任务是明确最小 CPUState、RAM、Bus 和取指的规格，再分步实现。最终 YanFS 是否理解知识对象的业务语义仍待讨论。

## 阅读顺序

1. [开发与 Git 准则](CONTRIBUTING.md)
2. [当前状态](docs/STATUS.md)
3. [项目初始化规格](docs/specs/0001-project-foundation.md)

Agent 开始工作前还应读取 [AGENTS.md](AGENTS.md)。

## 许可

尚未选择开源许可证。仓库公开可见与授予开源许可是两件不同的事；选定许可证后再添加 LICENSE。
