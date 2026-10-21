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
