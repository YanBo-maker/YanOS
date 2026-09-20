# YanOS

YanOS 是一个用于学习计算机系统的项目，计划实现 RISC-V 模拟器、操作系统、文件系统和终端知识工具。

开发从 YanCPU 开始。逐步实现指令执行、设备、异常、中断和持久化存储。每项能力都应有具体需求、明确规格和可复现的验证结果。

## 状态

已实现 Machine、Bus、RAM，以及 CPU 寄存器状态、复位、取指和整数指令单步执行。已实现 RV32I 的整数计算、控制转移、访存、FENCE、FENCE.I、ECALL 和 EBREAK，并提供 CSR 指令，支持 M-mode 同步异常与中断处理及 MRET。RV32M 的八条乘除指令已加入。平台侧实现了单 hart 的 CLINT 与单 context 的 M-mode PLIC，中断线经 Bus 接入 CPU 的 `mie` / `mip`；Guest 侧有最小的启动入口、统一的陷阱处理路径与 CLINT / PLIC 访问封装。验证层已接入逐指令差分测试、官方 `riscv-tests` 套件、官方 `riscv-arch-test`（ACT4）测试语料与 Sail 签名比对。此外还接入了 UART 字符设备与 Host 传输通道：PLIC 网关按设备线的电平语义工作并已通过独立验证，UART 与传输通道分别有 19 与 14 个模块用例通过。YanOS 侧建立了 `os/` 层，它的第一个成员控制台驱动已实现并提交在 `feat/m1-device-channel`（尚未合入 `main`），接口见 [控制台与 `os/` 层规格](docs/specs/0017-console-and-os-layout.md)。S/U 模式、中断委托、分页与其余设备仍待实现。详细进度见 [STATUS](docs/STATUS.md)。

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

构建 Guest 验证工具可添加 `-DYAN_BUILD_TOOLS=ON`。`yan_run` 支持 RV32 ELF、最大步数、`tohost` 退出、签名区导出和逐条架构状态记录；`yan_difftest` 让 YanCPU 与外部参考模型逐条比较架构状态；`yan_gen` 生成随机 RV32IM 指令流。

变异检查会为每个植入的缺陷重建一次并跑一遍被测组，本机实测打开开关后整套约 1.5～7 分钟（随机器负载波动），因此默认不注册；需要这份证据时在配置时添加 `-DYAN_ENABLE_MUTATION_TESTS=ON`，它注册两个套件：`guest_console_mutation`（控制台）与 `device_mutation`（28 个变异体，覆盖 `uart` / `transport` / `plic`）。合并前必须显式跑一次，见[开发准则](CONTRIBUTING.md)的合并前检查清单。

`yan_run` 的默认执行路径由 `guest_golden` 用基准工件逐字节守住：改动默认行为会让它失败，先读差异再决定是否有意为之。生成与重生成见 [`tests/guest/golden/README.md`](tests/guest/golden/README.md)。

外部验证层是可选依赖，缺失时对应的 CTest 不注册或报 SKIP，从不显示为通过：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DYAN_BUILD_TOOLS=ON \
  -DYAN_NEMU_REF_SO=/path/to/riscv32-nemu-interpreter-so \
  -DYAN_NEMU_REF_DIR=/path/to/nemu-source \
  -DYAN_RISCV_TESTS_DIR=/path/to/riscv-tests \
  -DYAN_RISCV_ARCH_TEST_DIR=/path/to/riscv-arch-test \
  -DYAN_SAIL_BIN=/path/to/sail_riscv_sim
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

分层、覆盖边界与退出码见 [CPU 验证规格](docs/specs/0011-cpu-validation.md)。

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

代码按三层分区：Host 侧的 `src/` 与 `include/`（CPU、Bus、设备与工具）、YanOS 侧的 `os/`（在 Guest 上运行的 YanOS 代码，如控制台驱动）、以及 `tests/guest/` 下的验证程序。依赖方向单向：`os/` 不引用 `tests/`，`tests/` 可以引用 `os/`，见 [控制台与 `os/` 层规格](docs/specs/0017-console-and-os-layout.md)。

各组件的完整能力将分阶段实现。浏览器支持计划通过 WebAssembly 和界面适配接入；新增能力的时机由实际使用决定。

## 开发

参阅 [开发准则](CONTRIBUTING.md)、[阶段 0 规格](docs/specs/0003-machine-bus-ram.md)、[CPU 状态与取指规格](docs/specs/0004-cpu-state-fetch.md)、[ADDI 单步执行规格](docs/specs/0005-addi-step.md)、[整数计算规格](docs/specs/0006-integer-alu.md)、[控制转移规格](docs/specs/0007-control-flow.md)、[Load / Store 规格](docs/specs/0008-load-store.md)、[Guest 异常规格](docs/specs/0009-guest-traps.md)、[M 扩展规格](docs/specs/0010-m-extension.md)、[CPU 验证规格](docs/specs/0011-cpu-validation.md)、[机器模式中断规格](docs/specs/0012-machine-interrupts.md)、[Guest 启动与统一 trap 环境规格](docs/specs/0013-guest-trap-environment.md)、[Host 传输通道规格](docs/specs/0014-host-transport-channel.md)、[UART 字符设备规格](docs/specs/0015-uart-device.md)、[PLIC 网关与设备中断线规格](docs/specs/0016-plic-gateway-and-irq-lines.md) 和 [控制台与 `os/` 层规格](docs/specs/0017-console-and-os-layout.md)。

## 许可证

待选定。
