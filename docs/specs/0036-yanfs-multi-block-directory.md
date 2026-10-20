# 0036：YanFS 多块目录（放宽 63 个文件上限）

- **意图**：一个镜像能存放远多于 63 篇笔记。目录从 1 块扩展为 mkfs 时确定的 D 块：默认 1023 个文件（D=16），最多 4095 个（D=64）。Guest 按镜像的实际容量分配内存，不按硬上限。文件名、引用语法、MCP 工具都不变；旧镜像继续可用，离线迁移后库身份不变。依据：[0003 决策记录](../decisions/0003-file-limit-and-images.md)（闫博 2026-10-20 裁定）。
- **状态**：Approved（闫博 2026-10-20）
- **supersedes**：无。扩展 [0021](0021-yanfs.md) 的盘上格式（新增 version 2），v1 定义不变。0035-R3-AC3 中「目录已满 63 项」按本规格理解为「目录已满（达到该镜像的容量）」。
- **成功标准**（从闫博的角度看）：
  1. 新建镜像默认就能放 1023 篇笔记；需要时可以在 mkfs 时调大或调小，最多 4095 篇。
  2. 笔记多了以后，终端、搜索、关系、块引用、MCP 照常可用，结果正确；超出索引容量时自动退回全量扫描，不会静默漏结果。
  3. 现有镜像不用处理就能照常使用；需要扩容时用一条命令复制出新镜像，库身份和已建索引都保持有效。
  4. 小镜像不会因为支持 4095 个文件而多占内存。
  5. 笔记名和 `[[NAME#ID]]` 的写法与以前完全相同。

## 代码核实（origin/main `41b83b5`）

### 63 的来源

`os/yanfs.h:28` 定义 `YAN_FS_MAX_FILES 63`。0021 规定目录只占块 0：64 B 头加 63 个 64 B 目录项（`docs/specs/0021-yanfs.md:31,34`）。格式化时写入 63（`os/yanfs.c:149`），挂载时要求恰好是 63（`os/yanfs.c:318`）。Guest 用 `os/yanfs.h:83` 的 `metadata[4096]` 缓存这一块目录。

### Guest 有没有动态分配

**没有。** `os/` 和 `apps/` 里没有 malloc、arena 或 sbrk。所有大对象都是 `apps/yanfs_terminal/main.c:109-133` 中的 static 对象。`os/guest.ld` 只有 text/rodata/data/bss/tohost，没有堆符号。RAM 布局如下：

- `yan_run` 默认 RAM 为 16 MiB（`tools/yan_run.c:122`）。
- 环形缓冲区固定在 RAM 顶部 16 KiB（`tools/yan_run.c:33`）。
- 目前 bss 约 1.7 MB（STATUS 实测 1735792 B）。

所以 bss 之后到环形缓冲区之间有十几 MiB 空闲，但 Guest 没有办法使用它。

本规格的做法见 0036-R3：一次性 bump arena。

- `os/guest.ld` 在 .bss 之后导出 `__yan_arena_start`，并定义 `__yan_arena_end = 0x80000000 + 32 MiB`（`yan_run` 默认 RAM 改为 64 MiB，环形缓冲区在 RAM 顶部 0x83FFC000，闫博 2026-10-20 裁定）。
- `yan_run` 加载 ELF 时检查 `__yan_arena_end ≤ 环形缓冲区基址`，不满足就拒绝启动。
- arena 只分配不释放。启动时先读目录头得到容量 N，再按 N 一次性分配所有与文件数有关的存储，此后不再分配。
- Host 侧的工具和测试改用 calloc 分配同样大小的内存。

### 内存：逐项计算

每文件字节数取自用 `gcc -std=c17` 在 Host 上对该提交的头文件做的 `sizeof`。这几项都是固定宽度字段（`char[32]` + `u32`），RV32 上大小相同。各 context 的现有值见 STATUS（Host/RV32 相差 16 B 以内）。

| # | 位置 | 每文件 | 63（现状） | 1023 | 2047 | 4095 |
|---|---|---|---|---|---|---|
| 0 | 目录缓存 `YanFs.metadata`（`os/yanfs.h:83`），共 (N+1)×64 B | 64 | 4 096 | 65 536（+61 440） | 131 072（+126 976） | 262 144（+258 048） |
| 1 | `YanNoteContext.entries`（`os/note_metadata.h:88`），`YanNoteEntry` 36 B | 36 | 2 268 | 36 828 | 73 692 | 147 420 |
| 2 | `YanNoteContext.sort_name`（`os/note_metadata.h:89`） | 32 | 2 016 | 32 736 | 65 504 | 131 040 |
| 3 | `YanRelationContext.rows`（`os/note_relations.h:57,116`），`YanRelationRow` 36 B | 36 | 2 268 | 36 828 | 73 692 | 147 420 |
| 4 | `YanSearchTermsLinear.metadata_eligible`（`os/search_terms_linear.h:69`），有两份：`search_linear` 一份，`term_index.linear` 内嵌一份 | 1×2 | 126 | 2 046 | 4 094 | 8 190 |
| 5 | `YanSearchTermsIndex.entries`（`os/search_terms_index.h:101`），`YanFsInfo` 36 B | 36 | 2 268 | 36 828 | 73 692 | 147 420 |
| 6 | `YanBlockContext.names`（`os/note_blocks.h:56`） | 32 | 2 016 | 32 736 | 65 504 | 131 040 |
| 7 | Host：`YanSemanticCorpus.files`（`host/semantic_corpus.h:48`，312 B/项），另有两个栈上数组 `YanSemanticFile files[63]`（`host/semantic_corpus.c:109,202`，40 B/项） | 312+80 | 24 696 | 401 016 | 802 424 | 1 605 240 |
| 8 | `allocate_extent` 的 `used_start/used_count`（`os/yanfs.c:442-443`），从栈移到 arena scratch（R5） | 8 | 504（栈上） | 8 184 | 16 376 | 32 760 |
| — | Guest 合计 #0–#6 + #8 | 246 | 15 562 | 251 722（**+236 160 ≈ +231 KiB**） | 503 626（**+488 064 ≈ +477 KiB**） | 1 007 434（**+991 872 ≈ +969 KiB**） |

与现有预算对比（RV32 值）：

- **每个 context ≤ 64 KiB**（`os/note_metadata.h:93`、`os/note_relations.h:120`、`os/note_blocks.h:66` 的静态断言）。如果把数组照旧留在 context 里，N=1023 时 note context 达到 30 536 + 960×68 = 95 816 B，**会超**；N=2047 时 block context 达到 26 424 + 1 984×32 = 89 912 B，**也会超**。所以本规格把 #1–#6 从 context 里移出、改为 arena 指针。context 自身随之缩小（note 约 26 252 B，relation 约 9 876 B，block 约 24 408 B），三个断言继续成立。
- **五个 context（index/note/shell/relation/block）合计**（旧规则 ≤ 2 MiB，已由 Guest 整体 32 MiB 预算取代，见实施澄清 3）：现为 1 645 488 B。把数组按容量计入后：N=1023 时为 1 645 488 + 960×173 = **1 811 568 B**，余量 285 584 B；N=2047 时为 1 645 488 + 1 984×173 = **1 988 720 B**，余量 108 432 B；N=4095 时为 1 645 488 + 4 032×173 = **2 343 024 B**，超出旧的 2 MiB，但在 32 MiB Guest 预算内。（173 = #1+#2+#3+#5+#6，再加上 index 内嵌的那一份 #4；目录缓存和 `search_linear` 原本就不计入五项合计，这里另列在 #0 和 #4。）
- **4 KiB 任务栈**：`allocate_extent` 在栈上开了 `used_start/used_count[YAN_FS_MAX_FILES]`（`os/yanfs.c:442-443`），占 8×N B。N=63 时 504 B，N=1023 时 8 184 B，**超过任务栈**。必须移进 YanFs 的 arena scratch（0036-R5）。

### 硬上限定为 4095（D=64）

> 闫博 2026-10-20 裁定：硬上限直接提到 4095，RAM 64 MiB、Guest 可用 32 MiB。以下第 1–3 条是原 2047 的理由，留作背景；4095 的内存账见 `/workspace/0040/0040-nlogn-algorithms.md`「内存账」（arena 约 0.99 MiB，Guest 总计约 24.8 MiB，32 MiB 内余约 7.2 MiB【源码计算】），O(N²) 由 0040 消除。


1. 2047 是策略上限，不是预算允许的最大值：按上面的公式，预算能容纳到约 N=2623（D=41），但那时余量只剩约 9 KiB。取 D=32（2 的幂）时，五个 context 合计 1 988 720 B，留出约 106 KiB（约 5%），用来容纳新增的指针、容量字段和对齐填充，以及以后的小改动；4095 则明确超出。
2. 目录缓存为 128 KiB，在 arena 里一次性分配，挂载时只需读 32 块（4095 时 256 KiB、64 块）。
3. 现有几处 O(N²) 的代码（见下文）在 N=2047 时约 2×10⁶ 次比较，还在可测范围内；再翻倍就是 8×10⁶，需要换算法。
4. 以后如果真要更多，只需调大编译期常量 `YAN_FS_MAX_DIR_BLOCKS` 并重新做预算核算，盘上格式不用改。

### 1023 个文件时其他写死的上限

| 位置 | 现状 | 在 1023/2047/4095 时的问题 | 处理 |
|---|---|---|---|
| `os/yanfs.c:330,382,389,445,493,514,1042-1059`：slot 循环和 list 游标都以 `YAN_FS_MAX_FILES` 为界；END 时 cursor=63 | 写死 63 | v2 要按实际容量 N 循环 | R2：循环和 END 游标改为 N |
| `os/yanfs.c:382-400`：挂载时两两比较文件名查重；`os/yanfs.c:439` 的 allocate_extent 注明 O(N²) | O(N²) | N=1023 约 5×10⁵ 次，N=2047 约 2×10⁶ 次，N=4095 约 8.4×10⁶ 次 | R5：保留算法，测步数；4095 由 0040 消除 |
| `os/yanfs.c:442-443`：栈上数组 | 8N B | N=1023 时超过 4 KiB 栈 | R5：移进 arena |
| `os/note_metadata.c:209,865` 越界检查；`os/note_metadata.c:876-882` 插入排序 | 63、O(N²) | 上界写死 | R3/R5：上界改为 N，测步数 |
| `os/note_relations.c:474,552` 行数上限；`os/note_relations.c:331-336` 插入排序（整行 36 B 搬移）；`os/note_relations.c:490` 对每条 link 遍历所有行 | 63、O(N²) | 上界写死 | R3/R5 |
| `os/note_blocks.c:246` 名称数上限；`os/note_blocks.c:249` 插入排序 | 63、O(N²) | 上界写死 | R3/R5 |
| `os/search_terms_linear.c:203,222,298`、`os/search_terms_index.c:392,554,674`：以 slot 上界做检查 | 63 | slot ≥ 63 的文件会被当成越界 | R3：改为 N |
| 词项索引的固定表：keys 8192、key_pool 256 KiB、postings 65536、hash 16384（`os/search_terms_index.h:52-55`；`os/search_terms_index.c:163,167,201`） | 按文本总量计，与文件数无关 | 1023 篇笔记很容易超过 65536 个词次，此时索引变成 LIMIT，查询退回完整 SCAN（`os/search_terms_index.c:19-20`）；结果正确但变慢 | 本规格不调大；R4 验证 LIMIT 时结果与 SCAN 一致 |
| 显示上限：search top 20、`YAN_RELATION_RELATED_SHOWN 20`（`os/note_relations.h:56`）、`YAN_BLOCK_REFS_SHOWN 20`（`os/note_blocks.h:14`） | 只限显示条数，total 是 u32 精确计数 | 无 | 不改；R4 验证 total 正确 |
| `YAN_BLOCK_MAX_COUNT 64`（`os/note_blocks_format.h:17`） | 单个文件的块数 | 与文件数无关 | 不改 |
| Host 语义：`YAN_SEMANTIC_FILE_MAX 63`（`host/semantic_corpus.h:11`；`host/semantic_corpus.c:60-61`；`host/semantic.c:193` 用来检查索引头里的 skipped） | 写死 63 | slot ≥ 63 时报 CORRUPT；skipped > 63 的索引会被拒绝 | R6：改为运行时容量，用 calloc 分配；skipped 的检查改为 ≤ 当前 N |
| Host 语义：`YAN_SEMANTIC_SEGMENT_MAX 8192`（`host/semantic_corpus.h:10`；`host/semantic_corpus.c:84`；`host/semantic.c:192`） | 按文本总量计 | 文件多了更容易返回 LIMIT；每段一条 4160 B 的记录（`host/semantic.h:9`），8192 段已经约 34 MB | 不调大；R6 要求 LIMIT 明确报出 |
| MCP 输出上限 1 MiB（`tools/mcp_protocol.h:8`） | 单个响应 | 现有工具都有显示上限（top 20 / shown 20），不会列出全部文件 | 不改 |

## 三栏边界

| 已决定 | 禁止 | 未规定（留给实现者） |
|---|---|---|
| 采用多块目录，D 在 mkfs 时确定；文件数用 `--files N` 指定，N = 64·D−1，D = 1..64（63…4095），缺省为 1023 | 在线扩充目录；子目录；挂载后改变 D | — |
| `--files 63` 写出的就是现有 v1 镜像（逐字节相同）；N > 63 时写 version 2 | 改动 v1 的任何字节定义 | — |
| v2 头：offset 24 的 4 字节为 `directory_blocks` = D，其余保留字段为 0；目录项依次铺满块 0..D-1（块 0 从 offset 64 开始，后续块从 offset 0 开始）；数据从块 D 开始；`entry_capacity` = N | 新增目录项字段 | — |
| v2 的 CRC 覆盖 D×4096 字节，计算时把 offset 60–63 当作 0，只存一份，放在块 0 | 每块单独的 CRC | — |
| 编译期硬上限 `YAN_FS_MAX_DIR_BLOCKS 64`（4095 个文件），只用于参数校验，不用来定数组大小 | 按硬上限开静态数组 | — |
| 所有与文件数有关的存储按镜像的实际 N 从一次性 arena 分配（Guest），或用 calloc 分配（Host） | 运行中再次分配或释放；挂载第二个容量更大的镜像 | arena 对齐方式；各服务 init 的参数形式 |
| 各 context 的 64 KiB 静态断言保留；Guest 总用量（代码 + bss + N=4095 时的 arena）≤ 32 MiB，取代旧的「五个 context ≤ 2 MiB」 | 放宽 64 KiB 断言或 32 MiB 总预算 | — |
| 词项索引和语义的固定表不调大；超出时走现有的 LIMIT 路径 | 静默截断结果 | — |
| 名称规则、0030 身份公式、slot 编号和遍历顺序都不变 | 改 `[[NAME#ID]]`、`link:` 或 MCP 名称 schema | — |
| 旧镜像继续可用；离线迁移保留 slot 号，所以库身份不变；有 `.assets/` 时一并复制 | 原地升级；覆盖已存在的目标；压缩 slot | 迁移工具的命令名 |

## 需求与验收

### 0036-R1：mkfs 设定文件数
`yan_mkfs` 增加 `--files N`：N 必须等于某个 D（1..64）对应的 64·D−1，缺省为 1023。N=63 时输出与现在逐字节相同；N>63 时写 v2。`--blocks` 必须大于 D。参数不合法时以非 0 退出，不创建文件。
- 0036-R1-AC1：Given `--files 63 --blocks 16`，Then 输出与 `41b83b5` 构建的 `yan_mkfs --blocks 16` 逐字节相同。
- 0036-R1-AC2：Given 不带 `--files`、`--blocks 64`，Then 头里 version=2、directory_blocks=16、entry_capacity=1023，CRC 按上面的规则校验通过，块 1..15 全为 0。
- 0036-R1-AC3：Given `--files 0`、`100`、`4096`、`8191`，或 `--files 4095 --blocks 64`（B 必须 > D=64），Then 以非 0 退出，目标路径不存在。

### 0036-R2：挂载并使用 v2 镜像
`yan_fs_mount` 同时接受 v1 和 v2。v2 的校验项：version=2；`1 ≤ D ≤ 64`；`entry_capacity = 64·D−1`；CRC 正确；所有 extent 都落在 `[D, capacity)` 内；文件名不重复。任一项不符都返回 CORRUPT。其余 API 的行为和错误码与 0021/0024 相同，slot 上界和 list 结束时的游标都改为 N。
- 0036-R2-AC1：Given N=1023 的空镜像，When 依次 create 1023 个文件，Then 全部成功，第 1024 个返回 DIRECTORY_FULL；重新挂载后 `list` 按 slot 顺序返回全部 1023 个，到末尾时 cursor=1023，内容逐字节正确。
- 0036-R2-AC2：Given N=127 的镜像，When 分别把 directory_blocks 改成 3、把 entry_capacity 改成 63、把某个 extent 的起点改成 1（这三种都重算 CRC），以及只改坏块 1 的一个字节（不重算 CRC），Then 挂载都返回 CORRUPT，零写入。
- 0036-R2-AC3：Given N=127 的镜像里 slot 0..64 都有文件，When 删除 slot 64 上的文件再 create 一个新文件，Then 新文件复用 slot 64；变化的字节只出现在块 0（CRC）、块 1 和新文件的数据块里。
- 0036-R2-AC4：Given 现有 v1 测试镜像（含 CI 里的 Guest 测试），When 用新代码运行原有的 0021–0035 测试，Then 全部通过；只读用例的镜像 SHA-256 不变；写入用例得到的镜像与用 `41b83b5` 构建的工具跑同一操作序列得到的结果逐字节相同。

### 0036-R3：内存按实际容量分配
Guest 增加一次性 bump arena：

- `os/guest.ld` 在所有加载段（包括 .bss 里的启动栈和 `.tohost`）之后导出 `__yan_arena_start`（16 字节对齐），并定义 `__yan_arena_end = 0x80000000 + 32 MiB`。
- `yan_run` 加载 ELF 时检查 `__yan_arena_start ≤ __yan_arena_end ≤ 环形缓冲区基址`，且 arena 不与任何加载段重叠；不满足，或者生产终端 ELF 缺少这两个符号，就打印原因并以非 0 退出。
- bump 分配要检查对齐后的加法溢出和越界。每次分配先把当前指针按 16 向上取整，再加上请求的字节数。
- 启动顺序：读块 0，校验 magic、version、D、N（`N = 64·D−1`，`D ≤ 64`），通过后才分配 → 从 arena 依次分配目录缓存 (N+1)×64 B、上表 #1–#6 和 #8 → 初始化各服务 → 挂载。arena 不够时，启动报告初始化失败（新增一个 APP_REASON），不运行 shell。
- 之后不再分配。挂载容量与启动时不同的镜像返回 INVALID。
- Host 侧同一套代码用 calloc 分配同样大小的存储。
- 上表 #1–#6 改为指针加容量。

验收：
- 0036-R3-AC1：Given N=63 和 N=1023 的镜像，When 启动 Guest，Then 实测 arena 用量等于按分配顺序逐次计算的 Σ(align16(当前指针) − 当前指针 + 各项字节数)，各项字节数为：目录 (N+1)×64、#1 36N、#2 32N、#3 36N、#4 两份各 N、#5 36N、#6 32N、#8 8N；同一个 ELF 跑这两个镜像（bss 不随 N 变化）。块 0 的 version/D/N 不合法时，启动失败，并且没有发生任何分配。
- 0036-R3-AC2：When 构建 Host 和 RV32，Then 现有三处 64 KiB 静态断言仍然通过；新增测试断言「Guest 代码 + bss + N=4095 时 arena 实际分配（包括对齐）≤ 32 MiB」也通过；N=63、1023、2047、4095 四档的实测数值写进 PR。
- 默认 RAM：`tools/yan_run.c:122` 的 `ram_size` 由 16 MiB 改为 64 MiB；环形缓冲区仍在 RAM 顶部 16 KiB，基址变为 0x83FFC000。测试驱动里写死的 16 MiB 改为 64 MiB：`tests/guest/block_drive.c:636`、`tests/guest/run_runtime.sh:288`、`tests/guest/run_console.sh:334`、`tests/terminal_runtime_driver.c:636`。`tests/guest/run_golden.sh:141` 和 `tools/yan_difftest.c:87` 保持 16 MiB（golden trace 含该值）。
- 0036-R3-AC4：Given 不带 `--ram` 运行 `yan_run`，Then RAM 为 64 MiB，环形缓冲区基址为 0x83FFC000，`__yan_arena_end`（0x82000000）≤ 该基址，启动检查通过；上述 4 个测试驱动不再含 16 MiB 字面值，原有 Guest 测试全部通过；golden 与 difftest 输出不变。
- 0036-R3-AC3：Given 把 `__yan_arena_end` 链接到环形缓冲区基址之上的测试 ELF，Then `yan_run` 以非 0 退出且不执行 Guest；Given arena 被人为缩小到放不下 N=4095 的测试 ELF，Then 启动报告初始化失败，镜像 SHA-256 不变。

### 0036-R4：上层服务在大容量下行为正确
search（线性与索引）、带标签的 search、`links`、`backlinks`、`related`、`block refs/backlinks`、0033 MCP 块工具，都按 N 工作，不再有写死的 63。显示上限（20）和 total 计数的语义保持不变。
- 0036-R4-AC1：Given N=1023 的镜像装满 1023 篇笔记，最后一篇（slot 1022）有唯一的词、标签、一条 `link:` 和一个具名块，When 分别执行 `search`、带标签的 search、`links`、`backlinks`、`related`（以一篇与它共享标签的笔记为源）、`block refs`，以及 MCP 的 `yanos_block_read`，Then 都能找到 slot 1022 上的这篇笔记。
- 0036-R4-AC2：Given 在同一个镜像上构造夹具，使建索引时 posting_count 恰好达到 65536 后还有下一个词次（夹具记录实际词次数），When 执行同一组查询，Then 索引状态为 LIMIT，查询结果（行、分数、顺序）与强制 SCAN 时逐字节相同。
- 0036-R4-AC3：Given 有 1000 篇笔记都含有 `[[a.txt#x]]`，并且都声明了 `link: a.txt`，When 执行 `block backlinks a.txt#x` 和 `backlinks a.txt`，Then 都只显示前 20 条，total 分别等于 1000。
- 0036-R4-AC4：Given 一个 v1 镜像，When 用新代码计算 0033 的 `source_sha256`，Then 与 `41b83b5` 构建的工具算出的值相同。

### 0036-R5：栈和耗时
`allocate_extent` 的两个数组改用 YanFs 的 arena scratch（2×4×N B）。O(N²) 的算法暂时保留（挂载查重、allocate_extent，以及 note_metadata、note_relations、note_blocks 中的三处插入排序），但规定步数上限。
- 0036-R5-AC1：Given N=1023 和 N=2047 两个装满文件的镜像（N=4095 的步数由 `/workspace/0040/0040-nlogn-algorithms.md` 的 0040-R5-AC1 验收，原文见实施澄清 2，0036 保留的 O(N²) 在 4095 时不要求达标），When 在 Guest 中分别执行挂载、create、`list`、`backlinks`、`related`、`block backlinks`，Then 每个操作的 Guest 步数都 ≤ 5×10⁷（PR 中记录实测值）；任务 0 的实测栈用量 ≤ 4 KiB。create 的测法是在装满的镜像里先删除 slot N/2 上的文件，再 create 一个同样大小的文件，确保真的走到 extent 分配；计步从命令行输入完成开始，到输出提示符为止。
- 0036-R5-AC2：When 用 `-fstack-usage` 构建 RV32，Then `allocate_extent` 的栈帧不随 N 增长（N=63 和 N=4095 时帧大小相同）。

### 0036-R6：Host 语义语料
`YAN_SEMANTIC_FILE_MAX` 改为运行时容量：`files` 数组和两个栈上数组都按 N 用 calloc 分配；加载索引时，索引头里 skipped 的检查改为「≤ 当前镜像的 N」。SEGMENT_MAX 保持 8192，超出时按现有逻辑返回 LIMIT，并且 CLI 和 MCP 都要明确报出。
- 0036-R6-AC1：Given N=1023 的镜像，slot 1022 上有一篇笔记，When 运行 `yan_semantic` 建索引并查询，Then 能命中这篇笔记；同一个 v1 镜像的语义索引与 `41b83b5` 生成的逐字节相同。
- 0036-R6-AC2：Given 文本切出的段数超过 8192，Then 建索引返回 LIMIT，CLI 以非 0 退出，MCP 返回带 LIMIT 的错误，不生成部分索引。

### 0036-R7：离线迁移，库身份不变
新增 Host 工具，用法为 `<tool> --from OLD --to NEW --files N [--blocks B]`（B 缺省为 OLD 的块数 + (D_new − D_old)，v1 源的 D_old 计为 1）。流程：

1. 对 OLD 拿 0035 旁锁的共享锁，拿不到就报 BUSY 并以非 0 退出。
2. 用 O_EXCL 创建 NEW。
3. 按原 slot 号写目录项并复制文件内容，空 slot 保持为空。
4. 有 `OLD.assets/` 时把它复制为 `NEW.assets/`；`NEW.assets` 已存在就失败。
5. fsync 后重新挂载 NEW，逐项比对 slot、名称和内容，并核对 0033 `source_sha256` 与 OLD 相等。

任何一步失败都只删除本次运行创建的路径（NEW，以及本次创建的 NEW.assets），然后以非 0 退出。OLD 始终只读。N 小于 OLD 中最大的已用 slot 号加 1 时报错。
- 0036-R7-AC1：Given 一个 v1 镜像，slot 0..62 中有若干空 slot，并带有 `OLD.assets/img/x.png`，When 迁移到 `--files 1023`，Then 退出码为 0；NEW 中每个文件的 slot、名称、内容与 OLD 相同，空 slot 仍为空；0033 `source_sha256` 相等；用 OLD 建的词项索引和语义索引在 NEW 上状态不是 STALE；`NEW.assets/img/x.png` 与原文件逐字节相同；OLD 和 OLD.assets 都不变。
- 0036-R7-AC2：Given 分别出现：NEW 已存在、NEW.assets 已存在、OLD 的旁锁被别人以排他方式持有、B 放不下全部文件、`--files` 小于最大已用 slot 号加 1，Then 都以非 0 退出；事先已存在的 NEW 或 NEW.assets 不被改动或删除，其余情况结束后不留下本次创建的路径；OLD 的 SHA-256 不变。

## 非目标

- 不做子目录，不做在线扩充目录，不做原地升级。
- 不调大词项索引表、语义 SEGMENT_MAX、单文件 16384 字节的编辑和写入上限，以及 MCP 的输入输出上限。
- 不把 O(N²) 换成更快的算法（除非 0036-R5 的步数上限不达标）。
- 不做通用堆（不提供 free），不支持在一次运行中换挂一个更大的镜像。
- 不改 0030/0033 的身份格式和 MCP schema；不做断电恢复（沿用 0021）。

## 约束

- 改动集中在 `os/yanfs.*`、`os/guest.ld`、新的 `os/arena.*`、各服务数组改为指针的部分、`apps/yanfs_terminal/main.c` 的启动顺序、`tools/yan_mkfs.c`、`tools/yan_run.c` 的符号检查、`host/semantic_corpus.*`，以及新的迁移工具。遵守 lean-code，按 R 分 PR（每个约 400 行以内）。
- 测试名和 CTest 标签带 `0036-R#-AC#`；Guest 侧的验收进 CI。

## 假设

- [假设] Guest 一次运行只挂载一个镜像，容量在启动时就能确定（与 `yan_run --disk-image` 的现有用法一致）。
- [假设] `yan_run` 的 RAM 为 64 MiB（缺省）；用 `--ram` 缩小到环形缓冲区与 arena 发生重叠时，由 0036-R3 的启动检查拒绝。
- [假设] 镜像在 MiB 量级，迁移时整份复制的耗时可以忽略。

## 已裁定（闫博 2026-10-20）

见 [0003](../decisions/0003-file-limit-and-images.md) 第 1–3 条：采用多块目录、mkfs 时确定块数；默认 1023 个文件，可通过 mkfs 参数调整；旧镜像继续可用，支持离线迁移，迁移后库身份不变。

## 实施澄清（规格所有者裁定，闫博 2026-10-20 批准 4095 与 RAM 64 MiB）

1. 硬上限 4095，D=1..64，全文统一（R1、R1-AC3 拒绝 `--files` ≥ 4096 及 B ≤ D、R2 挂载 1≤D≤64、内存表含 4095 列，五 context 合计 2 343 024 B）。
2. 不引用 0039（不存在）。具体数字：`yan_run` 默认 RAM 64 MiB，Guest 预算整 32 MiB，`__yan_arena_end = 0x80000000 + 32 MiB`，环形缓冲区 0x83FFC000。4095 的步数由 `/workspace/0040/0040-nlogn-algorithms.md` 的 0040-R5-AC1 验收，原文：「Given N=4095 装满的镜像（0036 已把 `YAN_FS_MAX_DIR_BLOCKS` 定为 64）。When 在 Guest 中执行挂载、create（测法同 0036-R5-AC1）、`list`、`backlinks`、`related`、`block backlinks`，Then 每个操作 ≤ 5×10⁷ 步（Q2 选 A 时，全库操作改按 Q2 的线性上限验收），PR 中记录 N=1023/2047/4095 三档实测值。前提见 P1。」
3. Guest 整体 32 MiB 预算取代「五个 context ≤ 2 MiB」；三栏中「放宽这两项预算」一行已改为只保留 64 KiB 断言与 32 MiB 总预算。
4. RAM 16→64 MiB 和 0x83FFC000 环形缓冲区并入 R3（新增 R3-AC4，含 4 个测试驱动）。
5. R7：`--blocks` 缺省 = OLD 块数 + (D_new − D_old)，v1 源 D_old = 1。

## 开放问题

无。

### 实施澄清（2026-10-20，答 develop-deepseek PR1）
- 批准 PR1（R1+R2 合在一个 PR）的非测试代码约 516 行，超过 400 行。原因：如果先单独合 R1，main 会生成自己挂载不了的 v2 镜像。
- D=1 时沿用 0021 的「至少 1 块」；只有 D>1 时才要求 B>D。
- `allocate_extent` 的栈数组可以在本 PR 里提前去掉，改成不用数组的写法。
- 挂载时 D 与初始化缓冲不一致，返回 INVALID。这是提前套用 R3 的规则。
- 旧测试夹具里用 version 2 表示「未知版本」的，改成 3，断言不变。
- 现有测试调用 mkfs 时都加上 `--files 63`，保持旧格式。
