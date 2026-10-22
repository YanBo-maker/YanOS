# 项目状态

## 当前阶段

当前按 [0035](specs/0035-agent-writable-knowledge.md) R8–R11 验证 dsh 的真实双库使用：从普通库检索并读取原文，向学习库预览记录，经闫博确认后提交，再重启 MCP 和通过生产 Guest 终端读回。学习记录保留空的 `known`；保存确认不代表确认学会。

检索、原文读取、预览和确认后的保存均已实测，重启与 Guest 读回和普通库隔离已由总审核核对。初次消费者在预览前异常退出，同一会话唯一一次重试成功；原因未确定，不能据此宣称客户端稳定性已经充分验证。接入说明与执行边界已由原独立只读 dsh 会话核对，总审核已完成本地收口；本轮文档尚未发布。

前一阶段学习笔记联合验收已发布为 [PR #85](https://github.com/YanBo-maker/YanOS/pull/85)，精确 head `30916a6` 的远端检查及原始日志已核对通过，尚未合入；0035 整体仍为 Approved。本分支基于该 head，不包含仍待合入的 [0036](specs/0036-yanfs-multi-block-directory.md) R4/R6（[PR #81](https://github.com/YanBo-maker/YanOS/pull/81)）和 R7（[PR #82](https://github.com/YanBo-maker/YanOS/pull/82)）。执行记录见 0035 的 VERIFY，操作入口见 [dsh 双库接入](dsh-mcp-quickstart.md)。

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
- 0035 镜像旁锁、原子写，以及 MCP 写工具 `yanos_note_create` / `yanos_note_replace`。[0035](specs/0035-agent-writable-knowledge.md)，[PR #52](https://github.com/YanBo-maker/YanOS/pull/52)（4af3cfd）、[PR #55](https://github.com/YanBo-maker/YanOS/pull/55)（2921fed）
- 0035 dsh 双库真实试用已在独立样本镜像完成，经确认保存记录并由重启 MCP 与生产 Guest 读回；接入文档已完成独立对齐。[接入说明](dsh-mcp-quickstart.md)

## 覆盖边界与已知限制

- CI 矩阵与失败条件见 `.github/workflows/build.yml`：ubuntu-22.04 Debug+ASan、ubuntu-22.04 Release 安装 `gcc-riscv64-linux-gnu` 并打开 `YAN_BUILD_TOOLS`；工具链缺失、Guest 测试未注册、或日志中有 `(Skipped)` 时 Linux 任务失败。windows-2022 Debug 不装该工具链，也不跑 Guest 测试。
- 外部验证层仍不在 CI：NEMU 差分（`difftest_*`）、官方 riscv-tests、Sail 签名、ACT4。依赖缺失时这些测试不注册，或按原脚本返回 SKIP，不记为通过。历史测量在归档的「外部验证层」。
- 差分比较 PC、32 个通用寄存器和结束时的整段 RAM。归档记录的 NEMU riscv32 参考状态没有 CSR，因此 trap、MRET 与中断语义不在差分范围内；`yan_difftest` 不连接 CLINT / PLIC。`yan_run` 映射四个设备窗口并每条指令推进一次 `mtime`。
- 第三方证据来自官方 riscv-tests 与 Sail 签名，范围是用户态 RV32IM。Guest trap 自检覆盖 MSIP 与 MTIP；MEIP 由 Host 侧测试驱动。
- `tests/official/run_act4.sh` 在 Sail 与 DUT 都没有签名时把用例记为 skipped；结束条件仍是 differed 为 0 且 matched 大于 0。跳过数大于 0 本身不使脚本失败。
- CI 不打开 `YAN_ENABLE_MUTATION_TESTS`。默认套件里的 `*_mutation_gate` 纯门禁仍会注册。`guest_console_mutation_gate` 只在变异配置打开时注册（`CMakeLists.txt` 中该门禁与 `YAN_ENABLE_MUTATION_TESTS` 同支）。
- Linux CI（`.github/workflows/build.yml`，2921fed）打开 `YAN_BUILD_SEMANTIC`，因此会构建 `yan_mcp`。`CMakeLists.txt` 在该开关下注册 `mcp_protocol`、`mcp_note_tool`、`mcp_stdio`、`mcp_sdk`、`mcp_faults` 等测试；`mcp_note` 还要求已有 `yan_mcp`、`yan_run`、`yanfs_terminal` 和 `note_read_probe`。workflow 在配置后检查 `mcp_note`、`mcp_note_tool`、`mcp_stdio`、`mcp_sdk` 四项都已注册。`guest_note_blocks_host` 写在 `if(TARGET yan_mcp)` 里，Linux 打开该开关后满足条件。windows-2022 的配置不传 `YAN_BUILD_SEMANTIC`（默认 OFF），不构建 `yan_mcp`。不依赖该开关的 Host 侧测试仍会注册。`YAN_ENABLE_MUTATION_TESTS` 仍然不打开。
- 没有 S/U 模式、`medeleg` / `mideleg`、虚拟内存分页和抢占式调度。YanFS 仍是单根目录、连续 extent；v1 最多 63 个文件，v2（0036）最多 4095 个，终端和 Host 工具按容量分配后都可挂载，但语义/MCP 检索在 R6（PR4）之前对 slot ≥ 63 的文件返回 CORRUPT。元数据写失败可能拒挂，不保证断电一致性：v1 一次目录更新是一次块写入，v2 改动块 0 以外的目录项时是两次（先写该块，再写块 0 的 CRC），两次之间断电会让镜像 CRC 不符而被拒挂。

- 学习笔记联合验收只在临时双镜像上模拟人工确认；它核对原文、引用位置、镜像隔离和持久化，不能判断项目所有者是否学会，也不能保证任意 Agent 都遵守确认约束。`guest_learning_note` 在 Linux 工具、semantic/MCP 与 Guest 目标齐备时随默认套件注册；Windows 只运行纯 Python 的 `learning_note_gate`。PR #85 精确 head `30916a6` 的远端 CI 已核对通过；实际 dsh 试用单独记录在 0035 VERIFY，不算默认 CI 覆盖。

## 待定设计

- 后续 RISC-V 指令、特权机制与设备的分阶段支持范围。
- ACT4 自带构建系统、UDB 配置与框架生成 RVMODEL 的接入方式。
- YanFS 的后续格式、事务恢复与层次目录。初版语义已由 0021 锁定。
- 项目许可证。

## 下一步

[决策 0001](decisions/0001-roadmap-A-D-B.md) 的 A 与 D 的 CI 部分已经落在 main。B 的写工具代码已在 main（见当前阶段）。C（U-mode / syscall）仍暂缓。0035 的规格状态仍是 Approved。

## 历史归档

拆分前的全文在 [STATUS-history.md](STATUS-history.md)。此后替换「当前阶段」时，把旧段落原样追加到该文件末尾。
