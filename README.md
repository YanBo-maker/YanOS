# YanOS

YanOS 是一个用于学习计算机系统的项目，计划实现 RISC-V 模拟器、操作系统、文件系统和终端知识工具。

开发从 YanCPU 开始。逐步实现指令执行、设备、异常、中断和持久化存储。每项能力都应有具体需求、明确规格和可复现的验证结果。

## 状态

已实现 Machine、Bus、RAM，以及 CPU 寄存器状态、复位、取指和整数指令单步执行。当前支持 21 条 RV32I 整数计算指令，涵盖加减、比较、逻辑、移位、LUI 和 AUIPC；分支、访存及系统指令仍待实现。详细进度见 [STATUS](docs/STATUS.md)。

## 构建与测试

需要支持 C17 的编译器和 CMake 3.21 或更新版本。Windows 可使用 WSL 中的 GCC，也可以使用 Visual Studio 的 C 工具链。

在项目目录执行：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

测试采用 Unity 2.6.1，由 CTest 运行。首次配置会下载固定版本并校验 SHA-256；仅构建核心库可添加 `-DBUILD_TESTING=OFF`。

GCC / Clang 的 Unix 构建可在配置时添加 `-DYAN_ENABLE_SANITIZERS=ON`，启用地址与未定义行为检测。

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

各组件的完整能力将分阶段实现。浏览器支持计划通过 WebAssembly 和界面适配接入；新增能力的时机由实际使用决定。

## 开发

参阅 [开发准则](CONTRIBUTING.md)、[阶段 0 规格](docs/specs/0003-machine-bus-ram.md)、[CPU 状态与取指规格](docs/specs/0004-cpu-state-fetch.md)、[ADDI 单步执行规格](docs/specs/0005-addi-step.md) 和 [整数计算规格](docs/specs/0006-integer-alu.md)。

## 许可证

待选定。
