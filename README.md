# YanOS

YanOS 是一个用于学习计算机系统的项目，计划实现 RISC-V 模拟器、操作系统、文件系统和终端知识工具。

开发从 YanCPU 开始。逐步实现指令执行、设备、异常、中断和持久化存储。每项能力都应有具体需求、明确规格和可复现的验证结果。

## 状态

YanCPU 已实现 RV32IM 整数指令、CSR、M-mode 异常与中断；平台包含 RAM、CLINT、PLIC、UART 和 Host 传输通道。`os/` 已有控制台驱动、块请求协议与协作式任务运行时。`yan_run --disk-image FILE` 接入已有块镜像，持久化后端已验证 Guest 写入与新进程读回；[0020](docs/specs/0020-persistent-block-image.md) 锁定成功前刷新、失败后停用、失败写不保证回滚的契约。

[YanFS](docs/specs/0021-yanfs.md) 已实现并验证格式化、挂载、列目录、读取、创建、整文件覆盖、删除和空间回收。初版采用单根目录、最多 63 文件和连续 extent；覆盖需要新旧内容同时容纳，碎片可能导致 NOSPACE。元数据写失败可能拒挂，不保证断电一致性，不自动修复。实现与验证已完成。

[Guest 终端文件操作](docs/specs/0022-terminal-file-operations.md) 已实现并验证：独立应用提供 help、ls、stat、cat、create、write、rm 和 exit，使用 UART 中断等待输入，接入已有 YanFS 镜像。create 创建新文件，write 整文件覆盖；单行最多 1023 字节，超长或非法控制字节整行拒绝。cat 保留合法 UTF-8，并转义指定控制字节和非法编码。实现与验证完成。

[多行文本编辑](docs/specs/0023-multiline-text-editor.md) 已实现并验证：edit加载或新建16KiB内存草稿，用行式命令追加、替换、删除和显示，一次保存或取消；保留已有换行格式。

上述能力已于2026-10-05经项目所有者批准，通过 [PR #16](https://github.com/YanBo-maker/YanOS/pull/16) 合入main，保留阶段提交。学习与理解程度仍由项目所有者判断。

S/U 模式、分页与抢占式调度尚未实现。覆盖边界、实测与用户审查状态见 [STATUS](docs/STATUS.md)。

## 构建与测试

需要支持 C17 的编译器和 CMake 3.21 或更新版本。Windows 可使用 WSL 中的 GCC，也可以使用 Visual Studio 的 C 工具链。

在项目目录执行：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

`build` 只是构建目录的名字，可以换成任意路径；同一份工作树里并行开发时，各自用独立目录（例如 `build/<任务名>`）可以避免互相覆盖缓存——直接对一个没有缓存的目录运行 `cmake --build` 会报 `could not load cache`。

测试采用 Unity 2.6.1，由 CTest 运行。首次配置会下载固定版本并校验 SHA-256；仅构建核心库可添加 `-DBUILD_TESTING=OFF`。

GCC / Clang 的 Unix 构建可在配置时添加 `-DYAN_ENABLE_SANITIZERS=ON`，启用地址与未定义行为检测。

构建 Guest 验证工具可添加 `-DYAN_BUILD_TOOLS=ON`。`yan_run` 支持 RV32 ELF、最大步数、`tohost` 退出、签名区导出和逐条架构状态记录；`yan_difftest` 让 YanCPU 与外部参考模型逐条比较架构状态；`yan_gen` 生成随机 RV32IM 指令流。

Linux 工具配置另提供 `yan_mkfs`，排他创建新 YanFS 镜像，已有文件或链接会被拒绝：

```sh
cmake -S . -B build/yanfs -DYAN_BUILD_TOOLS=ON
cmake --build build/yanfs --parallel
build/yanfs/yan_mkfs --image new.img --blocks 16
build/yanfs/yan_run --image guest.elf --disk-image new.img
```

`guest.elf` 是调用文件系统接口的 Guest 程序；公开接口见 [os/yanfs.h](os/yanfs.h)，Guest 块适配见 [os/yanfs_block.h](os/yanfs_block.h)，可运行的整体例子见 [Guest 验收程序](tests/guest/yanfs_check.c) 与 [验收脚本](tests/guest/run_yanfs.py)。mkfs 与当前工具验证使用 Linux/WSL 路径；Guest 不自动格式化镜像。

Linux/WSL 中具备 RISC-V 交叉编译器时，可构建生产终端应用；`BUILD_TESTING=OFF` 也能构建此目标，不需要 Unity：

```sh
cmake -S . -B build/terminal -DYAN_BUILD_TOOLS=ON
cmake --build build/terminal --target yanfs_terminal yan_run yan_mkfs --parallel
build/terminal/yan_mkfs --image terminal.img --blocks 16
python3 tools/yan_shell.py --run build/terminal/yan_run \
  --guest build/terminal/guest-apps/yanfs_terminal.elf --disk-image terminal.img
```

已有镜像可直接传给启动器；上面的 mkfs 仅用于创建新镜像。输入 `create hello.txt hello`、`cat hello.txt`、`write hello.txt world`、`rm hello.txt`，最后用 `exit` 结束。TEXT 不隐式添加换行，退格按字节处理。启动器临时调整 Unix TTY 并恢复完整属性；stdin EOF 不传给 Guest，必须显式 exit。默认使用很大的有限步数上限，可通过 `--max-steps` 缩小；空闲仍消耗 CPU。文件发布后输出失败不回滚，不自动修复或重新挂载。

多行笔记使用同一应用和启动器：

```text
edit note.txt
a 第一行
a 第二行
r 2 修订
p
w
cat note.txt
exit
```

编辑时提示符为 `edit> `。`a`追加LF行，`r N TEXT`替换第N行并保留原行终止符，`d N`删除一行；`p`只显示内存稿。`w`一次保存并返回shell，`q`取消且不写盘。草稿最多16384字节，命令行最多1023字节；容量或输入错误保留原稿。仅载入合法UTF-8文本，允许已有TAB和LF/CRLF，二进制仍用cat查看。公开接口见 [编辑核心](os/editor.h)。

生产入口见 [应用](apps/yanfs_terminal/main.c)，公共接口见 [shell](os/shell.h)、[读行器](os/line.h) 和 [UART 终端](os/terminal.h)。应用依赖 `os/`，不依赖 `tests/`。

变异检查默认关闭。启用工具与所需依赖后，添加 `-DYAN_ENABLE_MUTATION_TESTS=ON` 可注册十组：`editor_mutation`、`terminal_mutation`、`yanfs_mutation`、`persistent_block_mutation`、`persistent_combined_mutation`、`guest_console_mutation`、`device_mutation`、`guest_runtime_mutation`、`guest_block_mutation`、`guest_m2a_combined_mutation`。默认套件中的 `editor_mutation_gate`、`yanfs_mutation_gate` 和 `terminal_mutation_gate` 检查分类器判据，随默认回归运行；用 `-E '_mutation$'` 运行默认套件，`-R '_mutation$'` 选择实际变异。NEMU 差分变异组按外部参考模型依赖另行注册。运行要求见 [开发准则](CONTRIBUTING.md)，当前计数与测量日期见 [STATUS](docs/STATUS.md)。

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
| YanFS | 单根目录文件系统，格式与失败边界见 [0021](docs/specs/0021-yanfs.md) |
| Yan Knowledge System | 笔记、问题、关系和探索路径的终端交互 |

代码按职责分区：Host 侧的 `src/` 与 `include/`（CPU、Bus、设备与工具）、Guest 上的 `os/`（驱动、任务运行时与文件系统）、`apps/`（生产 Guest 应用），以及 `tests/`（本机与 Guest 验证程序）。依赖方向为 `apps/` → `os/`，生产应用与 `os/` 不引用 `tests/`；测试可以引用生产层。旧控制台分层约束见 [0017](docs/specs/0017-console-and-os-layout.md)，独立应用入口见 [0022](docs/specs/0022-terminal-file-operations.md)。

各组件的完整能力将分阶段实现。浏览器支持计划通过 WebAssembly 和界面适配接入；新增能力的时机由实际使用决定。

## 开发

参阅 [开发准则](CONTRIBUTING.md)、[阶段 0 规格](docs/specs/0003-machine-bus-ram.md)、[CPU 状态与取指规格](docs/specs/0004-cpu-state-fetch.md)、[ADDI 单步执行规格](docs/specs/0005-addi-step.md)、[整数计算规格](docs/specs/0006-integer-alu.md)、[控制转移规格](docs/specs/0007-control-flow.md)、[Load / Store 规格](docs/specs/0008-load-store.md)、[Guest 异常规格](docs/specs/0009-guest-traps.md)、[M 扩展规格](docs/specs/0010-m-extension.md)、[CPU 验证规格](docs/specs/0011-cpu-validation.md)、[机器模式中断规格](docs/specs/0012-machine-interrupts.md)、[Guest 启动与统一 trap 环境规格](docs/specs/0013-guest-trap-environment.md)、[Host 传输通道规格](docs/specs/0014-host-transport-channel.md)、[UART 字符设备规格](docs/specs/0015-uart-device.md)、[PLIC 网关与设备中断线规格](docs/specs/0016-plic-gateway-and-irq-lines.md)、[控制台与 `os/` 层规格](docs/specs/0017-console-and-os-layout.md)、[块请求协议规格](docs/specs/0018-block-protocol.md)、[协作式运行时规格](docs/specs/0019-cooperative-runtime.md)、[持久化块镜像规格](docs/specs/0020-persistent-block-image.md)、[YanFS 规格](docs/specs/0021-yanfs.md) 、[终端文件操作规格](docs/specs/0022-terminal-file-operations.md) 和 [多行文本编辑规格](docs/specs/0023-multiline-text-editor.md)。

## 许可证

待选定。
