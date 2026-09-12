# YanOS

YanOS 是一个用于学习计算机系统的项目，计划实现 RISC-V 模拟器、操作系统、文件系统和终端知识工具。

开发从 YanCPU 开始。通过小程序和测试观察指令执行，再逐步引入设备、异常、中断和持久化存储。每项能力都应有具体需求、明确规格和可复现的验证结果。

## 状态

项目处于 Phase 0，已建立开发准则和仓库结构。CPU、构建系统及测试尚未实现。

下一步是确定 C 工具链，以及 CPU 状态、RAM、Bus 和取指的最小规格。详细进度见 [STATUS](docs/STATUS.md)。

## 设计

YanCPU 使用 C17 编写，在本机命令行运行。初始目标为 RV32IM_Zicsr，采用 32 位、小端、单核和 M-mode 配置。模拟器关注指令执行后的状态变化，电路时序和微架构留待后续探索。

Guest 程序使用 C 和少量 RISC-V 汇编。CPU 经 Bus 访问 RAM 与虚拟设备，设备由 Host 提供终端和存储适配。Host 与 Guest 分别编译，通过虚拟硬件接口交互。

| 组件 | 职责 |
| --- | --- |
| YanCPU | 指令执行、寄存器、CSR、异常和中断 |
| Yan Machine Platform | Bus、RAM、终端、计时器与块设备 |
| YanOS | 启动、驱动、内存管理和系统接口 |
| YanFS | 持久化存储，具体语义待定 |
| Yan Knowledge System | 笔记、问题、关系和探索路径的终端交互 |

以上组件将分阶段实现。浏览器支持计划通过 WebAssembly 和界面适配接入；新增能力的时机由实际使用决定。

## 开发

参阅 [开发准则](CONTRIBUTING.md) 和 [初始化规格](docs/specs/0001-project-foundation.md)。

## 许可证

待选定。
