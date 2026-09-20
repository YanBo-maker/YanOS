# CPU 验证规格

## INTENTION

模块测试适合定位实现错误，不能回答 CPU 在真实 Guest 指令流中是否**持续**保持架构状态，也不能发现某一步错误对后续执行的影响。验证层要把 YanCPU 放到外部参考模型和官方测试套件面前，并且明确声明哪些架构状态已被覆盖、哪些没有。

本项目使用两条外部路线：一生一芯的逐指令差分思路（参考模型 NEMU），以及 RISC-V 官方测试（riscv-tests 自检套件与 Sail 参考签名比对）。三条链——测试生成、参考模型签名、DUT 执行器——都必须在同一次运行里真实参与比对。

## SPEC

验证分成四层。前两层随仓库自带，后两层是可选外部验证层。

1. **模块测试**：Unity / CTest，覆盖 RAM、Bus、Machine、CPU、取指、整数、控制转移、访存、CSR、异常、M 扩展、架构快照、机器模式中断、CLINT / PLIC 设备，以及 Guest 平台视图与陷阱 ABI 的跨层一致性，另有镜像加载与差分比较两个模块。
2. **逐指令差分测试**：`yan_difftest` 加载同一份 Guest 镜像，让 YanCPU 与外部参考模型从同一内存镜像、同一复位状态出发，YanCPU 每提交一条指令就与参考模型比较一次 PC 与 32 个通用寄存器；首次不一致时输出出错指令地址、机器码、首个不同寄存器与两侧完整寄存器表。运行结束时再比较整段 RAM。
3. **官方架构测试**：`riscv-tests` 的 `rv32ui` 与 `rv32um` 自检套件。测试通过 `tohost` 报告结果，`yan_run` 从 ELF 符号表解析该符号，不硬编码地址。
4. **签名比对**：`sail_riscv_sim`（第三方独立实现）按 ELF 的 `begin_signature` / `end_signature` 符号导出期望签名，`yan_run --signature` 导出 DUT 签名，两者逐字节 `cmp`。签名区由链接脚本决定，在本仓库的 Guest 语料里它覆盖整个可写段（含栈），因此这一层比较的是**步数上限处的整段可写状态**，而不是 ACT 风格的显式输出数组。
5. **官方 ACT4 测试语料**：直接使用 `riscv-arch-test` 的官方测试源（`tests/rv32i/I` 与 `tests/rv32i/M`），用本仓库的 DUT 配置 `tests/act4/`（`rvtest_config.h`、`rvmodel_macros.h`、`link.ld`）汇编成镜像；同一镜像由 Sail 产生参考签名、由 `yan_run` 产生 DUT 签名，逐字节比较。这一层不使用 ACT4 自带的构建系统（它需要 UDB/Docker 环境），但**测试源与参考模型都是官方的**。

### 覆盖边界

以下边界必须与实现一致，不能靠测试名称掩盖。

- 差分测试比较 **PC + 32 个通用寄存器 + 结束时的整段 RAM**（x0 按架构语义归一化为零，两侧内存镜像在开始前完全一致）。
- 差分测试**不比较 CSR**：`mstatus`、`mtvec`、`mscratch`、`mepc`、`mcause`、`mtval`、`mie`、`mip` 以及 trap / MRET / 中断语义都不在 NEMU riscv32 参考状态里（`DIFFTEST_REG_SIZE` 只有 132 字节 = 32 GPR + pc）。含 CSR、ECALL / EBREAK、MRET 的 Guest 不适用于本层；DUT 进入异常或中断时工具返回 3，并显式说明该情形超出参考模型范围。差分执行器不连接 CLINT / PLIC，因此 `yan_bus_pending_interrupts` 恒为 0，这一层也不会进入中断。
- 参考模型由本仓库补全：上游 NEMU 的 `src/cpu/difftest/ref.c` 与 `src/isa/riscv32/inst.c` 是留白桩，本项目的 RV32IM 参考实现与 DUT 出自同一作者。因此它证明的是**两份实现的一致性**，证据强度不等同第三方模型。
- 官方测试层只覆盖 `riscv-tests` 的 `rv32ui` 与 `rv32um`。`rv32ui-p-ma_data` 是**显式排除项**：Yan 平台按设计拒绝非对齐访存，而该测试要求非对齐访问成功；脚本打印 SKIP 与原因，不计为通过。`riscv-tests` 是自检式套件，不产生签名，因此它不能替代签名比对，也不与 `riscv-arch-test`（ACT4）互相等同。
- 签名层的参考模型是 Sail（第三方独立实现），但它只在用户态 RV32IM 上运行，同样不为 CSR / 特权语义提供证据。
- ACT4 官方框架（`riscv-arch-test`、RVMODEL 宏、`testplans`、Sail 期望结果编译进自检 ELF）**尚未接入**。

### ACT4 层的能力边界

- 只选 `tests/rv32i/I` 与 `tests/rv32i/M`。YanOS 有单 hart 的 CLINT 与单 context 的 M-mode PLIC，但没有 S/U 模式、没有中断委托、没有 PMP，框架的陷阱处理器与中断助手又都 gated 在 `STANDARD_SM_SUPPORTED` 之内，因此 `tests/rv32i/Zicsr`、`tests/rv32i/priv` 及其以上都跑不了。这两个目录里也没有任何用例调用中断宏，所以这一层今天只交付接口，不交付中断通过数。
- 汇编时必须传 `-DUNROLLSZ=0`：框架默认把入口对齐到 32 字节，在没有 C 扩展时只能用零填充，而执行流会踩过这段填充（Sail 与 YanOS 都会在那里取到非法指令）。
- 需要异常处理器的测试（非对齐的分支/跳转目标）报 SKIP：框架把陷阱处理器的实例化与整套 M-mode 中断助手都放在 `#ifdef STANDARD_SM_SUPPORTED` 之内，而该开关的前提是 `medeleg` / `mideleg` 委托、PMP 与 S-mode，这三者仍不在 YanOS 之列，因此 `mtvec` 不会被安装，两个模型都会落到地址 0 并报 `possible trap loop`。`mie` / `mip` 与 CLINT / PLIC 已实现，但都不是这个开关需要的东西，所以这条 SKIP 的原因没有消失。
- **中断与计时器由语料检查拦住，不由宏拦住**：本框架在 DUT 头之后 include `sail_macros.h`，并把 `RVMODEL_SET/CLR_MEXT_INT`、`SET/CLR_MSW_INT` 全部 `#undef` 成真实的 CLINT/PLIC 实现，因此 DUT 头里的 `.error` 守卫**不生效**（`-DRVTEST_SELFCHECK` 下才会生效）。真正生效的守卫在 `tests/official/run_act4.sh`：选中测试的**源码文本**里出现 `RVMODEL_(SET|CLR)_(MEXT|MSW)_INT`、`RVMODEL_MSIP_ADDRESS` 或 `RVMODEL_MTIME_ADDRESS` 时，该用例在运行前就被拒绝并报 SKIP。这条检查只看测试 `.S` 文本，不覆盖在 env 头里间接展开的路径；当前 47 个用例全部有明确归属（40 通过、7 因需要陷阱处理器而 SKIP）。`tests/rv32i/I` 与 `tests/rv32i/M` 里没有用例调用中断宏，所以这一层目前没有中断用例可跑。
- **两项配置检查**：脚本先做两次构建期探针，任一项失败即整体 FAIL，而不是 SKIP。(1) DUT 头声明的中断宏守卫是否真的被框架覆盖——探针必须让 `riscv_arch_test.h` 真正预处理（传 `-DTEST_FILE` 与 `-DSIGUPD_COUNT`），否则 `check_defines.h` 会在到达 DUT 头之前就中止，空的预处理输出会被误读成「框架覆盖了守卫」。(2) `tests/act4/rvmodel_macros.h` 声明的 CLINT 地址是否等于框架的有效地址——`sail_macros.h` 会用 `SAIL_*` 值替换 `RVMODEL_*`，所以不一致在生成代码里完全不可见；探针用 `.if` / `.error` 汇编，宏未定义也算不一致，从而同时拦住「DUT 头不再声明映射」这种情况。
- 这一层用签名比对决定结论，**不使用测试自身的 pass/fail 字**，也不使用 `tohost`：ACT4 的 `sail_macros.h` 把 `tohost` 当作 HTIF 控制台寄存器，逐字符写摘要串（首字符是 `'\n'` = 10），所以"第一个非零值"不是停机请求。`yan_run` 因此用 `--ignore-tohost` 跑满步数上限，只导出签名。只有两侧签名都存在且逐字节相同时才算通过。

### 退出码

`yan_run` 与 `yan_difftest` 共用出口约定，调用方不能把非零一律当作同一类失败。

| 码 | 含义 |
| --- | --- |
| 0 | PASS：命中 `tohost` 且值为 1（差分测试还要求每条指令状态一致、结束时 RAM 一致） |
| 1 | 差分测试发现首个不一致（仅 `yan_difftest`） |
| 2 | 用法或 Host I/O 错误 |
| 3 | DUT 进入异常，超出参考模型范围（仅 `yan_difftest`） |
| 4 | 达到步数上限仍未命中 `tohost` |
| 5 | 工具、镜像或参考模型内部失败 |
| 6 | Guest 通过 `tohost` 报告失败（值不为 1），工具同时打印该值。注意：当 Guest 把 `tohost` 用作控制台一类用途时该判据不成立，此时用 `--ignore-tohost` |
| 7 | 跑到步数上限并已导出签名区（`yan_run --signature`，`--ignore-tohost` 时也返回该值） |

### 运行方式

外部验证层由 CMake 选项开启；依赖缺失时它们**不注册或报 SKIP**，从不显示为通过。

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

`YAN_NEMU_REF_DIR` 只在参考模型侧变异测试中使用：它指向 NEMU 源码，测试会在副本里改坏一条指令、重新构建参考模型，并要求差分测试仍然报告不一致。

各层也可以单独运行：

```sh
./yan_difftest --image dut.elf --ref-so ref.so --max-steps 2000000 --report dut.jsonl
./yan_run --image test.elf --max-steps 4000000
./yan_run --image test.elf --max-steps 2000000 --signature dut.sig 0x80000610 0x80004930
```

参考模型共享对象按一生一芯流程构建：`NEMU_HOME=<nemu> make SHARE=1 ENGINE=interpreter`。

## IMPLE PLAN

1. ~~暴露只读的架构状态快照接口，固定差分比较的数据布局。~~ 已完成。
2. ~~增加 ELF32 little-endian Guest 加载器与受控执行器，支持入口地址、最大步数、`tohost` 退出和签名区导出。~~ 已完成，符号表解析使 `tohost` 与签名区都不依赖固定地址。
3. ~~统一提交记录格式并提供参考模型适配接口（参考模型采用 NEMU）。~~ 已完成。
4. ~~在 Linux 上以显式工具链与参考模型运行 RV32IM_Zicsr_Zifencei 子集；依赖缺失时保留本地模块测试，且不把跳过的外部测试报告为通过。~~ 已完成。
5. ~~用不一致指令的最小复现作为回归用例，并进入 CTest。~~ 已完成，形式是两组变异测试：往 DUT 注入已知缺陷，以及**改坏参考模型**。后者证明比较确实消费了参考状态，而不是把 DUT 与自己比较。
6. ~~接入 ACT4 官方框架，替换当前的自检式官方套件。~~ 部分完成：官方 ACT4 的**测试语料与参考模型**已接入（见第 5 层），框架自带的构建系统没有接入。

## VERIFY

外部验证层需要一个 RISC-V 裸机工具链、一个 NEMU 参考共享对象、一份 `riscv-tests` 检出和 Sail。工具链按 `YAN_RISCV_PREFIX`、`riscv64-unknown-elf-gcc`、`riscv32-unknown-elf-gcc`、`riscv64-linux-gnu-gcc` 的顺序探测；本机 `riscv64-unknown-elf-gcc` 不在 `PATH` 中，所以按上面的命令配置时实际选中的是 **`/usr/bin/riscv64-linux-gnu-gcc`（GCC 11.4.0）**。要使用 `/home/ybg/opt/riscv/usr/bin` 下解包的 `riscv64-unknown-elf-gcc 10.2.0`，需要同时把该目录加入 `PATH` 并传 `-DYAN_RISCV_PREFIX=riscv64-unknown-elf-`。

以下数字是 Debug 构建、本机实测结果：

| 层 | 结果 |
| --- | --- |
| 模块测试 | 19 个套件、133 个 Unity 用例全部通过（启用 Guest 工具链时另有 `guest_trap_env`，共 20 组）。接入 UART 与传输通道后为 21 个套件（启用工具链时 22 组），新增用例另计 |
| 逐指令差分测试 | 18 个 Guest 镜像（5 个手写源 × -O0 / -O2 + 8 个随机生成程序）逐条比较全部一致，结束时 RAM 一致 |
| 变异测试（DUT 侧） | 4 个注入缺陷（SLTI 有符号改无符号、JALR 不清 bit 0、DIV 除零改 0、LB 去符号扩展）全部被检出，均报出出错指令 pc 与机器码 |
| 变异测试（参考模型侧） | 把 NEMU 的 `slt` 改成无符号比较并重新构建参考模型，差分测试仍然报告不一致，且未变异时同一镜像通过 |
| 官方 riscv-tests | `rv32ui` + `rv32um` 共 50 例：49 通过，1 例显式 SKIP（`rv32ui-p-ma_data`） |
| 签名比对 | 8 个镜像的签名与 Sail 逐字节相同（手写用例 16384 / 17184 字节，随机程序 256 字节） |
| 官方 ACT4 测试语料 | `rv32i/I` + `rv32i/M` 共 47 例：40 例签名与 Sail 逐字节相同，0 例不同，7 例 SKIP（都需要异常处理器） |
| 超范围判定 | 故意非对齐访存的 Guest 被判为 exit 3（`mcause = 4`），且报告里不出现 `MISMATCH` |
| CTest 汇总 | 本机按当前 `CMakeLists.txt` 实测的**注册数**：只做本地模块测试时 21 个；加 `-DYAN_BUILD_TOOLS=ON` 为 22 个；再给出 `YAN_NEMU_REF_SO`、`YAN_RISCV_TESTS_DIR`、`YAN_RISCV_ARCH_TEST_DIR`、`YAN_SAIL_BIN` 为 28 个；再加上 `YAN_NEMU_REF_DIR` 共 29 个。这是注册数而不是通过数：各组的通过情况以 [STATUS](../STATUS.md) 的验证一节为准。外部验证层的测试只在 `YAN_BUILD_TOOLS=ON` 时注册，因此上一阶段记录的 25 / 27 不能按 +2 机械推算 |
| 中断层变异检查 | 在副本里注入 7 个缺陷（中断原因码优先级改成软件优先、中断 mtval 填 PC、MTIP 改成严格大于、PLIC 阈值改成不小于、claim 不清 pending、source 0 可被驱动、`mie` 写入不截断），`clint` / `plic` / `interrupt` 三组测试对每一个都报告失败。注意这一轮针对的是**旧网关**（接口名当时为 `raise`）；电平网关那一轮（[0016](0016-plic-gateway-and-irq-lines.md) 的 E 节）由独立验证进行，结果见 [STATUS](../STATUS.md) |

模块测试开启 AddressSanitizer 与 UndefinedBehaviorSanitizer 后同样通过。本阶段新增的 `transport` 套件尚未全绿，不在该结论内。

尚未完成：ACT4 官方框架（`testplans` / UDB / RVMODEL 生成）未接入；CSR、特权、异常与中断语义没有外部参考模型覆盖；CLINT / PLIC 的与平台相关行为（gateway 电平锁存、多 context、多 hart）不在验证范围内；Guest 陷阱环境的证据来自本仓库自检，不是与外部模型的一致性证据，且外部中断（MEIP）无法从 Guest 侧驱动，只由机器层测试覆盖。

参考：[一生一芯 DiffTest 介绍](https://oscpu.github.io/ysyx/events/2021-07-17_Difftest/difftest%E6%A1%86%E6%9E%B6%E4%BB%8B%E7%BB%8D.pdf)、[riscv-tests](https://github.com/riscv-software-src/riscv-tests)、[RISC-V Architectural Test](https://github.com/riscv/riscv-arch-test)、[Sail RISC-V model](https://github.com/riscv/sail-riscv)。
