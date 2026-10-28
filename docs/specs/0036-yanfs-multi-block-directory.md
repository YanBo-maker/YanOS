# 0036：YanFS 多块目录（放宽 63 个文件上限）

- **意图**：一个镜像能存放远多于 63 篇笔记。目录从 1 块扩展为 mkfs 时确定的 D 块：默认 1023 个文件（D=16），最多 4095 个（D=64）。Guest 按镜像的实际容量分配内存，不按硬上限。文件名、引用语法、MCP 工具都不变；旧镜像继续可用，离线迁移后库身份不变。依据：[0003 决策记录](../decisions/0003-file-limit-and-images.md)（闫博 2026-10-20 裁定）。
- **状态**：Approved（闫博 2026-10-20）
- **supersedes**：无整份替代。扩展 [0021](0021-yanfs.md) 的盘上格式（新增 version 2），v1 盘上定义不变；扩展 0029/0033 的文件容量，R4 按 2026-10-21 裁定取代 0028 的普通 backlinks 全量输出约束。0035-R3-AC3 中「目录已满 63 项」按本规格理解为「目录已满（达到该镜像的容量）」。
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
| 4 | `YanSearchTermsLinear.metadata_eligible`（`os/search_terms_linear.h:69`），只分配一份（`index_eligible`，即 `term_index.linear` 内嵌那份；`search_linear` 不再单独分配） | 1 | 63 | 1 023 | 2 047 | 4 095 |
| 5 | `YanSearchTermsIndex.entries`（`os/search_terms_index.h:101`），`YanFsInfo` 36 B | 36 | 2 268 | 36 828 | 73 692 | 147 420 |
| 6 | `YanBlockContext.names`（`os/note_blocks.h:56`） | 32 | 2 016 | 32 736 | 65 504 | 131 040 |
| 7 | Host：`YanSemanticCorpus.files`（`host/semantic_corpus.h:48`，312 B/项），另有两个栈上数组 `YanSemanticFile files[63]`（`host/semantic_corpus.c:109,202`，40 B/项） | 312+80 | 24 696 | 401 016 | 802 424 | 1 605 240 |
| 8 | `allocate_extent` 的 `used_start/used_count`（`os/yanfs.c:442-443`），PR1 已改为不用数组，**不再分配，不计入合计** | — | — | — | — | — |
| — | Guest 合计 #0–#6（未含 align16 填充；arena 实测见实施澄清） | 237 | 14 995 | 242 515（**+227 520 ≈ +222 KiB**） | 485 203（**+470 208 ≈ +459 KiB**） | 970 579（**+955 584 ≈ +933 KiB**） |

与现有预算对比（RV32 值）：

- **每个 context ≤ 64 KiB**（`os/note_metadata.h:93`、`os/note_relations.h:120`、`os/note_blocks.h:66` 的静态断言）。如果把数组照旧留在 context 里，N=1023 时 note context 达到 30 536 + 960×68 = 95 816 B，**会超**；N=2047 时 block context 达到 26 424 + 1 984×32 = 89 912 B，**也会超**。所以本规格把 #1–#6 从 context 里移出、改为 arena 指针。context 自身随之缩小（note 约 26 252 B，relation 约 9 876 B，block 约 24 408 B），三个断言继续成立。
- **五个 context（index/note/shell/relation/block）合计**（旧规则 ≤ 2 MiB，已由 Guest 整体 32 MiB 预算取代，见实施澄清 3）：现为 1 645 488 B。把数组按容量计入后：N=1023 时为 1 645 488 + 960×173 = **1 811 568 B**，余量 285 584 B；N=2047 时为 1 645 488 + 1 984×173 = **1 988 720 B**，余量 108 432 B；N=4095 时为 1 645 488 + 4 032×173 = **2 343 024 B**，超出旧的 2 MiB，但在 32 MiB Guest 预算内。（173 = #1+#2+#3+#5+#6，再加上 index 内嵌的那一份 #4；目录缓存不计入五项合计，另列在 #0；`search_linear` 的那份 #4 已不分配。）
- **4 KiB 任务栈**：`allocate_extent` 在栈上开了 `used_start/used_count[YAN_FS_MAX_FILES]`（`os/yanfs.c:442-443`），占 8×N B。N=63 时 504 B，N=1023 时 8 184 B，**超过任务栈**。必须移进 YanFs 的 arena scratch（0036-R5）。

### 硬上限定为 4095（D=64）

> 闫博 2026-10-20 裁定：硬上限直接提到 4095，RAM 64 MiB、Guest 可用 32 MiB。以下第 1–3 条是原 2047 的理由，留作背景；4095 的内存账见 `/workspace/0040/0040-nlogn-algorithms.md`「内存账」（arena 约 0.93 MiB（970 592 B），Guest 总计约 24.8 MiB，32 MiB 内余约 7.2 MiB【源码计算】），O(N²) 由 0040 消除。


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
- 启动顺序：读块 0，校验 magic、version、D、N（`N = 64·D−1`，`D ≤ 64`），通过后才分配 → 从 arena 依次分配目录缓存 (N+1)×64 B、上表 #1–#6（#4 一份）→ 初始化各服务 → 挂载。arena 不够时，启动报告初始化失败（新增一个 APP_REASON），不运行 shell。
- 之后不再分配。挂载容量与启动时不同的镜像返回 INVALID。
- Host 侧同一套代码用 calloc 分配同样大小的存储。
- 上表 #1–#6 改为指针加容量。

验收：
- 0036-R3-AC1：Given N=63 和 N=1023 的镜像，When 启动 Guest，Then 实测 arena 用量等于按分配顺序逐次计算的 Σ(align16(当前指针) − 当前指针 + 各项字节数)，各项字节数为：目录 (N+1)×64、#1 36N、#2 32N、#3 36N、#4 一份 N、#5 36N、#6 32N；同一个 ELF 跑这两个镜像（bss 不随 N 变化）。块 0 的 version/D/N 不合法时，启动失败，并且没有发生任何分配。
- 0036-R3-AC2：When 构建 Host 和 RV32，Then 现有三处 64 KiB 静态断言仍然通过；新增测试断言「Guest 代码 + bss + N=4095 时 arena 实际分配（包括对齐）≤ 32 MiB」也通过；N=63、1023、2047、4095 四档的实测数值写进 PR。
- 默认 RAM：`tools/yan_run.c:122` 的 `ram_size` 由 16 MiB 改为 64 MiB；环形缓冲区仍在 RAM 顶部 16 KiB，基址变为 0x83FFC000。测试驱动里写死的 16 MiB 改为 64 MiB：`tests/guest/block_drive.c:636`、`tests/guest/run_runtime.sh:288`、`tests/guest/run_console.sh:334`、`tests/terminal_runtime_driver.c:636`。`tests/guest/run_golden.sh:141` 和 `tools/yan_difftest.c:87` 保持 16 MiB（golden trace 含该值）。
- 0036-R3-AC4：Given 不带 `--ram` 运行 `yan_run`，Then RAM 为 64 MiB，环形缓冲区基址为 0x83FFC000，`__yan_arena_end`（0x82000000）≤ 该基址，启动检查通过；上述 4 个测试驱动不再含 16 MiB 字面值，原有 Guest 测试全部通过；golden 与 difftest 输出不变。
- 0036-R3-AC3：Given 把 `__yan_arena_end` 链接到环形缓冲区基址之上的测试 ELF，Then `yan_run` 以非 0 退出且不执行 Guest；Given arena 被人为缩小到放不下 N=4095 的测试 ELF，Then 启动报告初始化失败，镜像 SHA-256 不变。

### 0036-R4：上层服务在大容量下行为正确
search（线性与索引）、带标签的 search、`links`、`backlinks`、`related`、`block refs/backlinks`、0033 MCP 块工具，都按 N 工作，不再有写死的 63。搜索、related 和块反链保留原显示上限及 total 语义。

实施核对发现 0028 的普通 `backlinks` 原来输出全部来源，只有 shown，没有 total；此前“显示上限保持不变”不适用于它。项目所有者于 2026-10-21 明确批准：普通 `backlinks` 统一按来源文件名 ASCII 顺序只显示前 20 项，结尾为 `OK backlinks total=N shown=M target=OK|DANGLING`，M=min(N,20)。v1、v2 均适用；summary 的 total 是完整来源数，shown 是实际交付数。仍预检全库头，晚坏头和读取错误不得被显示上限遮住。此条取代 0028 的 backlinks 全量输出和 total=shown 约束，其余行为保持。
- 0036-R4-AC1：Given N=1023 的镜像装满 1023 篇笔记，最后一篇（slot 1022）有唯一的词、标签、一条 `link:` 和一个具名块，When 分别执行 `search`、带标签的 search、`links`、`backlinks`、`related`（以一篇与它共享标签的笔记为源）、`block refs`，以及 MCP 的 `yanos_block_read`，Then 都能找到 slot 1022 上的这篇笔记。
- 0036-R4-AC2：Given 在同一个镜像上构造夹具，使建索引时 posting_count 恰好达到 65536 后还有下一个词次（夹具记录实际词次数），When 执行同一组查询，Then 索引状态为 LIMIT，查询结果（行、分数、顺序）与强制 SCAN 时逐字节相同。
- 0036-R4-AC3：Given 有 1000 篇笔记都含有 `[[a.txt#x]]`，并且都声明了 `link: a.txt`，When 执行 `block backlinks a.txt#x` 和 `backlinks a.txt`，Then 都只显示前 20 条，total 分别等于 1000。
- 0036-R4-AC4：Given 一个 v1 镜像，When 用新代码计算 0033 的 `source_sha256`，Then 与 `41b83b5` 构建的工具算出的值相同。

### 0036-R5：栈和耗时
`allocate_extent` 的两个数组改用 YanFs 的 arena scratch（2×4×N B）。O(N²) 的算法暂时保留（挂载查重、allocate_extent，以及 note_metadata、note_relations、note_blocks 中的三处插入排序），但规定步数上限。
- 0036-R5-AC1：Given N=1023 和 N=2047 两个装满文件的镜像（N=4095 的步数由 0040-R5-AC1 验收；1023/2047 的达标手段见 0036-R8），When 在 Guest 中分别执行挂载、create、`list`、`backlinks`、`related`、`block backlinks`，Then 每个操作的 Guest 步数都 ≤ 5×10⁷（PR 中记录实测值）；任务 0 的实测栈用量 ≤ 4 KiB。create 的测法是在装满的镜像里先删除 slot N/2 上的文件，再 create 一个同样大小的文件，确保真的走到 extent 分配；计步从命令行输入完成开始，到输出提示符为止。
- 0036-R5-AC2：When 用 `-fstack-usage` 构建 RV32，Then `allocate_extent` 的栈帧不随 N 增长（N=63 和 N=4095 时帧大小相同）。

2026-10-25后续规划：所有者明确4095档全部九项均以50,000,000步为上限，不放宽全库查询门槛。可访问的方案已归档为[0040](0040-4095-file-performance.md)，所有者随后批准整体方案及有效块结果范围补充，当前为Approved，实施与验收尚未完成。上文1023/2047要求和下文历史结论保持；原仓库外0040引用保留为历史背景，不表示旧完整文件已恢复。

2026-10-26后续进展：0040的局部优化与固定夹具性能门禁已完成本机验收，验证和发布状态见[0040 VERIFY](0040-4095-file-performance.md#verify-执行记录)。此前条款和历史结论保留。

### 0036-R8：让 R5-AC1 达标的改动（闫博 2026-10-20 #70 裁定）
基线：N=1023 实测 mount 7.45e7、create 8.9e6、list 1.2e6、backlinks 1.91e8、related 1.90e8、block backlinks >7e8（未跑完）。主要开销是逐字节块传输（每 4 KiB 块 19–28 万步），其次是挂载两两比较、`find_entry` 线性查找（`os/yanfs.c:490`）和按 36 B 行逐字节搬移的插入排序。
必须修的查询：**mount、backlinks、related、block backlinks**；`meta list`（含标签过滤）和搜索扫描同样逐文件读头，必须在 PR 中实测，超标即同样适用本条改动。create、`list` 已达标，只回归。
改动（设计沿用 0040 原方案，0040 对应条目已标「移至 0036」）：
1. Guest 块传输（ring 拷贝）和 `memcpy`/`memmove` 按 32 位字拷贝，非对齐头尾逐字节。
2. 挂载校验：非空 slot 下标按名字排序后比相邻（重名），`start≠0` 的 extent 按 start 排序后比相邻（重叠），仍只返回 CORRUPT（0040 原 R1）。
3. `find_entry` 改为在 `name_order[N]`（uint16，arena）上二分；挂载成功和 `commit_entry` 发布时整体重建（0040 原 R4）。`name_order` 是文件系统内部工作区：文件 API 在原有状态优先级之后检查参数，与该表重叠的输入或输出范围返回 INVALID，不产生 I/O，索引保持不变；read 的有效字节数和 list 输出沿原错误规则处理。此补充由项目所有者于 2026-10-21 批准，未改变普通文件操作、命令或磁盘格式。
4. 公共下标堆排序 `yan_sort_index`（迭代 sift-down），供第 2、3 项使用，并替换 metadata、relations、blocks 三处插入排序（0040 原 R3）。
5. block backlinks 若在 1–4 之后仍超标：同一次查询内对 `target()` 的重复读做去重（按目标 slot 记一位，arena N 位），不建持久链接索引。
- 0036-R8-AC1：Given N=63、1023、2047 的同一组镜像（含 related 同分只能靠名字区分的行、重名/重叠/首尾相接的挂载夹具），When 分别用基线（本 PR 前的 main，PR 中写 SHA）和新实现执行 mount、create、`ls`、`meta list`、`backlinks`、`related`、`block backlinks`、`search`，以及 MCP `yanos_block_backlinks`，Then 终端输出、返回码、MCP JSON 和镜像 SHA-256 逐字节相同。
- 0036-R8-AC2：Given 随机 create/delete/rename/copy 序列（固定种子，10⁴ 步，N=2047），Then 每步后 `find_entry` 与线性 oracle 结果相同，`name_order` 严格递增且恰好覆盖非空 slot；注入写失败后不变量仍成立。
- 0036-R8-AC3：Given 0..2047 个随机名字及已排序、逆序输入，Then `yan_sort_index` 与插入排序参考实现逐项相同；源码中不再有 `while (j > 0` 形式的插入排序。
- 0036-R8-AC4：R5-AC1 在 N=1023、2047 全部通过（PR 中记录改前、改后步数）；`validate_directory` 栈帧在 N=63 与 4095 时相同。
- 落位：PR2b 之后新开 **PR3（R8）**，原 R4+R6、R7 顺延为 PR4、PR5。非测试代码约 200 行【估算】（字拷贝约 40、sort 约 45、挂载约 50、name_order 约 45、三处排序调用约 20；第 5 项若需要再加约 20），测试约 350 行。

R8 实施记录：块扫描和线性词项扫描对不超过既有 4 KiB 缓冲区的文件复用同次读取，先完成整文件文本资格检查，再解析或排名；更大的文件沿用流式读取。没有新增持久缓存、磁盘索引或接口。`block backlinks` 的目标检查原本只执行一次，因此未增加第 5 项的去重位表。R3-AC1 的原分配序列在 R8 后追加四项 `2N` 字节表（名称下标及三项排序下标），仍逐项按 align16 计算；原 PR2b 用量保留在后面的历史实施澄清中，当前数值见 VERIFY。

#### 数据结构结论
1. 光加内存索引不够：开销主要在 I/O，每个文件读头约 2×10⁵ 步，backlinks 在 N=1023 时就要 ~2×10⁸ 步；`find_entry` 线性查找 ×N 只有 ~5×10⁶ 步【估算】。改成按字拷贝后，每文件约 1–2×10⁴ 步，N=2047 约 2–4×10⁷ 步，所以 `name_order` 加字拷贝就能达标【估算，以 R8-AC4 实测为准】。backlink/link 索引要么在挂载时把 N 个文件头全读一遍（成本转嫁给 mount），要么写到盘上，所以不建。
2. 盘上格式不变。
3. 新增内存：`name_order` 2N + 三处排序下标 3×2N = 8N B，N=1023 时 8 184 B，N=4095 时 32 760 B【源码计算】；第 5 项再加 N/8 B。挂载排序复用 R5 现有的 8N scratch。对 32 MiB 预算影响可忽略：4095 时总量约 24.8 MiB，余约 7.2 MiB 不变（0040 内存账已含这几项）。

### 0036-R6：Host 语义语料
`YAN_SEMANTIC_FILE_MAX` 改为运行时容量：`files` 数组和两个栈上数组都按 N 用 calloc 分配；加载索引时，索引头里 skipped 的检查改为「≤ 当前镜像的 N」。SEGMENT_MAX 保持 8192，超出时按现有逻辑返回 LIMIT，并且 CLI 和 MCP 都要明确报出。

工程接口由 `YanSemanticSource.file_capacity` 携带 N，POSIX 适配器在成功挂载后绑定；corpus 与摘要入口拒绝容量 0 或大于 4095，不用 0 隐式表示旧容量。`YanSemanticCorpus` 首次使用前清零，scan 按 N 分配文件表；反复 scan 会先释放上次表，成功或失败后均由调用者执行 `yan_semantic_corpus_release`。facade 的 rebuild/status/search 全部退出路径均释放它。纯来源摘要的临时表在每条退出路径释放；这条块读取路径的分配失败按 0033 映射为 IO，语义 facade 原有 NOSPACE 类保持。

容量不进入来源身份或索引字节编码。默认 CI 使用从 `41b83b5` 实测捕获的 v1 固定来源身份和索引兼容样本；样本比较只归零模型/runner/配置身份的头字节 [64,160)，其余头、payload 校验及全部记录字节保持精确比较。本机再显式传入旧工具，对相同模型/runner/配置下的完整索引逐字节对照。Guest 输出不含分数，因此 R4-AC2 另由原生 API probe 在同一 LIMIT 镜像比较分数、顺序和摘要，不能仅凭终端输出宣称分数相同。
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
- 0036-R7-AC1：Given 一个 v1 镜像，slot 0..62 中有若干空 slot，并带有 `OLD.assets/img/x.png`，When 迁移到 `--files 1023`，Then 退出码为 0；NEW 中每个文件的 slot、名称、内容与 OLD 相同，空 slot 仍为空；0033 `source_sha256` 相等；用 OLD 建的盘上语义索引在 NEW 上无需重建且仍为 READY；内存词项索引在 NEW 上正常重建为 READY，搜索内容、分数和排序与 OLD 一致；`NEW.assets/img/x.png` 与原文件逐字节相同；OLD 和 OLD.assets 都不变。
- 0036-R7-AC2：Given 分别出现：NEW 已存在、NEW.assets 已存在、OLD 的旁锁被别人以排他方式持有、B 放不下全部文件、`--files` 小于最大已用 slot 号加 1，Then 都以非 0 退出；事先已存在的 NEW 或 NEW.assets 不被改动或删除，其余情况结束后不留下本次创建的路径；OLD 的 SHA-256 不变。

2026-10-21 所有者澄清：R7-AC1 原写「用 OLD 建的词项索引和语义索引在 NEW 上状态不是 STALE」。0026 词项索引只保存在 RAM，来源编号随重新挂载改变，不能随镜像跨进程携带；因此改为上面的重建与结果等价验收。盘上语义索引仍直接复用，0033 来源身份格式保持。

2026-10-21 所有者批准的附件与失败边界：附件源为 `realpath(OLD) + ".assets"`，递归复制普通文件和目录；符号链接或特殊文件使迁移失败，并清理本次创建的目标。目标落在源附件树内时，在创建前拒绝，避免自包含复制。沿用 0035 保留旁锁 `.lock`，不把锁文件作为迁移产物删除。迁移期间源镜像与附件须保持稳定，不新增快照或重试机制。前述失败清理规则针对清理操作本身可完成的情况；若磁盘或权限错误使清理也失败，工具必须非 0 退出并明确列出残留路径，不得声称已清理完毕。断电恢复仍为非目标。

## 非目标

- 不做子目录，不做在线扩充目录，不做原地升级。
- 不调大词项索引表、语义 SEGMENT_MAX、单文件 16384 字节的编辑和写入上限，以及 MCP 的输入输出上限。
- 不把 O(N²) 换成更快的算法，0036-R8 列出的几处除外（闫博 2026-10-20 裁定）。
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
- （答 #62）在 R3 合入之前，`yan_mkfs` 的 `--files` 默认值保持 63，PR1 可以显式传参生成 v2 镜像。默认值在 PR2b 里改成 1023（PR2 拆分后，见下条），并在那里验证 R1-AC2 的默认行为（原文误写为 R1-AC1）。这样 main 上的每个提交，用默认参数做出的镜像都能被所有使用方挂载。
- ~~（答 PR2）R5-AC1 步数上限移交 0040~~ **撤回**（闫博 2026-10-20 于 #70 裁定：R5-AC1 留在 0036，算法在 0036 内修，见 R8）。N=1023 实测步数保留为基线。
- PR2 拆成两个 PR。PR2a 包含 arena、guest.ld、yan_run 的 64 MiB RAM、启动顺序，以及 R3-AC3/AC4。PR2b 包含服务改用指针、Host 端 calloc、1023 默认值，以及 R3-AC1/AC2。1023 默认值跟 PR2b 一起进。
- R3-AC1 的 arena 公式去掉 #8（按文件分配的 allocate_extent 数组，PR1 已删除）。#4 只分配一份（index_eligible）。arena 用量（一份 #4、无 #8，含 align16 填充）：N=63 时 15,008 B，N=1023 时 242,528 B，N=2047 时 485,216 B，N=4095 时 970,592 B。此前的 15,072 / 243,552 / 974,688 是按两份 #4 算的，作废。
- R5-AC1 的栈部分（N=1023 和 2047 时 task0 栈 ≤ 4 KiB；当前实测 1088 B）随 R8 在 PR3 里一起验收，不在 PR2a/PR2b 验收。
- 两个 arena 符号必须同时存在或同时缺失；只缺其中一个时，yan_run 拒绝运行。
- 第 4 项只分配 terminal 实际用到的那一份，内存表跟着改。
- 下面几处按实现方的做法确认：R3-AC3 用临时链接脚本；启动时在 mount 之前多读一次块 0，7 个故障注入脚本的索引后移一位；删掉 yan_fs_init，统一改用 yan_fs_init_directory；服务允许在 mount 之前 init，但查询要求已挂载；README 示例改用 `--blocks 64`。
- （答 PR2a）PR2a 加一个临时守卫：terminal 挂载后，如果 N>63 就拒绝并给出明确的 reason code，避免服务里 63 项的固定数组越界。PR2b 删掉这个守卫和这个 reason code。PR2a 非测试代码约 341 行，yan_fs_init 的删除放到 PR2b。第 4 项在 PR2a 里不分配，到 PR2b 只分配 index_eligible 那一份。

## VERIFY 执行记录

### R8 本机复核（2026-10-21）

本轮基线为 main `4c5e0ab`；工作分支以 `2df5793` 为基点，保留所有者交来的 R8 修改。实现、默认回归、sanitizer、Windows、性能及实际变异验证已完成；独立文档对齐和总审核已完成。远端 CI 与合入审查另记，不能由本机结果推定。

- Linux Release 默认 CTest：138/138，183.10 s；Debug ASan/UBSan：138/138，321.52 s；Windows MSVC Debug：76/76，156.94 s。均零失败、零跳过。Windows 不运行 Linux/Guest 工具用例。测试包括新增 `sort_index`、`ring_copy`、`os_memory_helpers`、`yanfs_name_order` 和 `yanfs_r8_performance`；Guest 性能验收已注册到 Linux 默认套件。
- 开启实际变异后注册 156 组，选 `_mutation$` 的最终冻结树验证为 17/17，375.86 s，零失败、零跳过；基线、指定 owner/断言与负控均通过。设备变异只排除不链接模拟设备的原生 `yanfs_name_order`，默认回归仍完整运行该测试；当前设备工作副本与冻结树的 FS 源码、接口、v2 测试和排除脚本哈希逐项相同。旧版门禁失败和被中止的重复设备测试保留为诊断，未记作通过。
- R8-AC1：N=63、1023、2047 的 21 组终端输出及退出码与基线逐字节相同，完整镜像字节保持。满目录夹具的 extent 首尾相接；重名和重叠分别重算 CRC 后拒挂，新旧输出及镜像保持一致。related 同方向、同标签候选按名字区分。MCP 线级 JSON 三档相同：v1 成功，v2 保持 R6 尚未实现时的 CORRUPT；后两档不是 v2 检索成功证据。
- R8-AC2：N=2047、固定种子 `0x0036AC2D00C0FFEE`，真实 create/remove/rename/copy 共 10,000 次；每步校验名字表的覆盖、唯一性、严格递增及查找，最后穷举所有存活名字。一次 v2 第二元数据写失败后，已发布缓存与名字下标仍一致，故障镜像由另一个实例拒挂。Release、sanitizer、MSVC 均通过。
- R8-AC3：0..2047 的随机、顺序、逆序索引与独立参考排序相同；附加 4095 档。环形拷贝独立字节 oracle 覆盖 47,104 组回绕、对齐和守卫边界；真实 Guest 内存函数覆盖分离对象及重叠，sanitizer 启用无关指针比较检查。
- R8-AC4：下表计步从 UART 消费命令末尾 LF 到下一个完整提示符，mount 计到首提示符；create 先删除 slot N/2，再创建同名、同长度内容，实际进行 extent 分配。每项小于等于 50,000,000 步，最大实测任务栈 2096 B。RV32 `-fstack-usage -fno-inline` 下 `allocate_extent` 和 `validate_directory` 都是 48 B 静态帧，N 是运行时输入，帧不随 63/4095 增长。

| 操作 | N=1023 基线 | N=1023 R8 | N=2047 基线 | N=2047 R8 |
| --- | ---: | ---: | ---: | ---: |
| mount | 74,537,061 | 7,808,922 | 277,828,697 | 16,281,233 |
| create | 8,922,603 | 11,340,016 | 27,845,583 | 33,383,215 |
| ls | 1,193,070 | 1,133,770 | 2,386,030 | 2,267,338 |
| meta list | 190,642,704 | 15,238,156 | 668,637,945 | 30,893,798 |
| backlinks | 191,214,632 | 16,199,295 | 669,778,193 | 32,921,047 |
| related | 190,257,591 | 15,235,575 | 667,812,512 | 30,942,816 |
| block backlinks | 422,550,806 | 23,413,305 | 1,419,603,455 | 47,124,417 |
| search | 224,350,203 | 22,138,134 | 735,070,203 | 44,384,164 |

R3-AC1（含 R8 的四张下标表）arena 实测/独立求和：N=63 为 15,518 B，1023 为 250,718 B，2047 为 501,598 B，4095 为 1,003,358 B；32 MiB 总预算及三处 context 静态断言仍通过。create 的额外开销来自提交后整体重建名字表，属于 R8 已批准的取舍。

性能默认测试另外执行 `meta list tag:t`：N=1023 为 15,268,110 步，N=2047 为 30,954,947 步，均排除那篇没有标签的夹具。包含名称索引别名保护的修补后，16 条命令均成功，镜像字节不变。工作区回归测试还确认 read 输出/字节数、stat、list 输出/游标、source 输出及 create 内容不得覆盖索引，拒绝时零 I/O。

复现：配置启用 `YAN_BUILD_TOOLS`、`YAN_BUILD_SEMANTIC` 及 RISC-V 工具链，分别运行 Release / sanitizer 默认 CTest；Windows Debug 运行默认 CTest。`ctest --test-dir BUILD -R 'yanfs_name_order|yanfs_r8_performance|stack_usage|arena_accounting' --output-on-failure` 可重跑核心验收。实际变异另启 `YAN_ENABLE_MUTATION_TESTS` 并选 `_mutation$`，纯门禁不计作实际缺陷检测。

证据根目录 `E:/YanOS/build/20261020-development-audit/`：`closure-release-ctest.log`、`closure-asan-ctest.log`、`closure-windows-ctest.log`、`release/yanfs-r8-performance/measurements.json`、`terminal-parity.json`、`mount-parity.json`、`mcp-parity.json`。基线 Guest、命令、状态与捕获见相邻 `20261020-performance-audit/`。`r8-snapshot-inputs.json` 的对照对象是所有者原目录 `E:/YanOS-36/`：该目录的 26 个输入均未改动，另有原始副本保存在 `input-copy/`。当前隔离分支在这份输入之上修复了其中的文件；它们不应与输入快照保持相同哈希。

独立只读 dsh 首轮审核已完成，指出冻结证据过期、完成口径、输入路径歧义和工作区保护的规格缺口；已重新冻结并复验，原目录路径已明确，工作区补充已获所有者批准。证据另见 `code-freeze.json`、`original-input-verification.json`、`frozen-device-snapshot.json`、`frozen-actual-mutations.log`。同一只读dsh会话两轮对齐已完成；提交前九项总审核已核对。本机收口时尚未推送或合入；后续远端与合入事实见下条记录。


### R8 远端与合入记录（2026-10-21）

[PR #80](https://github.com/YanBo-maker/YanOS/pull/80) 的精确 head `2b82d17065bbd5b83e2d5e2f201f37f5308dedf5` 经 [Actions 39515061452](https://github.com/YanBo-maker/YanOS/actions/runs/39515061452) 验证：Linux Release 138/138（268.55 s）、Debug ASan/UBSan 138/138（638.03 s）、Windows Debug 76/76（203.63 s），默认测试零失败零跳过。两个 Linux 任务另行复验 MCP 四项并通过；实际变异仍为上节的本机证据。本次工作流未配置 artifact 上传，使用 Actions 任务原始日志，不将本机文件冒充 CI artifact。

PR 已于 2026-10-21 合入 main，merge 为 `abbb1514e3dd5af9ffedc3bd6504c5965730a876`，由 `gh pr view 80 --json state,mergeCommit,mergedAt` 核实。R8 不等于 0036 整体完成，R4/R6/R7 继续按已批准范围推进。

### R4＋R6 实施记录（2026-10-21）

项目所有者批准推进 R4＋R6。基线为 `abbb151`；语料表已按挂载容量分配，大容量终端、Host CLI 与 MCP 按 R4/R6 验收。普通 backlinks 的新显示规则由所有者另行批准，原分段上限、索引字节格式及来源身份保持。实现与本机验证已完成，独立冻结对齐与提交前总审核已完成；本地提交准备完成，尚未推送或有本阶段远端 CI，不把 0036 整体改为 Implemented。

- 最终默认 CTest：Linux Release 141/141，211.39 s；Debug ASan/UBSan 141/141，745.57 s；Windows MSVC Debug 77/77，192.97 s。零失败、零跳过。新增 `semantic_digest_alloc`、Linux `large_capacity_gate` 与 Linux/Guest/semantic 配置下的 `yanfs_large_capacity`；Windows 不运行后两项。
- 实际变异配置共 159 个注册；选 `_mutation$` 的实际检查 17/17，584.79 s，零失败零跳过。默认纯门禁不冒充实际缺陷检测。设备组只排除原生纯 FS 的 `yanfs_name_order`，其余策略沿 R8；27 个非文档输入在运行前已冻结，测后哈希全部保持。设备组的临时树退出后按脚本清理，`device-snapshot.json` 记录冻结前后核对及此范围，不声称读取已删除的副本。
- R4-AC1：真实生产 FS API 创建并重新挂载 1023 个文件，独立解码确认全目录与最后 slot 1022 的字节；Guest 普通/标签 search、links、backlinks、related、block refs 及 MCP block_read 均访问到最后一篇。索引 READY 的初始 posting 实测为 10,144。
- R4-AC2：把最后一个额外 posting 放入夹具，产品先报告 READY/65,536，再因第 65,537 条进入 LIMIT；7 组 Guest 输出与强制 SCAN 逐字节一致。独立原生 API probe 另比较 3 组查询的行号、分数、顺序、截断标记和原始摘要；唯一词的分数为 1，防止只比较终端上没有分数的文本。
- R4-AC3：整文件和块反链均 total=1000、shown=20，实际只交付 ASCII 名称顺序的前 20 项；完整计数未截断。原生默认用例还预检了第 20 项之后的坏头。另将原生读失败注入延后到第 24 篇，独立实验 40/40：零回调、summary 未发布、busy 已释放，后续调用保持 FAULTED 且零新增读取；该实验是 `late-io24.log`，不计入 CTest 注册数。
- R4-AC4/R6-AC1：MCP 与 CLI 的 v1 来源身份匹配 `41b83b5`；带旧工具路径的本机验收实际比较完整索引字节，v1 索引未变。默认 Linux CI 的固定样本保留来源、计数、payload 校验和记录字节，只规范化外部依赖身份字段；这与本机完整字节比较分别记录。额外的稀疏/满 v1 两档对照也保持来源、索引、CLI JSON 和块操作内容；旧 MCP 外层 serverInfo 的 0033→0035 是本阶段前已存在的版本变化，不把它当 R6 回归。
- R6-AC2：v1 与高槽位 v2 的超过 8192 段夹具，rebuild/search/status 均明确 LIMIT，CLI 非零退出，MCP 错误码保留；原有索引字节不变，fresh 路径不留下部分索引。语义超限时，独立原文身份路径仍允许 MCP block_read 读取 slot 1022。
- 错误边界：摘要分配失败先实测返回 NOSPACE（映射成 LIMIT），修后直接注入失败为 IO，容量 63/1023/4095 均零来源回调、输出保持。普通 backlinks 的 v1 完整目录和 shell/Guest/oracle 已按所有者新规则同步；首轮旧 oracle 和测试驱动的聚合返回值错误保留为诊断，不作为最终回归结论。缺输入门禁 4 项实测通过：源码/被测产物缺失硬失败，只有外部编译器缺失 77，校验不枚举无关目录。
- 真实模型为既有 Qwen3-Embedding-0.6B Q8_0，模型 SHA-256 `06507c7b42688469c4e7298b0a1e16deff06caf291cf0a5b278c308249c3e439`，runner SHA-256 `be637d0423071dfe307c0ab5579ceeff7252f7395ddf65d8cbf40da98e637b5b`。在 1023 个具名文件中放入原固定质量语料（其余为空文件），把 irq.txt 放在 slot 1022；21 个真实分段建索引成功，“设备如何叫醒处理器”和“修改笔记以后检索缓存怎么办”各自目标均排第一。20 项原文、物理行号和字节范围已核对，镜像与索引只读。这里只验高槽位真实路径和两条固定问题，不扩大 0029 的通用质量结论；fake runner 只验证协议。

本机证据根 `E:/YanOS/build/20261021-r4-r6/`：`code-freeze.json`（27 个非文档输入）、`closure-release-ctest.log`、`closure-asan-ctest.log`、`closure-windows-ctest.log`、`actual-mutations.log`、`real-highslot/report.json`、`v1-parity/compare.json`、`integration-red.log`、`integration-green.log`、`late-io24.log`。默认大容量的完整 AC 记录在前一阶段共用构建目录 `E:/YanOS/build/20261020-development-audit/release/yanfs-large-capacity/large-capacity-evidence.json`；该目录复用构建配置，不复用旧测试结果。dsh 的首轮实施报告及原始定向证据在隔离 worktree 的 `build/20261021-r4-r6/`，首轮报告已复制到本机证据根。

独立 dsh 的第二轮冻结复核已完成，首轮阻塞均已消除；提交前九项总审核已核对。R8 合入后监控通道已原样归档并核对 SHA-256；当前 R4/R6 未合入，不归档新通道。旧当前阶段原样追加到 STATUS-history，0036 规格与索引保持 Approved，R7 迁移仍未实现。远端 CI、推送和维护者意图验收另记，不由本机验证推定。


### R4＋R6 远端记录（2026-10-21）

项目所有者批准推送后，代码 `0839ab7` 和相邻文档 `131a39b` 已发布为 [PR #81](https://github.com/YanBo-maker/YanOS/pull/81)。精确 head `131a39b4c881598f0e8e11b363ba71c9124dde91` 的 [Actions 39537301229](https://github.com/YanBo-maker/YanOS/actions/runs/39537301229) 已通过：Linux Release 141/141（419.52 s）、Debug ASan/UBSan 141/141（996.35 s）、Windows Debug 77/77（317.40 s），零失败零跳过。两个 Linux 任务实际执行 `yanfs_large_capacity`、`large_capacity_gate` 和 `semantic_digest_alloc`，并另行复验 MCP 四项；Windows 执行分配失败用例。实际变异和真实模型仍是上节的本机证据。

完整证据为 Actions 三项任务日志，工作流未配置 artifact 上传；核实副本为 `E:/YanOS/build/20261021-r4-r6/ci/` 的三份 `job-*.log` 与 `verified.json`，不将本机日志冒充 CI artifact。由 `gh pr view 81` 核实，PR 当前仍 OPEN，尚未合入；上节「尚未推送」保留为当时本机收口记录。

### R7 实施记录（2026-10-21，进行中）

项目所有者要求规划并继续下一项，沿已批准的 R7 推进。依赖基线为 PR #81 的 `131a39b`，分支 `codex/0036-r7-offline-migration`。本阶段只增加独立 Host 离线迁移模块、薄 CLI 与实际验收；索引验收和附件/失败边界的两项补充已获所有者批准并写入 R7 与决策 0003。实现交由 dsh 编码和独立验收，总审核负责集成、文档及最终核对；本阶段尚无完成的实现或回归结论，不推定已交付或已合入。


### R7 本机验证与待审事项（2026-10-21）

上条为阶段启动记录。迁移模块 `host/image_migrate.c/.h`、薄 CLI `tools/yan_migrate.c` 和独立验收已实现，CMake 已注册 `yanfs_migrate_acceptance`、`yanfs_migrate_work_gate`，故障目标包含 `readdir` wrap。已有生产模块未改动。实现与本机验证完成；最终冻结对齐及提交前总审核已完成；本次冻结审核时 R7 尚未提交、推送、进入远端 CI 或合入，0036 保持 Approved。

| 配置 | 本次默认 CTest | CTest 实测耗时 |
| --- | --- | --- |
| Linux Release，tools/semantic ON | 143/143 | 247.84 s |
| Linux Debug ASan/UBSan，tools/semantic ON | 143/143 | 668.32 s |
| Windows MSVC Debug，tools/semantic OFF | 77/77 | 201.77 s |

默认回归均零失败、零跳过；Windows 不编译或运行 Linux 迁移工具。变异配置实际注册 161 项，显式选 `_mutation$` 的 17 个实际组为 17/17，536.23 s，零失败零跳过；未把该配置所有注册项说成一次完整回归。默认门禁和实际变异负控均实际执行，断言/owner 判据保持，timeout、信号和 sanitizer 错误不计为发现缺陷。

默认回归启动前冻结 355 个非文档输入。总审核随后仅修订迁移验收及工作目录门禁两份测试脚本：真实失败优先于外部依赖 77，显式传入但缺失的 CLI 故障程序在工作目录初始化前硬失败，并补齐单块空 v1 边界；其余输入包括全部生产代码保持。修后两项测试在 Release 2/2（19.71 s）及 ASan/UBSan 2/2（20.11 s）通过，完整迁移矩阵均为 74 PASS / 0 FAIL / 0 SKIP。全套回归与这两项修后复验分别记录，不能把早期 72 项日志当成最新 74 项结果。

- R7-AC1：独立 Python 解码器核对 v1 稀疏槽、空文件、UTF-8、跨块二进制、空槽、尾零和来源摘要；覆盖默认/显式块数、v2 高槽扩容与允许的缩容、63/4095 容量。单块空 v1 按默认公式扩到 1023 时 B=16，不满足 v2 的 B>D，明确 NOSPACE；显式 `--blocks 17` 成功。工具不偷偷增加容量。
- R7-AC2：目标镜像或附件已存在（包括悬空链接）、排他旁锁、容量或槽范围不足、坏源、附件特殊文件及目标位于源附件树内均被拒绝；核对旧镜像及附件保持、预存目标不被删除。写入、刷新、重新挂载、比较、关闭和清理故障分别验证，清理失败诊断保留完整残留路径。
- 词项索引在 OLD/NEW 的独立新进程分别 rebuild 为 READY，非空查询的原文、分数、顺序及摘要相等。盘上语义索引采用仓库 fake runner、自造 model/index 验证无重建复用，NEW 为 READY 且有实际命中；fake runner 不代表真实模型质量。
- 真实索引复用另行验证：既有 1023 容量、64 块镜像迁移到 4095 容量、112 块，slot 1022 的 irq.txt 保持；直接使用旧盘上索引，未 rebuild，`STATUS READY segments=21 skipped=0`。两条固定问题的完整 JSON（分数、原文、行号和字节范围）与 OLD 结果相同；旧镜像、索引、Qwen 模型和 runner 的 SHA-256 均不变。仅验这份固定语料的迁移兼容，不扩展通用检索质量结论。
- `YAN_BUILD_SEMANTIC=OFF`、`BUILD_TESTING=OFF` 的独立 tools 配置可构建 `yan_migrate`，迁移本身不需要模型、HTTP 或 JSON 依赖。开启语义工具的默认 Linux 验收从当前源码生成受控坏候选和实际 CLI 故障副本，不依赖本机预建程序或模型路径。tools-only 验收显式跳过语义索引项；工作目录门禁的基础子集也显式跳过可选项，均不冒充完整 AC1。

总审核真实 RED → GREEN 与永久回归：

| 发现 | 原问题 | 修复与当前依据 |
| --- | --- | --- |
| ROOT-F1 | 最终父目录 close 失败后留下 NEW，未报告残留 | 重开父目录清理；失败则列出可能残留。`FAULT.parent-close` 与实际 CLI 双残留测试通过 |
| ROOT-F2 | CLI 512 字节缓冲截断合法长残留路径 | 实际生产 CLI 保留两条完整路径；731 字节目标及 738 字节附件路径测试通过 |
| ROOT-F3 | 缺工具前删除调用者整个工作目录 | 先核显式输入，仅创建本次 `run-*`；marker 在错误和正常运行后保持，工作目录门禁通过 |
| ROOT-F4 | 成功 write 遗留 errno=EINTR 被当成目录读取失败 | 每次 readdir 前清 errno，只在该次 NULL 返回时判断；成功写保留 errno 及 copy/verify/remove 真实 EIO 测试通过 |
| ROOT-F5 | 断言失败与缺外部编译器并存时返回 77，掩盖失败 | 有 FAIL 先返回 1；门禁同时核对纯外部依赖缺失仍为 77 |
| ROOT-F3 补核 | 显式 `--cli-fault` 缺失未在输入预检检查，退出 0 | 缺输入退出 1，零工作子目录；永久门禁核对 marker 和初始化顺序 |

证据根 `E:/YanOS/build/20261021-r7/`：`frozen-inputs.json`、`final-inputs.json`、`final-test-only-changes.json`、四份 `*-registered.json` / `*-ctest.log` / `*-verification.json`、`release-r7-final.log` / `asan-r7-final.log`、`verification-summary.json`、`real-index-reuse/report.json`，以及 `root-close-dir-*`、`root-long-residual-*`、`root-input-gate-*`、`root-success-errno-*`、`root-skip-priority-red/green.json`、`root-cli-input-red/green.json`。早期 RED 与旧专项日志保留为诊断。所有者原目录 `E:/YanOS-36` 的 26 个输入逐项 SHA-256 保持，见 `owner-worktree-preservation.json`。

dsh 本阶段复用同一协调会话及原编码/独立验收两个子任务，子任务均已停止。报告如实记录两轮技术审核与修后核验；第二轮仍有 CMake 接入阻塞，现由总审核完成并验证；独立只读冻结对齐已完成，不把实现报告当成文档 PASS。生产 C/头文件共 1459 行，另有 CMake 接入 54 行，超过约 400 行暂定 PR 范围；重复盘上解码已删除，附件 copy/verify/remove 已合并。提交范围与额外一轮冻结核验已由所有者批准（见下条），本轮为批准的额外审核，不自动增加审核轮次或放宽后续范围。

未覆盖断电恢复、源/附件并发变化和目标父目录被并发替换；沿用已批准的离线稳定前提。附件末尾 EOF 探测遇 EINTR 可保守失败，故障 wrapper 与当前系统调用形状相关；这些低优先级审查意见留在报告，待最终审核分类，不声称已解决。复现入口：Linux tools/semantic 配置后运行 `ctest --test-dir BUILD -R '^yanfs_migrate_' --output-on-failure -V`；源码/产物缺失必须硬失败，外部编译器缺失可返回 77。

2026-10-21 所有者随后批准：本次保留 R7 整组（生产 C/头文件 1459 行，另有 CMake 54 行），并复用原 dsh 会话增加一轮只读冻结审核。此为本阶段范围和审核轮次的例外，不修改全局约 400 行与两轮规则；尚未授权 R7 推送或创建 PR。

2026-10-21 独立只读冻结审核完成：当前源码、测试、CMake、正式文档及 355 个非文档输入与上述证据逐项核对，未发现阻塞；状态词按 `E:/YanOS/build/20261021-r7/dsh-final-audit-report.md` 机械替换。该轮由所有者明确批准，原两个子任务保持停止，协调 headless 已正常结束；本次冻结审核时 R7 仍未提交、推送、进入远端 CI 或合入，0036 保持 Approved。本机回归及提交前九项总审核已完成，代码与文档按相邻提交准备。

2026-10-21 本地提交准备完成：实现与配套测试提交为 `86d77156435f135937cc75d9dc6358e3afebb6e5`，文档与其同批相邻提交。提交前九项总审核逐条核实，状态词修正和提交事实均为总审核按本轮命令落笔；正式文档的实质内容已由同一 dsh 会话冻结核对。R7 尚未推送、创建 PR、进入远端 CI 或合入；0036 保持 Approved。低优先级 EOF/EINTR 和故障测试符号覆盖留作发布后 follow-up，源/目标的并发干扰与断电仍不在离线迁移验收内。

### R7 发布记录（2026-10-21）

所有者批准推送、创建 PR 和运行远端 CI 后，实现 `86d7715` 与相邻文档 `df63004` 已发布为 [PR #82](https://github.com/YanBo-maker/YanOS/pull/82)。父 [PR #81](https://github.com/YanBo-maker/YanOS/pull/81) 仍待合入，因此 R7 以 `codex/0036-r4-r6-retrieval` 为临时 base，仅展示本阶段改动；父合入后再调整 main。此前“尚未推送”的段落保留为本机收口时的事实。

发布时三个远端任务已触发；当前精确 head 的完整远端 CI 记录（注册与通过数、CTest 耗时及原始任务日志）待 CI 完成后由总审核更新到 PR 与 MONITOR，不能由本机结果推定。工作流未配置 artifact 上传，不将本机日志冒充 CI artifact，也不把运行中的任务当成通过。R7 仍待合入，0036 保持 Approved。

两个非阻塞后续项已实际建立并标记 `follow-up`：[EOF/EINTR #83](https://github.com/YanBo-maker/YanOS/issues/83)、[故障测试符号覆盖 #84](https://github.com/YanBo-maker/YanOS/issues/84)，均关联 PR #82。未把它们写成已修复功能；本次发布记录只更新真实 PR、CI 入口和后续项，源码、接口、验收行为及非文档输入保持。



### R4/R6 与 0035 主线的整合验证（2026-10-23）

本轮将 PR #81 的 `131a39b` 与 main `0948b94` 整合，复用原短期工作树，新建本地任务分支；不改变文件名、磁盘格式、来源身份或 MCP schema。生产源码与 `131a39b` 相同，主线的学习记录夹具、Guest 联合验收与写工具文档检查完整保留，CMake 同时注册两组能力。新主线 `0948b94` 的三项 CI 与实际任务日志已核对，该证据只证明主线基线，不代替本轮整合回归。

首份候选树的当前实测如下，代码和测试输入共 354 项，在这些运行后逐项保持；旧的 2026-10-21 记录仍保留其原提交范围。

| 首份候选配置 / 命令 | 注册 / 实际结果 | CTest 实耗 |
| --- | --- | --- |
| Linux Release，`ctest --test-dir <chief-release> --output-on-failure -j3` | 144 / 144 通过 | 547.75 s |
| Linux Debug ASan/UBSan，同上 `<chief-asan>` | 144 / 143 通过，1 失败 | 911.56 s |
| Windows MSVC Debug，同上 `<chief-windows> -C Debug` | 79 / 79 通过 | 197.50 s |
| Linux Release 实际变异，`<chief-mutation> -R '_mutation$'` | 注册 162，选择并通过 17 | 584.44 s |

均无跳过。首轮 ASan 的失败仅来自 `mcp_faults` 中 `test_block_busy_cancel_and_reuse` 等待 MCP 帧的 harness 超时，日志未报告 sanitizer 错误；其余 143 项和该配置下的六项大容量 AC 均通过，不能把这次运行写为全绿。没有改动 MCP 代码、测试或超时设置，单独复跑该套件 1/1（11.34 s）通过；诱因尚未确定，不宣称修复了该超时。另行执行仅在变异配置中注册的 `guest_console_mutation_gate`，1/1（86.76 s）通过，不计为第十八组实际变异。

独立 dsh 的集成检查确认两个父分支的实现及测试均保留，未发现合并引入的生产代码缺陷。核对 R4-AC3 时发现大库检查只断言条数和 total，因此由原编码者在 `tests/run_large_capacity.py` 加入两类反链的实际名称序列断言：1000 个来源仍只输出 20 项，且必须依次为 `n0000.txt`…`n0019.txt`。保留原条数、total、shown 和回执检查，并在案例证据中保存实际名称。

此次补强只改一个 Linux 验收脚本，其余 353 项输入保持，生产源码、CMake、Windows 输入及实际变异判据未改。总审核使用实际运行保存的输出，保持条数和 total，分别替换或倒序普通反链、块反链；精确原脚本对四种错误输出均接受，新脚本均在 ASCII-first 断言拒绝，正确输出两者均通过。它们是受控输出检查，不混算十七组实际变异。

补强后的 Release 只复跑 `large_capacity_gate` 与 `yanfs_large_capacity`，2/2（88.78 s）通过，六项 R4/R6 案例和两组前 20 名称均已核对；随后对最终候选完整复跑 ASan/UBSan，144/144（365.03 s）通过，包含 mcp_faults 和新名称断言，均无失败或跳过；没有修改 MCP 代码、原测试或超时设置。首轮超时在单独和本次完整复验中均未重现，诱因仍未确定。默认验收使用确定性 provider 核对容量、结果和故障边界，本轮未启动真实模型；不把旧真实模型效果记录或上述故障重跑冒充新的完整模型验收。

状态文档合并保留 main 历史全文前缀、PR #81 的独有历史追加和旧 main 当前阶段原文；不改写原历史标题。README 和 STATUS 保留 0035 已合入能力，说明本分支的大容量范围；0036 状态仍为 Approved，R7 仍在后续 PR #82。测试细节仅记在本节，STATUS 不新增套件数或耗时。

证据位于 `E:/YanOS/build/20261023-r4-r6-integration/`：`initial-results.json` 与 `initial-*-ctest.log` 保留首轮结果；`isolated-mcp-faults.log`、`console-gate-ctest.log`、`post-release-ctest.log` 分别记录重跑范围；`post-asan-ctest.log` 与 `post-asan-LastTest.log` 记录最终完整复验；`ac3-detection.json`、`post-release-large-capacity-evidence.json` 和 `post-asan-large-capacity-evidence.json` 记录检测力与实际名称；`frozen-inputs.json`、`post-assertion-inputs.json` 划分修改前后输入。原独立只读 dsh 已完成对当前差异、实际源码与接口、当前验证证据和文档的对齐审核，无剩余阻塞项；原历史、首轮失败和重跑日志保持。总审核逐提交执行九项检查，合并原 main、补强测试和验证文档分别提交，保留两个父分支的已发布历史。本次整合的提交、发布、远端检查和合入状态统一以 PR #81 的实际记录为准，不以旧 head 的 CI 代替新提交检查。


### R7 与新父分支的整合验证（2026-10-23）

本轮将原 R7 `37be40d` 与已验证的父分支 `f10516e` 整合，保留两个已发布父历史。迁移模块、薄 CLI 及已有生产源码均与原 R7 相同，保留新主线的 0035 学习记录、写工具文档核对及父分支大容量验收；不改变磁盘格式、来源身份、MCP schema 或迁移边界。旧 R7 和 R4/R6 VERIFY 原文保留，各自只证明对应历史树。

首份候选冻结 360 项非文档输入。Release 为 145/146（362.41 s），`mcp_sdk` 在 PID 文件已创建而内容尚未写完时读取空串失败；同次实际变异为 16/17（491.95 s），`mcp_mutation` 的生命周期检查发生帧等待超时，按 harness error 处理，不能记为检测成功。先补 SDK 读取等待及子进程 hold 就绪同步后，中间候选 Release 146/146（311.43 s）通过，实际变异仍为 16/17（439.40 s）：另一个消费者在 `test_crashed_worker_reaps_all_generations` 读取同一测试 runner 的空 PID。两次实际变异均未全绿，后续控制台门禁当时未执行。原始失败及中间结果全部保留。

最终采用共同生产者修复：`tests/semantic_fake_runner.py` 的两处 PID 发布均先在同目录临时文件写完整内容，再替换最终路径；SDK 的临时读取补丁撤回，恢复两个父版本的原文件，避免在消费者中重复等待逻辑。新增 Linux 默认测试 `semantic_runner_pid` 实际运行版本探测和服务启动入口，在文件打开后、字节写入前检查最终 PID 路径不可见，并核对完整 PID 和临时文件清理。精确旧 runner 在该断言失败，当前 runner 通过；该测试不启动 HTTP 服务或真实模型。缺少测试夹具明确失败。

子进程就绪问题另有受控复现：旧测试仅见父进程 fork 记录便发送请求并取消，子进程可能尚未进入 hold；注入 50 ms 子进程延迟时，旧测试在后续请求等待中超时。故障 hook 现在在实际进入 hold 后记录该子进程 PID，测试等到此标记才继续原 BUSY、取消和回收检查；相同延迟下当前测试连续三次通过。原请求、取消与帧等待时限保持，不修改 MCP 生产代码。此控制解释了可复现的同类竞态，不将所有历史超时追认成同一原因。

R7 验收补齐两类真实断言：正常迁移及已进入来源锁路径的失败后，`realpath(OLD)+".lock"` 必须保留；OLD rebuild 后的盘上语义索引 SHA-256 在 NEW status 及 OLD/NEW search 后必须保持。总审核用实际 CLI 包装候选删除旁锁、用实际查询候选保持输出却改写索引尾字节；精确旧验收脚本接受三种坏候选，新脚本分别拒绝，正确候选两者均接受。这些定向控制不计入十七组实际变异，迁移矩阵仍为 74 项。

最终冻结 361 项非文档输入；相对首份候选，355 项不变，五个已有输入修订，新增一个测试。生产源码全部保持。下列完整运行全部使用最终输入，不把首轮、中间候选和定向复跑拼成最终通过数。

| 最终配置 / 命令 | 实际结果 | CTest 实耗 |
| --- | --- | --- |
| Linux Release，`ctest --test-dir <chief-release> -C Release --output-on-failure -j3 -E '_mutation$'` | 147/147，零失败、零跳过 | 359.09 s |
| Linux Debug ASan/UBSan，`ctest --test-dir <chief-asan> -C Debug --output-on-failure -j3 -E '_mutation$'` | 147/147，零失败、零跳过 | 389.97 s |
| Windows MSVC Debug，`ctest --test-dir <chief-windows> -C Debug --output-on-failure -j3 -E '_mutation$'` | 79/79，零失败、零跳过 | 177.48 s |
| Linux 实际变异，`ctest --test-dir <chief-mutation> --output-on-failure -j3 -R '_mutation$'` | 17/17，零失败、零跳过 | 509.18 s |

Linux 默认注册 147 项，变异配置总注册 165 项、只选择十七个实际组；Windows 注册 79 项，不运行 Linux 迁移工具。额外执行 `guest_console_mutation_gate`，1/1（143.98 s）通过，不计为第十八组实际变异。ASan/UBSan 在其他本轮完整运行结束后单独启动。Release 和 ASan 的迁移矩阵均为 74 PASS / 0 FAIL / 0 SKIP，工作目录门禁、六项 R4/R6 大库案例及反链前 20 名称断言均通过，学习记录、文档核对、官方 SDK 与生命周期测试均在完整默认套件中实际执行。工作目录门禁内部的可选基础子集不冒充完整迁移验收。

R7-AC1 仍逐项比较 OLD/NEW 的槽号、名称、字节内容、空槽、尾零和来源摘要；内存词项索引在新进程重建后比较真实查询结果，盘上语义索引在 NEW 为 READY 且无需重建。R7-AC2 继续核对预存目标保护、特殊附件拒绝、创建后各阶段故障和清理残留路径。默认语义验收使用确定性 provider；本轮没有新增真实模型效果验收，历史固定语料的真实索引复用记录保持原范围。另用 `YAN_BUILD_TOOLS=ON / YAN_BUILD_SEMANTIC=OFF / BUILD_TESTING=OFF` 构建当前 `yan_migrate` 并检查动态依赖，迁移目标不依赖模型、HTTP 或 JSON 库；此构建检查不冒充完整 AC1。

文档合并保留父分支历史全文前缀、R7 独有历史，以及父分支旧「当前阶段」原文；0036 保留 R7 全文前缀及父分支独有 VERIFY 后追加本节。README 与 STATUS 仅更新当前整合状态、能力及边界，不复写本节计数与耗时。0035、0036 保持 Approved；[EOF/EINTR #83](https://github.com/YanBo-maker/YanOS/issues/83) 与[故障符号覆盖 #84](https://github.com/YanBo-maker/YanOS/issues/84) 仍为后续项。离线稳定、普通附件及无断电恢复的已批准边界保持。

证据位于 `E:/YanOS/build/20261023-r7-integration/`：`initial-*`、`intermediate-*` 保留失败与中间运行；`release/asan/windows/mutation-ctest.log`、各自 `*-LastTest.log`、`*-registration.json`、`*-execution.json` 及 `verified-results.json` 对应最终完整结果。`release/asan-large-capacity-evidence.json` 保存实际大库案例，`mutation-console-gate.log` 单列控制台门禁。`pid-publication-old/new.json`、`block-diagnostic-delayed-old/new-results.json`、`migration-detection-old/new.json` 保存定向检测力；`frozen-inputs.json`、`final-inputs.json`、`document-merge.json` 划分输入与历史保留范围。

本轮 dsh 使用同一协调器、一个编码者及一个独立只读审核者，最多三个会话；正式审核上限两轮，传输重试上限一次且已在准备阶段使用，不重启旧实验。编码者未修改正式文档，文档始终由总审核修改；角色边界与任务目录检查记在本轮总审材料。

独立只读 dsh 已用同一会话完成一轮正式冻结审核，核对当前源码、接口、AC 断言、实际运行证据及文档，无剩余阻塞项；提交前九项总审核已逐项签署。必要测试修复随两父整合提交，验证文档同批相邻提交，不产生已知失败的测试中间树。本轮更新尚未发布，新 head 的远端 CI 尚未运行，PR #81 与 #82 尚未合入；不归档或重置监控通道。


### PR #81／#82 合入与全面文档核查（2026-10-23）

项目所有者明确授权合入后，PR #81 以 merge commit `79c8e440383be94038b26632a4b12af0cae42399` 合入 main，父提交为 `0948b94` 与 `f10516e`。随后 PR #82 改以 main 为 base，并以 `26c25fca31ef2dd6edf0049e8bdf3af74d3f3f6e` 合入，父提交为 `79c8e44` 与 `1150c9a`。两次保留已发布历史。最终 main 的 tree 为 `ee69a593279a2681738d703f219246109da8e89b`，与已审 `1150c9a` 完全一致；上文整合时的“尚未发布／合入”保留为历史快照。

PR #82 精确 head `1150c9a` 的 [Actions 39922221554](https://github.com/YanBo-maker/YanOS/actions/runs/39922221554) 已核对成功；合入后的精确 main `26c25fc` 另经 [Actions 39927592436](https://github.com/YanBo-maker/YanOS/actions/runs/39927592436) 验证，不使用父分支或 PR 合成提交代替本次 main 结果：

| main 配置 | 默认 CTest 实际结果 | CTest 实耗 |
| --- | --- | --- |
| Linux Release | 147/147，零失败、零跳过 | 552.07 s |
| Linux Debug ASan/UBSan | 147/147，零失败、零跳过 | 1341.36 s |
| Windows Debug | 79/79，零失败、零跳过 | 254.90 s |

两个 Linux 任务各额外运行 MCP 四项，均 4/4。原始日志的检出提交、测试名集合、汇总与跳过判据均已核对。CI 仍未开启实际变异；十七组实际变异、迁移矩阵与真实模型记录分别保持上文的运行范围，本次未重跑真实模型。原始 CI 日志与核对数据在 `E:/YanOS/build/20261023-merge-doc-audit/main-ci/`、`main-ci-verified.json`；前一精确 head 的证据在 `20261023-r7-integration/published-ci-verified.json`。

本轮总审核使用 style-calibrator 审校读者文档，安排一个 dsh 协调器与两名独立只读审核者，覆盖全部 50 份受跟踪 Markdown，并逐文件记录修改或保持的理由。读取当前源码、接口、测试断言与匹配本树的原始验证证据，核对范围不限于文档之间的一致性。生产代码、测试、所有者原输入和已确认试用镜像保持；学习笔记 Markdown 是精确字节夹具，不作润色。

机械修正包括合入状态、0034 自身 AC9.6 要求的 Implemented 状态、v1 单块空镜像边界、MCP 工具归属、空文本预览的零起始行号、锁路径与内容身份的区别、验证记录入口、历史规格头字段例外及错误规格编号；不改变接口或已批准行为。旧「当前阶段」原字节追加到 STATUS-history，旧 VERIFY 全文前缀保持。合入后的 MONITOR 通道已归档并核对 SHA-256，入口与回执保留在仓库外。

R8-AC1 的历史全量跨版本比较已实际执行，但 `ring_copy`／`os_memory_helpers` 的标签只证明组件断言，不等于完整 parity 随当前默认 CI 重跑。当前库内缺少该全量重跑入口，见 [follow-up #89](https://github.com/YanBo-maker/YanOS/issues/89)。本轮读取并核对外部 parity 文件，没有重新执行旧基线比较；后续批准的 backlinks 输出和 v2 成功检索不能要求与 R8 前基线无条件相同。N=4095 的容量与内存支持不表示性能验收通过；仓库未包含所引用的 0040，相关计划与验收缺口见 [follow-up #90](https://github.com/YanBo-maker/YanOS/issues/90)。0036 整体保持 Approved，R5/R8 原条款不改。

针对接口说明，使用已构建的实际 `yan_mcp` 和 `yan_mkfs` 在独占临时镜像核对：空文本 create 预览返回 `start_line:0` 与两个空数组；字节相同的副本来源摘要相同、锁路径不同；预览前后镜像字节保持；`--files 63 --blocks 1` 成功生成 4096 字节 v1 镜像；实际 tools/list 与文档结构比较通过。首次临时检查误用不存在的 `--output`，工具按用法拒绝且未执行该项验收；改用现有 `--image` 后上述检查通过，不涉及生产修复。定向结果见 `doc-examples.json`，不冒充完整回归。


针对当前文档另执行 Linux Release 定向 CTest `mcp_note|mcp_write_docs`，2/2（0.90 s），零失败零跳过；纯检查器自测 18/18。此次检查对应修后文档，不把主线 CI 的旧文档版本当作本次文档验收。静态检查覆盖 50 份 Markdown 与 35 份规格，未发现本地链接或锚点错误，STATUS 小于 16 KiB。检查脚本首次经 WSL 直接传含括号正则被 shell 拒绝，未启动 CTest；使用 argv 形式后完成上述运行。证据 `doc-ctest.log`、`doc-python.log`、`doc-checks.json`。


独立只读 dsh 复用原协调器与两名审核者完成两轮；第二轮仅复核首轮发现及修正，无剩余阻塞项。总审核完成九项提交前检查。全部生产及测试输入保持，本轮仅提交文档修正；该文档提交与其后发布、远端检查状态另记，不把 main 的 CI 结果冒充未发布文档分支 CI。


### PR #91 文档核查合入回执（2026-10-23）

项目所有者直接授权后，文档修正 PR #91 以 merge commit `9d8cf9bc848b5a594acf12649dfebafeaaf3ae37` 合入 main，保留 `ec5b497` 的历史；merge 的 tree `6ed767868ea55c2e4fabe14d39516972f7c10221` 与该已审文档提交相同。上文“本轮仅提交文档”的待发布口径保留为当时记录。

该精确 main 的 [Actions 39960213984](https://github.com/YanBo-maker/YanOS/actions/runs/39960213984) 已核对成功：Linux Release 147/147（544.92 s）、Linux Debug ASan/UBSan 147/147（1339.56 s）、Windows Debug 79/79（237.67 s），均零失败、零跳过；两个 Linux 任务各额外 MCP 四项通过。检出提交、测试名和原始日志摘要已核对。此记录对应主线基线，后续 #89 的本机验证另列。

### R8-AC1 仓库内重跑入口（#89，2026-10-23–24）

本轮补齐上文“当前库内缺少该全量重跑入口”所述的当时缺口，沿既有 R8-AC1 增加受跟踪的入口，不改变生产代码、盘上格式、文件命令或 MCP schema。基线固定为 `4c5e0ab1a036e845123e304333d363478243c146`（tree `eb284e7cf94c2e4e7aa442583004f1041e8beb6f`），R8 候选固定为 `2b82d17065bbd5b83e2d5e2f201f37f5308dedf5`（tree `a6daeffc8a713dfaf3ca1b249a43f7d6aa14d953`）。后续 R4 的普通 backlinks 输出与 R6 的 v2 Host 支持沿各自已批准规格；本入口的 `current_main_compared` 为 false。

实现分为纯 Python 夹具/比较器 `tests/yanfs_r8_parity.py`、Linux/WSL 操作员入口 `tests/guest/run_yanfs_r8_parity.py`、默认纯控制 `tests/test_yanfs_r8_parity.py`。已有终端 runner 仅增加可选末尾 timeout 参数，原默认值保持；CMake 只新增 `yanfs_r8_parity_harness`，标签为 `issue89;0036-R8-AC1-harness`，原组件标签保持。

入口定义 24 组正常终端比较、6 组 CRC 有效的重名/重叠拒挂比较、3 组 MCP 比较，共 33 对、66 次产品执行；首尾相接的 extent 由正常满目录挂载与独立解码检查共同覆盖。每份镜像的物理文件名顺序为 f0000 后接其余名字的逆序，物理中间槽保存 60 个 a，其余文件保存既有 65 字节笔记。普通 related/块反链按 ASCII 文件名检查；search 沿 0026 的同分物理槽、行号规则，显示前十个物理文件各两行，共二十行。夹具编码、CRC、数据内容、连续 extent 和名字排列均检查；新夹具哈希与旧的顺序排列私有夹具不同。

终端完整原始输出逐字节比较，同时核对正确命令回执、返回码、Guest tohost、交付字节数和前后完整镜像；MCP 比较原始单行字节并核对结构、真实来源名和物理位置。N=63 的 MCP 应成功，total=62、shown=20；N=1023/2047 的两次历史 MCP 比较应均为 CORRUPT，归类为错误保持。目录与模型身份变化后的当前主线行为不由这组旧版本比较推定。

缺命名仓库输入、固定提交、成员清单或生成夹具自检不符返回 1；外部工具或 CURL/yyjson 缺失返回 77 并记录 NOT VERIFIED；构建、超时、信号或 sanitizer/runtime-error 诊断属于 harness 失败 2。普通 stderr 文本保留，不单独判失败。终端每次执行默认 900 s、最多 3,000,000,000 步；MCP 帧等待 60 s，正常关闭等待 12 s，终止后再等待至多 5 s。工作目录须为空或不存在，拒绝既有目录时不改写原 manifest。入口从固定 Git 归档重新构建，不复用历史 ELF；保存工具版本、源码和产物身份、命令、已返回进程的原始输出与退出状态，以及失败时已完成范围。构建子进程在返回前超时仍归为 harness 失败并记录原因，此时没有该次构建的完成行。

以下全量结果对应 schema 错误路径修补前冻结的 364 项非 Markdown 输入。Linux Release 默认 CTest 为 148/148（221.16 s），Debug ASan/UBSan 为 148/148（1043.40 s），Windows MSVC Debug 为 80/80（143.26 s），均零失败、零跳过；新增默认门禁包含 28 项纯控制。实际命令与原始记录在 `E:/YanOS/build/20261023-r8-parity-repro/` 的各配置 execution、registration、ctest 和 LastTest 文件中。

本轮受控反例覆盖两边同错、空输出、错误回执/返回/状态、改镜像、错误 MCP 字段或字节格式、缺命名输入、不完整/重复矩阵及已有工作目录保护。另以真实 MCP 包装程序注入预设诊断：最终 dsh 草稿接受该反例，总审核修复 stderr 提前读取绕过检查的问题后，响应前与退出时的诊断均归为 harness 失败，原始 stderr 保留。此控制只验证判定器，不是产品出现 sanitizer 错误；来源与前后字节记录位于本轮私有交付材料。

完整历史命令为 `python3 -B tests/guest/run_yanfs_r8_parity.py --source . --work <fresh-work> --cc cc --gcc riscv64-linux-gnu-gcc --deps <local-prefix>`。本次从固定 Git 对象重新归档、构建后，33 对 / 66 次执行全部通过，耗时 2294.68 s；24 对正常终端、6 对拒挂、3 对 MCP 的确切集合、原始输出和镜像字节均核对。MCP 为一档成功和两档预期错误保持，来源/产物身份取自本次 manifest；没有使用旧私有捕获或其哈希作为通过常量。manifest 的 SHA-256 为 `3e289f1453edf48795bc094cb774969536a5a2542a4577ff23a2343d3e2c14d0`。

| 本轮命令 / 配置 | 实际选择与结果 | 实耗 |
| --- | --- | --- |
| Linux Release，`ctest --test-dir <root-release> -C Release -j3 -E '_mutation$' --output-on-failure` | 148/148，零失败、零跳过 | 221.16 s |
| Linux Debug ASan/UBSan，`ctest --test-dir <root-asan> -C Debug -j1 -E '_mutation$' --output-on-failure` | 148/148，零失败、零跳过 | 1043.40 s |
| Windows MSVC Debug，`ctest --test-dir <root-windows> -C Debug -j3 -E '_mutation$' --output-on-failure` | 80/80，零失败、零跳过 | 143.26 s |
| Linux 实际变异，`ctest --test-dir <root-mutation> -j3 -R '_mutation$' --output-on-failure` | 17/17 实际组，零失败、零跳过 | 491.01 s |
| 同一变异配置，`ctest --test-dir <root-mutation> -R '^guest_console_mutation_gate$' --output-on-failure` | 单独 1/1；不计为第十八个实际组 | 132.56 s |

变异配置实际注册 166 项，上表只选择十七个实际组并另跑控制台门禁；没有声称完整运行该配置的全部默认项。各组最终 footer 与指定 owner/断言、基线/no-effect/错误 owner 等控制均核对，存活为零；timeout、信号、sanitizer、构建失败不计检出。默认门禁和实际变异保留独立范围。Release/ASan 各自的迁移验收仍为 74 PASS / 0 FAIL / 0 SKIP，现有大容量、学习记录、官方 SDK、生命周期和写工具文档测试均在本次完整套件中执行。默认语义测试沿用确定性 provider，本轮历史 MCP 比较不使用模型，也不新增真实模型效果结论。

上述全量运行均使用修补前同一份已冻结的 364 项非 Markdown 输入。ASan 在 Release/Windows 完成后以单个 CTest 任务运行，历史比较仍有一个执行进程；本轮实际变异在 ASan 结束后启动。实耗只记录本次运行成本，不作为 Host 性能基准。另对实际命令入口验证已有 manifest 的零改写、缺仓库输入先于外部依赖失败，以及外部缺失的 NOT VERIFIED 清单。补充 Linux 实进程检查确认缺 Host 编译器、缺依赖前缀均返回 77、完成数为零；未单独模拟前缀存在但内部 CMake package 缺失。本次使用显式 Git 工作树定位，未另行全量验证默认 .git 解析入口。原始日志、注册集合、每项通过名、产物与输入摘要已相互核对；未将首轮红色控制、历史私有结果和当前通过数拼接。

仓库内的完整证据在 `build/20261023-r8-parity-repro/root-full-historical/` 的 manifest、归档源码、构建日志及 runs/cases 原始文件中；本轮总审材料在 `E:/YanOS/build/20261023-r8-parity-repro/`，包括 `verified-results.json`、`historical-green-verified.json`、各配置原始 CTest/LastTest/execution/registration、`candidate-frozen-inputs.json`、`entry-controls-verified.json` 和受控诊断的前后记录。旧 R8 私有材料中的候选产物曾被后续构建覆盖，本轮重新建立固定提交到新产物、输入和输出的证据链；上文历史结论原文保持。

本轮只读独立审核复用原 dsh 会话，上限两轮，核对当前差异、实际源码/接口、上述本轮证据与文档；结论与总审核九项检查随私有交付材料记录。总审核独占正式文档修改权，旧「当前阶段」原字节追加到 STATUS-history，历史规格与历史 VERIFY 前缀保持。本阶段尚未发布，远端检查与合入状态以关联 PR 为准；0035、0036 整体保持 Approved，#90 的 4095 档性能范围保持。


总审随后以真实 stdio 子进程返回 `{}`、`[]` 和缺 result 的 JSON，发现共享 schema 辅助函数的 KeyError/TypeError 会逃出入口，虽不误报通过，却未形成最终失败清单。原 dsh 编码会话将这些 schema 异常限定转换为 MCP_SCHEMA，补充跨平台控制并保留原始响应、stderr、退出状态；sanitizer 与传输故障优先级保持。最终仅 `tests/guest/run_yanfs_r8_parity.py` 与 `tests/test_yanfs_r8_parity.py` 两项非 Markdown 输入相对上述全量冻结有变化。

修后在 Release、ASan、Windows 三个已有配置分别定向运行 `yanfs_r8_parity_harness`，各 1/1，门禁含 29 项纯控制；另经真实异常响应及受控 main 路径核对 MCP_SCHEMA、exit 1 与部分失败 manifest，并重验早/晚诊断。使用全量运行产物中已核对哈希的两个固定历史 MCP 二进制，修后重新执行三档 MCP，共 3 对 / 6 次，均符合 v1 成功、v2 错误保持。此为定向后验，不是重新构建的完整 33 对，也不替代修补前的三平台完整回归或十七组实际变异。修后源码摘要、命令和原始记录见 `post-schema-verification.json`、`r1-boundaries-post2/` 与 `post-schema-mcp-v2/`；先前日志与 manifest 原样保留。

### R7 Issue #83：附件EOF探测的EINTR处理（2026-10-27）

本次修复沿用已批准R7的成功比对和失败清理契约，跟踪于[Issue #83](https://github.com/YanBo-maker/YanOS/issues/83)。附件按已知长度比对后，原两次1字节EOF探测把信号中断也判为VERIFY。`host/image_migrate.c`现通过局部静态助手，仅对`read`返回`-1/EINTR`重试同一fd的一次读取；返回0才算EOF，尾字节与其他读取错误仍为`YAN_IMAGE_MIGRATE_VERIFY`，保留`asset '<name>' size changed during verification`及既有清理。依据[Linux read(2)](https://man7.org/linux/man-pages/man2/read.2.html)，EINTR发生在读出任何数据之前。接口、错误分类、格式、来源身份、旁锁和写行为保持；没有增加整次迁移重试、快照或回滚。

本节取代2026-10-21记录中“附件末尾EOF探测遇EINTR可保守失败”的当前限制；原文及后续待办记录保留为当时的历史事实。0036整体保持Approved，故障符号覆盖[#84](https://github.com/YanBo-maker/YanOS/issues/84)和并发干扰、断电恢复不在本次范围。

独立默认测试`yanfs_migrate_eintr`使用实际源/目标附件目录树、精确文件名、验证阶段、单字节读取和`offset==st_size`锚定故障，不按全局读取次数猜测。标记带角色、阶段、偏移、大小和注入次数；未命中或次数不符必须失败。固定附件包含空文件和20000字节文件。修后专项共22项：健康CLI一次、EINTR后EOF成功八项、直接尾字节/EIO失败八项、EINTR后尾字节/EIO失败四项、标记缺失负控一项。故障驱动器断言库结果VERIFY（枚举8）及其退出码9；它没有覆盖真实CLI同类故障的stderr前缀，真实CLI只在健康案例及原迁移矩阵中运行。

先使用精确`d2113d8:host/image_migrate.c`构建旧实现，八项EINTR案例均复现原VERIFY/9及目标清理；这是预期RED，不是当前正常回归失败。修后22项通过，原迁移矩阵在语义配置下74 PASS / 0 FAIL / 0 SKIP，最终定向CTest三项通过（24.86s）。最初未开启语义组件的运行是73 PASS / 1 SKIP，仅保留为诊断；初期夹具与标记判据修订前的日志也保留，不拼入最终通过数。原74项与新增22项分别计数。

总审核重读原始RED/GREEN退出状态、消息、故障标记、二进制摘要和实际镜像/附件，确认成功内容一致，失败目标及附件清理，OLD字节和旁锁保持。另在build下的独立目录生成两个实际坏候选：只重试一次被四个连续中断断言拒绝；把非EINTR错误当EOF被六个EIO断言拒绝。两者均构建成功、以具名断言失败退出1；不把构建失败、超时、信号或sanitizer当检出，也不计为第十八组实际变异。

<!-- issue83-current-validation -->
当前冻结实现的完整本机验证如下，均零失败、零跳过；每次配置、构建及执行前后核对同一370项非Markdown输入，未发生变化。

| 配置 | 注册数 | 实际通过 | CTest实耗 |
| --- | ---: | ---: | ---: |
| Linux Release | 151 | 151/151 | 326.03s |
| Linux Debug ASan/UBSan | 151 | 151/151 | 1196.69s |
| Windows MSVC Debug | 81 | 81/81 | 136.60s |
| Linux实际变异 | 169 | 17/17 | 593.15s |

Linux开启TOOLS/SEMANTIC，默认选择`ctest -E '_mutation$' --no-tests=error --output-on-failure`（Release -j3，ASan -j1）；Windows为MSVC Debug、无Linux工具，-j3。实际变异配置注册169项，只选择`-R '_mutation$' -j3`的十七个实际组，不宣称运行该配置全部默认项。另单独串行运行`guest_console_mutation_gate`，1/1（137.76s）通过，不计为第十八个实际组。新专项的22项、原迁移74项、两个坏候选及17组实际变异各自保留范围；真实模型效果、历史R8全量比较未重跑。

保留的验证诊断包括：首批Release/Windows提前配置时最终测试脚本仍修改，两者均被输入守卫中止，只完成配置、未执行测试；总审核随后误重复启动已包含在原脚本中的控制台门禁，停止重复任务，保留终止和重叠记录，再完成上述独立串行复核。原17组LastTest在额外门禁之前已复制保留；重叠门禁日志不作为最终证据，终止记录不算产品回归失败，也不算检出。最终控制台证据仅为`final/console-serial-*`。

dsh协调器使用一名编码者与一名独立只读审查者，最多三个会话；编码者已完成实现并停止写入。独立预检不算正式审核；同一只读审查者已完成正式R1，PASS且零阻塞，未使用第二轮或传输重试；原两个子任务已inactive，协调headless已退出，不重启归档实验。本轮四项0040合入文档同步由总审核落笔，dsh发现这些并发改动后保持原样；新阶段旧STATUS当前段落原字节追加到历史。实现/测试与文档按同批相邻提交收口，具体提交以本轮交付材料为准；尚未推送、创建本次PR或运行新head远端CI。

证据入口：`E:/YanOS/build/20261027-migration-eintr/`的`handoff-root-verified.json`、`detection-root-verified.json`、`final/*-execution.json`、`final/*-root-verified.json`和原始CTest/LastTest/注册集合；当前检出`build/20261027-migration-eintr/`保留dsh的`implementation-ready.md`、`manifest.json`、`issue83/red/`、`issue83/green/`、`issue83/green-semantic-ctest-final.log`及两次协调器重跑。真实模型效果、历史R8全量对比未重跑。旧验证日志只证明各自历史树，本次未把它们计入新结果。

总审核按独立结论完成审核状态的机械同步，生产与测试370项输入保持；R1报告为当前检出`build/20261027-migration-eintr/formal-r1-report.md`，SHA-256为`19d1fc0c3677b5e320ddf76a28eaf74195304e6d88848317456684336cc9522d`。本次提交前九项总审核与文件归属见本阶段`audit-code-nine-checks.json`及`audit-docs-nine-checks.json`；审核前的冻结文档摘要保留于`final-delivery-inputs.json`，状态同步另记于`closeout-document-state.json`。

本轮本地实现/测试提交为`85ef0395b5282070731cc0ab709caec1300cab91`，文档与其同批相邻提交；提交状态以交付材料为准，尚未发布。

2026-10-27所有者随后批准推送`codex/83-migration-eintr`、创建PR与运行远端CI。实现/测试`85ef039`和文档`db62cd6`保持，发布状态同步仅记录此次授权；实际PR及其CI、可用工件见[Issue #83的关联PR](https://github.com/YanBo-maker/YanOS/issues/83)。此前“尚未发布/推送”的文字保留为本机收口时的快照，不能用0040的CI代替本次新head检查。本次仍沿0036-R7规格通道修复实现，EINTR从误拒绝变为重试成功是可见修复，不归入“不改用户可见行为”的轻量通道；没有新增接口或改变R7不变量。


### 2026-10-27：Issue #88 镜像符号链接入口的附件定位验收

本轮为既有0036-R7及2026-10-21附件澄清补直接测试，依据仍是`realpath(OLD) + ".assets"`与[0003决策](../decisions/0003-file-limit-and-images.md)。分支基于main `552dcde`；生产C、公开接口、Guest、磁盘格式及原错误边界保持。附件EOF/EINTR修复位于独立的[PR #94](https://github.com/YanBo-maker/YanOS/pull/94)，不将其新增用例计入本轮。

新增`tests/run_yanfs_migrate_symlink.py`复用原独立Python镜像解码与附件oracle，通过真实`yan_migrate`执行三个场景：相对链接指向v1镜像、绝对链接指向含slot 1000的v2镜像、真实附件缺省但入口旁存在诱饵。前两者逐字节及目录结构比较真实附件，第三者要求目标没有附件目录；同时核对slot/name/size/content、0033来源身份、源镜像与两份附件保持、链接目标保持，以及真实路径旁锁存在而入口旁无锁。CMake在既有Linux工具条件下注册`yanfs_migrate_symlink`，标签为R7-AC1和issue88；它不直接验收AC2错误清理，没有SKIP_RETURN_CODE。Windows不执行此Linux迁移场景。

正确生产CLI的三个新场景共61项检查通过。实际私有错误候选仅将附件选择从`old_real`改为`from`，编译成功，三次迁移均以0退出且打印正常成功信息；新runner分别由`assets.canonical-manifest-equal`（两次）与`assets.target-absent`（一次）拒绝。该路径在首个附件断言停止，已执行35项检查；其后来源保持与锁断言没有执行，不能记为通过。这是错误成功输出的实际检测证据，不把构建失败、超时或信号当检出，也不称原生产实现存在RED故障。缺显式CLI或oracle的控制硬失败，且未创建工作目录；总审核另以缺CLI和缺oracle命令独立核对退出1与零工作目录。

最终370项非Markdown输入在各配置的配置、构建、注册与执行前后均一致；配置/执行原argv、原始日志和总审核验保存在`E:/YanOS/build/20261027-migration-symlink/`的`*-execution.json`、`*-root-verified.json`与`handoff-root-verified.json`。本机最终结果：

| 配置 | 注册数 | 选中并通过 | CTest耗时 |
| --- | ---: | ---: | ---: |
| Linux Release | 151 | 151/151 | 396.82s |
| Linux Debug ASan/UBSan | 151 | 151/151 | 1291.19s |
| Windows MSVC Debug | 81 | 81/81 | 152.00s |
| Linux Release 实际变异 | 169 | 17/17 | 626.21s |

以上最终执行均零失败、零CTest跳过。两个Linux默认配置均开启TOOLS/SEMANTIC，保留原74项迁移专项的完整通过结果，新三个场景单独统计；Windows为MSVC Debug。十七组实际变异之外，`guest_console_mutation_gate`另以串行入口通过1/1（131.41s），不计第十八组。旧1023/2047门槛及4095默认性能门禁随完整Linux回归执行；未重跑真实模型效果或历史全量对比。

默认回归命令为`ctest --test-dir BUILD -C CONFIG --no-tests=error --output-on-failure -E '_mutation$'`（Release/Windows为-j3，ASan为-j1）；实际变异使用`-R '_mutation$' -j3`，其后单选console门禁。初次构建及后续依赖检查出现未来mtime警告；三个Linux构建在各自测试完成后复核一次没有重新编译或链接，比对清单内的目标产物SHA-256保持一致，警告原文与产物比对记录在`*-build-root-verified.json`及其原始日志中，不宣称警告已消失。最初dsh的tools-only试跑保留了一个语义用例SKIP，不把该次试跑宣称为完整通过；最终SEMANTIC开启的完整迁移矩阵单独记录。

独立只读dsh正式R1针对冻结树`70d3fa513e36e38899ee2ef9036ac1337542a8dc`完成源码、接口、实测与文档对齐，PASS且零阻塞，无需R2；报告为本轮工作区`build/20261027-migration-symlink/formal-r1-report.md`，SHA-256为`269a722d9bcda35c45fc538d4a34b70a5872cb96c5a2658154b7f06c153e27d9`。协调器随后修正私有报告中对坏候选后续锁检查的过度概括，正式文档原已明确该限制；编码与审查子会话均inactive，headless已退出。总审核九项检查通过，记录见仓库外本阶段`audit-code-nine-checks.json`与`audit-docs-nine-checks.json`。正式审核后的改动仅同步上述已核实结论、补列由总审核独立完成的缺oracle控制并统一“总审核”称谓，源码与测试保持冻结。本轮尚未推送、创建PR或运行新的远端CI。旧阶段结论与此前VERIFY均原样保留。


### 2026-10-27：Issue #88 发布授权

所有者明确批准推送`codex/88-migration-symlink`、创建PR并运行远端CI；总审核据此执行发布。此前“尚未发布/未运行远端CI”保留为本地交付时的快照。此处仅同步授权事实，370项非Markdown输入和原独立dsh R1所审实现保持；没有新增行为或新的dsh审核结论。发布、精确head检查与原始工件入口随[Issue #88](https://github.com/YanBo-maker/YanOS/issues/88)关联PR维护，远端结果与上述本机实测分开记录。


### 2026-10-27：PR #94／#95 合入整合

所有者在核对两项PR状态后明确要求完成合并。附件EOF/EINTR修复[PR #94](https://github.com/YanBo-maker/YanOS/pull/94)已以merge commit `203825a7dcc099c0029da3e82e688656d9c14bbf`合入main，父提交为`552dcde`与已审head `eb9cbd9`，合并tree与该head相同，原实现、文档和发布提交均保留。[PR #95](https://github.com/YanBo-maker/YanOS/pull/95)在此基础上整合镜像链接入口验收，合入授权覆盖这两项；先前本地交付及发布快照保持原文，最新合入记录见对应PR。

整合不新增产品行为。CMake冲突由本轮dsh编码者解决：在相同既有条件内保留两组原测试注册、超时、标签和命令，各出现一次。总审核逐字节确认，相对PR #94仅增加原PR #95的七行注册；生产源码、EOF专项与故障驱动器均等于已审PR #94，新链接专项等于已审PR #95。两边规格追加段落完整保留；STATUS当前阶段同步，两个PR合入前的完整当前阶段分别原字节追加到历史一次。README、开发流程、0040及其索引无需新行为修订。

对同一371项非Markdown输入重新配置、构建并执行Linux Release完整默认套件：注册152项，152/152通过，CTest实耗283.07s，零失败、零跳过。配置开启TOOLS/SEMANTIC，命令为`cmake --build BUILD --config Release --parallel 3`及`ctest --test-dir BUILD -C Release --no-tests=error --output-on-failure -j3 -E '_mutation$'`。当前完整LastTest与注册集合逐名匹配；原迁移74项、EOF22项、链接3例61项分别通过，两个迁移二进制摘要与同次专项输出相符。4095默认性能门禁在本次完整套件中运行，未把旧性能数值当作新测量。

证据在`E:/YanOS/build/20261027-migration-merge/`：`source-union-proof.json`、`frozen-source.json`、`integration-execution.json`、`integration-registration.json`、原始`integration-ctest.log`／`integration-LastTest.log`、`integration-verified.json`及`verify-integration.py`。旧PR #94／#95精确head各自的三项CI和工件已有独立核验；本次新整合head的CI、精确检出与原始工件另随PR #95维护。本机只新跑上述Release套件，不将旧ASan、Windows、实际十七组、真实模型或历史全量比较说成当前整合树的新执行。

本轮dsh协调器与窄范围编码者的整合预检不计正式轮次。独立只读审核者针对冻结树`9ef6bd6699a105434c4be157cad59ef21e7a14d7`完成正式R1，PASS且零阻塞；上限两轮，未用R2或启动恢复。报告为当前检出`build/20261027-migration-merge/formal-r1-report.md`，SHA-256为`43d51932500243e75cea2ccba057304a903b956f55836b62fe747c547def97bd`；核对实际源码、接口、原始证据和文档，编码者与审核者均inactive，协调headless已退出。九项总审核通过，见本轮`audit-nine-checks.json`。审核后仅由总审核机械填入上述已核实结论，371项源码输入保持冻结，不宣称新增dsh轮次或新测试。新整合head仍须单独通过CI；PR #95合入后按0034-R7归档监控通道并保留入口，归档回执在本轮交付材料中记录。


### R7 Issue #84：故障测试的系统调用符号覆盖（2026-10-27）

本轮落实既有[Issue #84](https://github.com/YanBo-maker/YanOS/issues/84)的测试支持边界，不新增产品语义、平台、公开接口或磁盘格式。新增独立`tests/run_yanfs_migrate_symbols.py`；CMake只注册Linux默认测试`yanfs_migrate_symbols`，从故障目标实际`LINK_OPTIONS`传入包装声明，正常CLI和故障二进制分别由目标路径传入，工具取`CMAKE_NM`。包装名不在Python中重复维护；标签为`issue84;0036-R7-symbol-coverage`，超时60秒，不把静态支持检查标为完整AC1/AC2验收，没有SKIP_RETURN_CODE。故障驱动器只修正文首漏列readdir的链接示例及说明，`#define _XOPEN_SOURCE 700`之后的运行源码与父提交逐字节一致。

当前GNU/Linux构建的十一项声明为`open/openat/pwrite/pread/read/write/fsync/close/mkdirat/unlinkat/readdir`。检查通过显式nm读取正常CLI动态未定义符号和故障二进制已定义`__wrap_*`，仅对有效ELF版本后缀去标记，比较声明、定义及导入名称。当前十一项全部对应，额外无关导入允许存在。声明/定义/生产导入不符返回1并打印具名MISMATCH；缺文件、无包装声明、空符号输出或工具失败返回2并打印ERROR，不以77或空结果伪装通过。

这是声明符号的静态支持检查，不证明每个调用点均被拦截，也不覆盖直接系统调用、dlsym、内联或所有I/O。真实注入标记未命中硬失败、真实close一次后报告EIO、残留errno与真实目录失败的控制继续由原行为测试承担；原独立Python镜像/附件oracle不被符号检查替代。当前实际导入为readdir，所以没有增加readdir64包装。自定义编译选项改变调用形状时，检查报告当前故障包装/导入映射不覆盖该形状，不据此判定产品行为无效，不自动按64后缀改名或建立通用包装框架。

修后Linux Release与Debug ASan/UBSan配置各自构建实际CLI和故障目标，符号CTest各1/1通过（0.05s与0.06s）；ASan配置含实际`__asan_init`。这些检查读取ELF符号，不等于执行迁移二进制的ASan完整回归。另从未修改的实际生产源码构建`-D_FILE_OFFSET_BITS=64`正常CLI，编译成功；实际导入变为open64/openat64/pread64/pwrite64/readdir64，当前检查器返回1并给出五项对应的missing-production-call。该结果说明故障包装映射边界，不是产品故障或编译失败检测；不计入实际变异组。

总审核独立重放两个健康配置、真实FOB64候选与十项控制，核对当前检查器SHA-256 `63811cf3316015dfb543e7ed068c481e1d132bda09a16d65c4fbd5d0772c0c3f`、二进制摘要、实际nm输出、编译选项和返回码。缺正常CLI、故障文件、nm，以及nm非零/空输出、空包装声明、无包装定义均为ERROR；缺定义或漏声明为具名MISMATCH，畸形包装声明为ERROR。实际EOF/数据控制的行为保持。总审核随后从同一372项非Markdown输入重新配置、构建并运行完整Linux Release默认套件：153/153，CTest300.05s，零失败、零跳过，注册集合与完整LastTest逐名一致；原迁移74项、EOF22项、链接3例61项与新十一符号检查分别确认，4095性能门禁亦在该套件内通过。不沿用父阶段的ASan全量、Windows、实际十七组、真实模型或历史全量比较冒充新执行。

证据位于`E:/YanOS/build/20261027-migration-symbols/`的`baseline-symbols-root-verified.json`、`source-scope-root-verified.json`、`frozen-source.json`、`targeted-root-verified.json`及`root-targeted-final/`。当前工作区`build/20261027-migration-symbols/`保留实际构建、nm、符号CTest、控制和候选的原始argv/退出状态/stdout/stderr；初始阶段与标签/措辞修前材料留在`phase1-initial-raw/`，小型临时编译探针只是诊断，不替代实际完整生产目标候选。独立只读dsh针对冻结树`946303b6bc7fe753bd7eca5ae6e0e3f4eadabfd2`完成正式R1，PASS且零阻塞；上限两轮，未用R2或启动恢复。报告为当前检出`build/20261027-migration-symbols/formal-r1-report.md`，SHA-256为`0a23f49a3604b093b88ad131e515c99534f6c9d7160c50c1d8bd7bd809383606`；编码者与审核者均inactive，协调headless已退出。总审核九项检查通过，证据见`audit-nine-checks.json`。审核后仅由总审核机械填入这些已核实结论，372项源码输入保持，未新增dsh轮次或测试结果。


当前完整回归命令为`cmake --build BUILD --config Release --parallel 3`和`ctest --test-dir BUILD -C Release --no-tests=error --output-on-failure -j3 -E '_mutation$'`，TOOLS/SEMANTIC开启；每个配置/构建/注册/执行步骤前后核对同一372项输入。原始argv与退出状态在`integration-execution.json`，注册与完整输出在`integration-registration.json`、`integration-ctest.log`和`integration-LastTest.log`，总审核验在`full-release-root-verified.json`。这是当前完整Release结果，不是ASan完整执行或本次远端CI；本次本地提交以交付材料为准；尚未推送或创建PR。


### 2026-10-27：Issue #84 发布授权

所有者明确批准推送`codex/84-migration-symbols`、创建PR并运行远端CI；总审核据此执行发布。此前“尚未发布/未运行远端CI”保留为本地交付时的快照。此处仅同步授权事实，372项非Markdown输入和原独立dsh R1所审实现保持；没有新增行为或新的dsh审核结论。发布、精确head检查与原始工件入口随[Issue #84](https://github.com/YanBo-maker/YanOS/issues/84)关联PR维护，远端结果与上述本机实测分开记录。
