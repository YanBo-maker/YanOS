# 0013：Guest 启动与统一 trap 环境

## INTENTION

[机器模式中断](0012-machine-interrupts.md)让 CPU 能采样设备线并进入 M-mode 中断陷阱，但 Guest 一侧什么都没有：`tests/guest/start.S` 只设置 `sp`、清 `tohost` 并调用 `main`，没有安装 `mtvec`，也没有处理程序。于是任何一条 ECALL、非法指令或访存故障都会跳到地址 0，Guest 只能死在那里；PLIC / CLINT 也只有 Host 侧的 C 接口，Guest 写不出可运行的驱动代码。

本阶段补上这一层：一个最小的 M-mode Guest 启动入口、一个统一的 trap 处理路径、以及 Guest 可见的设备访问封装。目标不是操作系统，而是**一条能被观测的通道**——同步异常与 MSIP / MTIP / MEIP 都进入同一个向量，处理程序决定继续执行的位置，`mret` 回到那个位置；同时把 ACT4 中断用例真正需要的接口准备好，并说清还缺什么。

范围严格限制在单 hart、M-mode。不实现 S/U 模式、`medeleg` / `mideleg` 委托、`mtvec` 向量模式、中断嵌套或任何新设备。

## SPEC

### 启动

| 项 | 约定 |
| --- | --- |
| 入口 | `_start`，位于 `.text.start`，被 `link.ld` 放在镜像基址 |
| 栈 | `__yan_guest_stack_top`，`.bss` 中 16 KiB |
| 向量 | `__yan_guest_trap_vector`，4 字节对齐，Direct 模式写入 `mtvec` |
| 启动序列 | `sp` → 清 `tohost` → `yan_guest_boot()` → `main()` |
| 启动后的状态 | `mtvec` = 向量地址，`mstatus.MIE = 0`，`mie = 0` |
| `main` 返回 | 写入 `tohost = 0xdead`（非 1 即非通过），不当作正常结束 |

`yan_guest_boot()` 先安装向量再清中断使能：安装之前发生故障无处可去，安装之后至少可报。启动序列不使用 `medeleg` / `mideleg` / PMP，因此在一台只有机器模式 CSR 的 CPU 上也能执行。

`mtrap_entry.S` 与既有的 `start.S` **并存而不替换**。`start.S` 是差分测试语料的入口，那些镜像要与没有 CSR 状态的参考模型逐条比较，在其中加入 `csrw` 会把它们推出该层的适用范围。需要 trap 的 Guest 链接 `mtrap_entry.S`。

### 统一 trap 路径

```
设备线 / 指令故障
      │
      ▼
__yan_guest_trap_vector        （CPU 已清零 MIE，因此不会嵌套）
      │  压入固定帧，保存调用者保存寄存器与被中断的 sp
      │  csrr a0/mcause, a1/mepc, a2/mtval
      ▼
yan_guest_trap_dispatch(cause, epc, tval)
      │
      ▼
已安装的 handler(cause, epc, tval) ──► 返回“继续执行的 PC”
      │
      ▼
csrw mepc, a0 → 恢复寄存器与 sp → mret
```

| 项 | 约定 |
| --- | --- |
| 帧 | 20 字（80 字节）压在**被中断的栈**上；被中断的 sp 存在帧内，等于帧顶 |
| 对齐 | 80 字节使 `sp` 保持 16 字节对齐，满足帧下方 `call` 的 RV32 psABI 要求 |
| 保存集合 | 调用者保存寄存器：`ra`、`t0`–`t2`、`a0`–`a7`、`t3`–`t6`，共 16 个，外加被中断的 `sp` |
| 未保存 | `s0`–`s11` 由 C 调度器的调用约定保证不变，向量无需保存 |
| 恢复 | 先恢复全部寄存器，最后 `lw sp, ...` 一并弹帧 |
| 返回 | handler 的返回值写入 `mepc`；`mret` 后再由 CPU 恢复 `mstatus.MIE = MPIE` |
| 嵌套 | 不支持：入口时 CPU 已清 `MIE`，且本环境不重开 |

原因码：同步异常用特权规范的原因码；中断在最高位置 1，低位是同一套编号（MSIP 3、MTIP 7、MEIP 11）。因此一个处理程序只需比较一个 32 位值就能区分全部来源。

中断的“确认”留给处理程序：MSIP 清 `msip`，MTIP 把 `mtimecmp` 推远，MEIP claim/complete。框架只提供设备访问封装，不替处理程序决定策略——这是 Guest 代码该做的判断。

### Guest 可见的设备与 CSR

`tests/guest/guest_devices.h` 给出 Guest 侧设备映射与 32 位字访问封装：`msip`、`mtimecmp`、`mtime` 的高低半字读写，PLIC 的 priority / enable / threshold / pending / claim / complete。访问宽度固定为 32 位：设备只回答字访问，用字节或半字访问会被 CPU 转成访存故障。

`tests/guest/mtrap.h` 给出 CSR 访问（`mstatus` / `mie` / `mip` / `mtvec`）、中断使能、以及触发各类陷阱的辅助函数。

Host 与 Guest 分别编译，Guest 不能包含 Host 的 `include/yan/interrupt.h`（它会带进 Host 类型），所以设备映射与 CSR 编号在 Guest 侧**重述**了一次。重述由 `tests/test_boot.c` 逐条与 Host 常量比对：任何一处漂移都会让 Host 测试失败。这是刻意的取舍——宁可保留一份被测试锁住的副本，也不让 Guest 代码依赖 Host 头文件。

### 结果上报

`tohost` 的值 1 表示“到达最终检查”，其他非零值是失败码。检查通过一个包装函数上报，它把失败码的最高位置 1，因此**任何检查编号都不会被读成通过**（0 与 1 都不行）。这条约束由 Guest 自检自身保证，不依赖编号约定。

### ACT4 接入评估

需要处理三件事，逐条给出结论。

**一、CLINT 地址宏：已实现。** `tests/act4/rvmodel_macros.h` 现在声明 `RVMODEL_MSIP_ADDRESS`、`RVMODEL_MTIMECMP_ADDRESS`、`RVMODEL_MTIME_ADDRESS`，基址 `YANOS_CLINT_BASE_ADDRESS = 0x02000000`，偏移与 Host 的 `include/yan/interrupt.h` 一致。注意框架在 `sail_macros.h` 里 `#undef` 并以 `SAIL_*` 值替换这些宏，所以声明**不改变生成代码**；它改变的是可检测性：`run_act4.sh` 现在汇编一段探针，若框架的有效地址与 DUT 声明的地址不一致（含宏未定义）就直接失败，不再是静默指向别的寄存器。

**二、外部中断：不可实现，保留显式 SKIP。** 框架的 `RVMODEL_SET_MEXT_INT` 写的是 Sail 的测试中断发生器设备（`SAIL_SIG_ADDRESS`），而 YanOS 的 PLIC 没有 raise 寄存器，只有 Host 能通过 `yan_plic_raise()` 拉起一条源线。要让 Guest 自己拉起外部源就得新增一个设备，属于本阶段明确排除的范围。因此这两个宏保持 `.error`，语料检查继续拒绝调用它们的用例。

**三、陷阱处理器与中断助手：仍被 `STANDARD_SM_SUPPORTED` 挡住。** 框架把陷阱处理器的实例化与整套 M-mode 中断助手（`rvtest_set_mtime_int_m`、`rvtest_set_msw_int_m` 等）都放在 `#ifdef STANDARD_SM_SUPPORTED` 之内，而该开关的前提是 `medeleg` / `mideleg` 委托、PMP 与 S-mode。`mie` / `mip` / CLINT 现在已经存在，但不是它需要的东西。结论：`tests/rv32i/I` 与 `tests/rv32i/M` 中依赖陷阱处理器的 7 个分支 / 跳转用例继续报 SKIP，并在 `run_act4.sh` 的输出里写明原因，绝不报成通过。

**四、语料本身没有中断用例。** 选中的 `tests/rv32i/I` 与 `tests/rv32i/M` 中，调用 `RVMODEL_SET/CLR_MEXT_INT`、`RVMODEL_SET/CLR_MSW_INT`、`RVMODEL_MSIP_ADDRESS` 或 `RVMODEL_MTIME_ADDRESS` 的测试**数量为零**。因此本阶段能交付的是**接口**（设备映射、启动与陷阱路径、可运行的 Guest 自检），而不是“ACT4 中断用例通过”；等语料提供中断用例、且平台补上委托与 PMP 之后，这层才可能增加通过数。

### 明确不在范围内

单 hart、仅 M-mode；无 S/U 模式、无委托、无 `mtvec` 向量模式、无中断嵌套、无 PMP；不新增设备；不是操作系统（无调度、无进程、无系统调用 ABI、无页表）。本规格不声明完整的 RISC-V 特权架构符合性。

### 规范依据

- 机器模式 CSR、原因码、`mret` 语义：[特权架构 20240411，Machine-Level ISA 1.13](https://docs.riscv.org/reference/isa/v20240411/_attachments/riscv-privileged.pdf)。
- RV32 调用约定与 16 字节栈对齐：[RISC-V psABI](https://github.com/riscv-non-isa/riscv-elf-psabi-doc)。
- 框架的启动、陷阱与中断助手位置：[riscv-arch-test](https://github.com/riscv/riscv-arch-test) 的 `tests/env/riscv_arch_test.h`、`rvtest_setup.h`、`sail_macros.h`、`check_defines.h`。
- 设备语义沿用[机器模式中断规格](0012-machine-interrupts.md)，本阶段不改变 Bus / CPU / Device 分层。

## IMPLE PLAN

1. 先写 `tests/guest/trap_env.c`、`tests/guest/run_trap_env.sh` 与 `tests/test_boot.c` 并注册到 CTest，确认在实现前失败。
2. 实现 `mtrap_frame.h`、`mtrap.h` / `mtrap.c`、`mtrap_entry.S`、`guest_devices.h`。
3. 把 CLINT / PLIC 接入 `yan_run` 的平台映射（每步推进一次 `mtime`），使 Guest 程序能真正访问设备；差分执行器保持不连接设备。
4. 完成 ACT4 的最小宏适配与地址探针。
5. 运行 Debug、ASan/UBSan、Release 与外部层 CTest，更新 STATUS 与相关规格。

## VERIFY

- **Host 层**（`tests/test_boot.c`，始终运行，无需交叉工具链）：Guest 与 Host 的设备常量、CSR 编号、`mie` / `mstatus` 位逐条一致；陷阱帧的 17 个槽位互不重叠、帧为 16 字节倍数；CPU 执行 Guest 的 `sw` / `lw` 能读写 CLINT 与 PLIC；启动契约（复位无向量、`csrw mtvec` 只保留 31:2、`mstatus` 掩码行为）成立。
- **Guest 层**（`tests/guest/run_trap_env.sh` → `yan_run`）：启动状态、向量安装、ECALL / EBREAK / 非法指令 / 非对齐访存四种同步异常走同一路径、MSIP 与 MTIP 中断进入与返回、调用者保存寄存器跨陷阱不变、被中断指令按 handler 的决定重放或跳过、CLINT 与 PLIC 的 Guest 访问。
- **变异检查**：在副本里注入 10 个缺陷（不清 MSIP、不 disarm MTIP、MTIP 未使能、不改 `mepc`、不装 `mtvec`、帧只压 4 字节、帧槽位与 `a4` 重叠、错误 CLINT 基址、错误 PLIC enable 偏移、`mtimecmp` 只写低半字），Guest 自检与 Host 测试对每一个都报告失败，没有存活者。第一次运行还暴露出自检自身的漏洞：失败码 1 与“通过”的 `tohost = 1` 撞车，使 `no-mtvec` 变异体存活；修正为失败码最高位置 1 后该变异体被检出。
- **ACT4**：CLINT 地址探针通过；语料结果与改动前一致（40 匹配 / 0 不同 / 7 SKIP），SKIP 原因写明需要陷阱处理器。
- 命令：Debug、`-DYAN_ENABLE_SANITIZERS=ON`、Release 三套 `ctest --output-on-failure`，以及在依赖可用时的外部层全量 CTest。

实现、验证与审查状态见 [STATUS](../STATUS.md)。
