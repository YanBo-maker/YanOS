# 规格索引

本表回答「现在哪份 spec 定义了这个行为」。状态用 Draft、Approved、Implemented、Superseded。文件里没有状态字段、且能力已经在 main 上的旧规格，按事实填 Implemented；不回写各 spec 文件的头。

| 编号 | 标题 | 状态 | supersedes | 被谁取代 |
| --- | --- | --- | --- | --- |
| [0001](0001-project-foundation.md) | 项目基础 | Implemented | 无 | 无 |
| 0002 | 未入库，见 PR #2，已取代 | Superseded | 无 | 后续已入库规格 |
| [0003](0003-machine-bus-ram.md) | Machine、Bus 与 RAM | Implemented | 无 | 无 |
| [0004](0004-cpu-state-fetch.md) | CPU 状态与取指 | Implemented | 无 | 无 |
| [0005](0005-addi-step.md) | ADDI 与单步执行 | Implemented | 无 | 无 |
| [0006](0006-integer-alu.md) | RV32I 整数计算 | Implemented | 无 | 无 |
| [0007](0007-control-flow.md) | 条件分支与跳转 | Implemented | 无 | 无 |
| [0008](0008-load-store.md) | Load / Store | Implemented | 无 | 无 |
| [0009](0009-guest-traps.md) | M-mode 同步异常 | Implemented | 无 | 无 |
| [0010](0010-m-extension.md) | M 扩展规格 | Implemented | 无 | 无 |
| [0011](0011-cpu-validation.md) | CPU 验证规格 | Implemented | 无 | 无 |
| [0012](0012-machine-interrupts.md) | 机器模式中断 | Implemented | 无 | [0016](0016-plic-gateway-and-irq-lines.md)（仅 PLIC 网关简化约定与 `yan_plic_raise`；其余继续有效） |
| [0013](0013-guest-trap-environment.md) | Guest 启动与统一 trap 环境 | Implemented | 无 | 无 |
| [0014](0014-host-transport-channel.md) | Host 传输通道 | Implemented | 无 | 无 |
| [0015](0015-uart-device.md) | UART 字符设备 | Implemented | 无 | 无 |
| [0016](0016-plic-gateway-and-irq-lines.md) | PLIC 网关与设备中断线 | Implemented | [0012](0012-machine-interrupts.md) 的 PLIC 网关简化约定与 `yan_plic_raise` | 无 |
| [0017](0017-console-and-os-layout.md) | 控制台与 `os/` 层 | Implemented | 无 | 无 |
| [0018](0018-block-protocol.md) | 块请求协议 | Implemented | 无 | 无 |
| [0019](0019-cooperative-runtime.md) | 协作式运行时 | Implemented | 无 | 无 |
| [0020](0020-persistent-block-image.md) | 持久化块镜像 | Implemented | 无 | 无 |
| [0021](0021-yanfs.md) | YanFS | Implemented | 无 | 无 |
| [0022](0022-terminal-file-operations.md) | Guest 终端文件操作 | Implemented | 无 | 无 |
| [0023](0023-multiline-text-editor.md) | 多行文本编辑 | Implemented | 无 | 无 |
| [0024](0024-file-rename-copy.md) | 文件重命名与复制 | Implemented | 无 | 无 |
| [0025](0025-knowledge-search.md) | 知识检索基础设施 | Implemented | 无 | 无 |
| [0026](0026-term-search-index.md) | 词项搜索与可重建索引 | Implemented | 无 | 无 |
| [0027](0027-note-metadata.md) | 笔记元数据与标签筛选 | Implemented | 无 | 无 |
| [0028](0028-knowledge-relations.md) | 知识关系导航 | Implemented | 无 | [0036-R4](0036-yanfs-multi-block-directory.md#0036-r4上层服务在大容量下行为正确)（仅容量与 backlinks 输出） |
| [0029](0029-host-semantic-search.md) | Host侧语义检索 | Implemented | 无 | [0036-R6](0036-yanfs-multi-block-directory.md#0036-r6host-语义语料)（仅文件容量与 skipped 边界） |
| [0030](0030-retrieval-context-json.md) | 可核验的检索上下文包 | Implemented | 无 | 无 |
| [0031](0031-mcp-retrieval-tools.md) | 面向Agent的只读MCP检索工具 | Implemented | 无 | 无 |
| [0032](0032-block-references.md) | 具名块与精确正文引用 | Implemented | 无 | 无 |
| [0033](0033-block-mcp-navigation.md) | Agent 的只读块导航 | Implemented | 无 | [0036-R4/R6](0036-yanfs-multi-block-directory.md#0036-r4上层服务在大容量下行为正确)（仅文件容量） |
| [0034](0034-ci-guest-tests-and-status-archive.md) | CI 运行 Guest 默认测试与 STATUS 归档 | Implemented | 无 | 无 |
| [0035](0035-agent-writable-knowledge.md) | Agent 可写的知识工具 | Approved | 无 | 无 |
| [0036](0036-yanfs-multi-block-directory.md) | YanFS 多块目录 | Approved | 无整份替代；局部扩展见规格头 | 无 |
