# 项目状态

## 当前阶段

[0034](specs/0034-ci-guest-tests-and-status-archive.md) 已批准（2026-10-19）。R1–R5 已随 [PR #43](https://github.com/YanBo-maker/YanOS/pull/43) 合入 main（41b83b5，提交 e483bf9）。两个 ubuntu-22.04 任务用 apt 安装 `gcc-riscv64-linux-gnu`，并以 `-DYAN_BUILD_TOOLS=ON -DYAN_RISCV_PREFIX=riscv64-linux-gnu-` 配置。`YAN_RISCV_GCC` 不是可执行路径，或 `ctest -N -R '^guest_'` 为 0 时，任务在测试前失败。Linux 的 ctest 输出经 `tee` 记入日志，出现 `(Skipped)` 则失败。windows-2022 仍只做 Debug，配置、构建和 `ctest` 命令与 PR #43 之前相同，不安装 RISC-V 工具链。

合入后 main 的 [Actions 39339094819](https://github.com/YanBo-maker/YanOS/actions/runs/39339094819) 三项成功：Linux Debug+ASan 与 Linux Release 各 108 项、0 失败，`^guest_` 各 31；Windows Debug 70 项、0 失败。安装包为 `gcc-11-riscv64-linux-gnu 11.4.0-1ubuntu1~22.04cross1`。该次运行的配置日志写明差分测试未注册（参考模型路径为空）。`YAN_ENABLE_MUTATION_TESTS` 与 `YAN_BUILD_SEMANTIC` 不在这套 CI 里。

[0033](specs/0033-block-mcp-navigation.md) 的四个只读块工具已随 [PR #38](https://github.com/YanBo-maker/YanOS/pull/38) 合入 main（75e057a）。各阶段验证明细在对应规格的 VERIFY 执行记录，不在本页展开。

## 已交付能力

- 0001 项目说明与开发流程。[0001](specs/0001-project-foundation.md)，[PR #1](https://github.com/YanBo-maker/YanOS/pull/1)（5918c0d）
- 0003 Machine、Bus 与 RAM。[0003](specs/0003-machine-bus-ram.md)，[PR #3](https://github.com/YanBo-maker/YanOS/pull/3)（d3649a6）
- 0004 CPU 状态与取指。[0004](specs/0004-cpu-state-fetch.md)，[PR #4](https://github.com/YanBo-maker/YanOS/pull/4)（1496681）
- 0005 ADDI 与单步执行。[0005](specs/0005-addi-step.md)，[PR #5](https://github.com/YanBo-maker/YanOS/pull/5)（01731bf）
- 0006 RV32I 整数计算。[0006](specs/0006-integer-alu.md)，[PR #6](https://github.com/YanBo-maker/YanOS/pull/6)（6fc90d4）
- 0007 条件分支与 JAL/JALR。[0007](specs/0007-control-flow.md)，[PR #7](https://github.com/YanBo-maker/YanOS/pull/7)（d6f71ae）
- 0008 Load / Store。[0008](specs/0008-load-store.md)，[PR #8](https://github.com/YanBo-maker/YanOS/pull/8)（dab770e）
- 0009 M-mode 同步异常。[0009](specs/0009-guest-traps.md)，[PR #9](https://github.com/YanBo-maker/YanOS/pull/9)（7e21bd3）
- 0010 RV32M 乘除。[0010](specs/0010-m-extension.md)，[PR #10](https://github.com/YanBo-maker/YanOS/pull/10)（0bb3c61）
- 0011 架构差分与官方测试验证层。[0011](specs/0011-cpu-validation.md)，[PR #12](https://github.com/YanBo-maker/YanOS/pull/12)（6573081）
- 0012 机器模式中断与 CLINT。[0012](specs/0012-machine-interrupts.md)，[PR #12](https://github.com/YanBo-maker/YanOS/pull/12)（6573081）
- 0013 Guest 启动与统一 trap 环境。[0013](specs/0013-guest-trap-environment.md)，[PR #12](https://github.com/YanBo-maker/YanOS/pull/12)（6573081）
- 0014 Host 传输通道。[0014](specs/0014-host-transport-channel.md)，[PR #13](https://github.com/YanBo-maker/YanOS/pull/13)（03f5b80）
- 0015 UART。[0015](specs/0015-uart-device.md)，[PR #13](https://github.com/YanBo-maker/YanOS/pull/13)（03f5b80）
- 0016 PLIC 电平网关与设备中断线。[0016](specs/0016-plic-gateway-and-irq-lines.md)，[PR #13](https://github.com/YanBo-maker/YanOS/pull/13)（03f5b80）
- 0017 控制台与 `os/` 分层。[0017](specs/0017-console-and-os-layout.md)，[PR #13](https://github.com/YanBo-maker/YanOS/pull/13)（03f5b80）
- 0018 块请求协议。[0018](specs/0018-block-protocol.md)，[PR #15](https://github.com/YanBo-maker/YanOS/pull/15)（65d22bb）
- 0019 协作式任务运行时。[0019](specs/0019-cooperative-runtime.md)，[PR #15](https://github.com/YanBo-maker/YanOS/pull/15)（65d22bb）
- 0020 持久化块镜像。[0020](specs/0020-persistent-block-image.md)，[PR #16](https://github.com/YanBo-maker/YanOS/pull/16)（dc7f8fe）
- 0021 YanFS 单根目录。[0021](specs/0021-yanfs.md)，[PR #16](https://github.com/YanBo-maker/YanOS/pull/16)（dc7f8fe）
- 0022 Guest 终端文件操作。[0022](specs/0022-terminal-file-operations.md)，[PR #16](https://github.com/YanBo-maker/YanOS/pull/16)（dc7f8fe）
- 0023 多行文本编辑。[0023](specs/0023-multiline-text-editor.md)，[PR #16](https://github.com/YanBo-maker/YanOS/pull/16)（dc7f8fe）
- 0024 重命名与独立复制。[0024](specs/0024-file-rename-copy.md)，[PR #18](https://github.com/YanBo-maker/YanOS/pull/18)（9ab2488）
- 0025 字面检索。[0025](specs/0025-knowledge-search.md)，[PR #20](https://github.com/YanBo-maker/YanOS/pull/20)（9b8a01f）
- 0026 词项搜索与可重建索引。[0026](specs/0026-term-search-index.md)，[PR #22](https://github.com/YanBo-maker/YanOS/pull/22)（c84b2e7）
- 0027 笔记元数据与标签筛选。[0027](specs/0027-note-metadata.md)，[PR #24](https://github.com/YanBo-maker/YanOS/pull/24)（7451c85）
- 0028 知识关系导航。[0028](specs/0028-knowledge-relations.md)，[PR #26](https://github.com/YanBo-maker/YanOS/pull/26)（8349b7d）
- 0029 Host 语义检索。[0029](specs/0029-host-semantic-search.md)，[PR #30](https://github.com/YanBo-maker/YanOS/pull/30)（79fdde6）
- 0030 可核验检索上下文包。[0030](specs/0030-retrieval-context-json.md)，[PR #32](https://github.com/YanBo-maker/YanOS/pull/32)（5170cf0）
- 0031 只读 MCP 检索入口。[0031](specs/0031-mcp-retrieval-tools.md)，[PR #34](https://github.com/YanBo-maker/YanOS/pull/34)（a9a1e9c）
- 0032 具名块与精确正文引用。[0032](specs/0032-block-references.md)，[PR #36](https://github.com/YanBo-maker/YanOS/pull/36)（d8804be）
- 0033 Agent 只读块导航。[0033](specs/0033-block-mcp-navigation.md)，[PR #38](https://github.com/YanBo-maker/YanOS/pull/38)（75e057a）
- 0034 Linux CI 编译并运行 Guest 默认测试。[0034](specs/0034-ci-guest-tests-and-status-archive.md)，[PR #43](https://github.com/YanBo-maker/YanOS/pull/43)（41b83b5）

## 覆盖边界与已知限制

- CI 矩阵与失败条件见 `.github/workflows/build.yml`：ubuntu-22.04 Debug+ASan、ubuntu-22.04 Release 安装 `gcc-riscv64-linux-gnu` 并打开 `YAN_BUILD_TOOLS`；工具链缺失、Guest 测试未注册、或日志中有 `(Skipped)` 时 Linux 任务失败。windows-2022 Debug 不装该工具链，也不跑 Guest 测试。
- 外部验证层仍不在 CI：NEMU 差分（`difftest_*`）、官方 riscv-tests、Sail 签名、ACT4。依赖缺失时这些测试不注册，或按原脚本返回 SKIP，不记为通过。历史测量在归档的「外部验证层」。
- 差分比较 PC、32 个通用寄存器和结束时的整段 RAM。归档记录的 NEMU riscv32 参考状态没有 CSR，因此 trap、MRET 与中断语义不在差分范围内；`yan_difftest` 不连接 CLINT / PLIC。`yan_run` 映射四个设备窗口并每条指令推进一次 `mtime`。
- 第三方证据来自官方 riscv-tests 与 Sail 签名，范围是用户态 RV32IM。Guest trap 自检覆盖 MSIP 与 MTIP；MEIP 由 Host 侧测试驱动。
- `tests/official/run_act4.sh` 在 Sail 与 DUT 都没有签名时把用例记为 skipped；结束条件仍是 differed 为 0 且 matched 大于 0。跳过数大于 0 本身不使脚本失败。
- CI 不打开 `YAN_ENABLE_MUTATION_TESTS`。默认套件里的 `*_mutation_gate` 纯门禁仍会注册。`guest_console_mutation_gate` 只在变异配置打开时注册（`CMakeLists.txt` 中该门禁与 `YAN_ENABLE_MUTATION_TESTS` 同支）。
- CI 不打开 `YAN_BUILD_SEMANTIC`，因此 `yan_mcp`、`mcp_*`、`semantic_*` 不注册。`guest_note_blocks_host` 还要求已有 `yan_mcp` 目标（`CMakeLists.txt` 中 `if(TARGET yan_mcp)`），本次 CI 名单里没有它。
- 没有 S/U 模式、`medeleg` / `mideleg`、虚拟内存分页和抢占式调度。YanFS 初版仍是单根目录、最多 63 个文件、连续 extent；元数据写失败可能拒挂，不保证断电一致性。

## 待定设计

- 后续 RISC-V 指令、特权机制与设备的分阶段支持范围。
- ACT4 自带构建系统、UDB 配置与框架生成 RVMODEL 的接入方式。
- YanFS 的后续格式、事务恢复与层次目录。初版语义已由 0021 锁定。
- 项目许可证。

## 下一步

[决策 0001](decisions/0001-roadmap-A-D-B.md) 的 A 与 D 的 CI 部分已经落在 main。下一步是 B：写入类 MCP。C（U-mode / syscall）暂缓。

## 历史归档

拆分前的全文在 [STATUS-history.md](STATUS-history.md)。此后替换「当前阶段」时，把旧段落原样追加到该文件末尾。
