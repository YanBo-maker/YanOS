# YanOS

YanOS 是一个用于学习计算机系统的项目，计划实现 RISC-V 模拟器、操作系统、文件系统和终端知识工具。

开发从 YanCPU 开始。通过小程序和测试观察指令执行，再逐步引入设备、异常、中断和持久化存储。每项能力都应有具体需求、明确规格和可复现的验证结果。

## 状态

阶段 0 已实现 Machine、Bus 和 RAM，提供 32 MiB 内存、小端读写、地址检查、内存镜像装载及测试。CPU 与设备尚未实现。详细进度见 [STATUS](docs/STATUS.md)。

## 构建与测试

需要支持 C17 的编译器和 CMake 3.21 或更新版本。Windows 可使用 WSL 中的 GCC，也可以使用 Visual Studio 的 C 工具链。

在项目目录执行：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

运行内存示例：

```sh
./build/memory_demo
```

Visual Studio 构建的示例位于 `build/Debug/memory_demo.exe`。示例装入四个字节，经 Bus 读回一个 32 位值，并检查非法访问：

```text
RAM: 0x80000000 (33554432 bytes)
Word: 0x00700293
Bytes: 93 02 70 00
Unaligned write: rejected; RAM unchanged.
Unmapped read: rejected; output unchanged.
```

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

参阅 [开发准则](CONTRIBUTING.md)、[阶段 0 规格](docs/specs/0003-machine-bus-ram.md) 和 [代码导读](docs/stage0-walkthrough.md)。

## 许可证

待选定。
