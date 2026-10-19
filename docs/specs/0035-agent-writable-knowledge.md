# 0035：Agent 可写的知识工具

- **意图**：让 Agent 能把笔记写回 YanOS，并用这个能力支撑闫博的学习：笔记里写下可检查的「学会标准」；已懂、不懂、还没碰过的内容分开记；Agent 照标准出题、核对理解、定位原文，再把结果写回库里，下次会话接着用。是否学会由闫博判断。依据：[0001 路线](../decisions/0001-roadmap-A-D-B.md)、[0002 并发路线](../decisions/0002-B-write-concurrency.md)（闫博 2026-10-20 裁定）。
- **状态**：Approved（2026-10-20，闫博）
- **supersedes**：无。本规格扩展 [0033](0033-block-mcp-navigation.md) 的工具集合，不改变 0033 已有的六个工具。
- **成功标准**（从闫博的角度看）：
  1. 闫博不开 Guest 终端时，对 Agent 说「把这条学会标准记下来」或「把刚才的结论写回去」，Agent 先给出预览（将写入的内容和差异），确认后再提交；提交后下次会话里搜得到、读得到原文。
  2. 一篇学习笔记打开后，能分清哪些是学会标准、哪些已懂、哪些不懂、哪些还没碰过，以及 Agent 每次核对的记录和它引用的原文位置（`[[笔记#块]]`）。
  3. Agent 出题和纠错时给出的每条依据，都能用 0033 的块工具在普通笔记库里读到对应原文。
  4. 学习记录和普通笔记分开放在两个镜像里：写学习记录不会改动普通笔记库，也不会因为普通笔记库被占用而写不进去，反过来也一样。
  5. 「学会了没有」始终由闫博写或确认；库里没有任何字段是 Agent 单方面写上「已学会」。
  6. 不开写入开关时，MCP 的行为与 0033 完全一致；开了写入也不会和 Guest 终端同时写坏镜像。

## 三栏边界

| 已决定 | 禁止 | 未规定（留给实现者，或见开放问题） |
|---|---|---|
| 只有两个写工具：新建、整文件覆盖 | 删除、改名、块级补丁、追加写 | 写工具的 description 怎么措辞 |
| 写工具默认关闭，靠启动参数 `--allow-write` 打开 | 未开启时出现在工具列表中 | 锁文件里写什么内容（可以为空） |
| 身份用整个库的身份：0030/0033 的整库 source_sha256，不新增文件级身份 | 不带身份的提交；基于缓存跳过来源核验 | 临时副本的文件名（只要求与镜像在同一目录） |
| 并发：镜像独占锁，拿不到返回 BUSY（0002） | Host 与 Guest 同时打开镜像写 | Host 写适配的内部模块怎么划分 |
| 写入整份镜像原子替换：临时副本 → fsync → rename | 原地改写镜像；成功响应早于 fsync | — |
| 写后不重建索引；索引因来源改变自然变 STALE（0026/0029 现有规则） | 写工具里启动模型、runner 或重建索引 | — |
| 学习笔记用现有格式表达（0027 头 + 0032 具名块 + `[[NAME#ID]]` 引用），不新增文件格式 | 新增「已学会」类字段；Agent 把条目移入 `known` 而没有闫博的确认 | 学习笔记各块里每一行的写法 |
| 内容规则沿用编辑器与 0032 的文本资格（合法 UTF-8，允许 TAB/LF/CRLF） | 写入二进制或不合格文本 | — |
| 单次写入上限 16384 字节（与 Guest 编辑器整稿上限一致） | 超过上限的写入 | — |
| 学习笔记和普通笔记分属两个镜像，各由一个 `yan_mcp` 实例服务（0035-R11）；锁、库身份各自独立 | 在一个实例里同时打开两个镜像；跨镜像解析 `[[NAME#ID]]` | MCP 客户端里两个实例怎么命名 |
| 五个学习块的约定只适用于学习镜像 | 要求普通笔记库遵守学习块约定 | — |
| 先预览再提交：两个写工具都带 `dry_run`；提交必须带预览时拿到的库身份 | 预览改动镜像或拿排他锁；不带身份的提交 | 差异里每行文本的显示方式（原字节原样返回即可） |

## 前置条件

实施 0035-R5 之前，先在闫博的 WSL 上实测：两个进程对 `/mnt/e` 下同一个文件做 `flock(LOCK_EX|LOCK_NB)`，第二个必须拿不到锁；用 WSL 原生文件系统（例如 `~/`）下的路径再测一遍做对照。结果写进 PR 描述。这一条对两个镜像都成立：普通笔记镜像和学习镜像实际要放的目录都要测；另外测一次「两个镜像的旁锁互不干扰」（持有一个的排他锁时，另一个能拿到）。
- 通过：按本规格实施，镜像可以放在 `/mnt/e`。
- 不通过：drvfs 上的锁不可靠，两个镜像都必须放在 WSL 原生文件系统里（只要任一目录不通过，两个镜像都挪走）。写工具和 `yan_run --disk-image` 遇到 drvfs 路径（`statfs` 判定为 9p/drvfs）时，打印原因并拒绝（写工具返回 INVALID，`yan_run` 以非 0 退出）；README 写明这一要求，并增加对应的 AC 后再实施。

## 并发方案

按 0002 的裁定：
- 锁对象是 `realpath(image)` 加后缀 `.lock` 的旁锁文件：不存在就创建，**永不删除、不 rename**。用 `flock(LOCK_EX|LOCK_NB)`。锁加在旁锁文件上，而不是镜像本身，因为镜像会被 rename 替换。
- 顺序固定为：先拿锁 → 再打开镜像 → 挂载/操作 → 关闭（写入时还要 rename）→ 最后释放锁。不允许先打开镜像再拿锁，否则 rename 之后进程可能还在操作旧镜像的 inode。
- `yan_run --disk-image` 从打开镜像到退出一直持有这把锁；拿不到就打印原因，以非 0 退出，不运行 Guest。
- MCP 写工具每次调用时拿锁，调用结束时释放；拿不到返回 BUSY，零写入。
- 0033 的六个只读工具不拿锁，行为和前提都不变：仍然要求「Guest 已退出、调用期间镜像稳定」，靠 STALE 检测变化，不保证快照。MCP 写入因为是 rename，只读工具看到的是替换前或替换后的完整镜像；Guest 在运行时原地写镜像，这种情况仍按 0033 的前提处理，本规格不扩大只读工具的保证。


## 需求与验收

### 0035-R1：写工具默认不可见
当 `yan_mcp` 启动时没有 `--allow-write`，除 `serverInfo.version`（无论是否开启写入都为 `0035`）外，`tools/list` 和所有工具调用的行为与 0033 逐字节相同；调用写工具名返回与未知工具相同的结果。
- 0035-R1-AC1：Given 不带 `--allow-write` 启动，When `tools/list`，Then 输出与 0033 的六工具列表逐字节相同。
- 0035-R1-AC2：Given 同上，When 调用 `yanos_note_create`，Then 返回值与调用一个不存在的工具名相同，镜像 SHA-256 不变。

### 0035-R2：开启后增加两个写工具，注解如实
当带 `--allow-write` 启动时，`tools/list` 在原六个工具之后追加 `yanos_note_create`、`yanos_note_replace`。两者的注解都是 `readOnlyHint:false`、`openWorldHint:false`；replace 另带 `destructiveHint:true`、`idempotentHint:true`；create 带 `destructiveHint:false`。原六个工具不变。（`serverInfo.version` 无论是否开启写入都为 `0035`，见 R1。）
- 0035-R2-AC1：Given 带 `--allow-write` 启动，When 用官方 SDK 列工具，Then 依次为原六个加两个新工具，注解与上述一致。

### 0035-R3：新建笔记
`yanos_note_create(name, text, expected_source, dry_run?)`：提交（`dry_run` 缺省或为 false）时按以下顺序判定，第一条命中即返回：①name 已存在且字节等于 text → 成功并带 `unchanged:true`，不写入（重试幂等）；②当前身份不等于 `expected_source` → STALE；③name 已存在 → EXISTS；④新建文件，内容为 text 的 UTF-8 原字节。成功响应含 `schema_version:1`、`operation:"note_create"`、`name`、`bytes`，以及写前和写后的 `source_sha256`。
- 0035-R3-AC1：Given 镜像里没有 `a.txt`，When 带当前身份新建 `a.txt`，Then 成功；用 Host 侧 `yan_fs_read` 在新镜像上读回的整文件与 text 逐字节相同（text 带具名块时，0033 的 `yanos_block_read` 也能读到对应块）；响应中写后的 `source_sha256` 等于新镜像上 0033 计算出的身份。
- 0035-R3-AC2：Given `a.txt` 已存在且内容与 text 不同，When 带当前身份新建，Then 返回 EXISTS，镜像 SHA-256 不变。
- 0035-R3-AC4：Given 0035-R3-AC1 已成功，When 原样重发同一请求（`expected_source` 已过期），Then 返回成功且 `unchanged:true`，镜像 SHA-256 不变。
- 0035-R3-AC3：Given 目录已满 63 项或没有足够连续空间，When 新建，Then 分别返回 YanFS 对应的错误（沿用 0021 的结果），镜像 SHA-256 不变。

### 0035-R4：整文件覆盖，带乐观并发检查
`yanos_note_replace(name, text, expected_source, dry_run?)`：提交时：
按以下顺序判定，第一条命中即返回：①文件存在且字节等于 text → 成功并带 `unchanged:true`，不写入（重试幂等）；②当前身份不等于 `expected_source` → STALE；③文件不存在 → NOT_FOUND；④整文件替换为 text，响应格式同 0035-R3（`operation:"note_replace"`）。
- 0035-R4-AC1：Given 用 0033 读到的身份 S，When 带 S 覆盖，Then 成功，读回的内容等于 text。
- 0035-R4-AC2：Given 在读到 S 之后另有一次写入改变了身份，When 带 S 覆盖不同的内容，Then 返回 STALE，镜像 SHA-256 不变。
- 0035-R4-AC3：Given 0035-R4-AC1 已成功，When 原样重发同一请求，Then 返回成功且 `unchanged:true`，镜像 SHA-256 不变。
- 0035-R4-AC4：Given 提交时（create 或 replace）缺少 `expected_source` 或格式错误，Then 返回 INVALID，零来源 I/O（沿用 0033 的错误优先级）。

### 0035-R5：与 Guest 终端互斥
按「并发方案」一节执行，前提是「前置条件」已通过。
- 0035-R5-AC1：Given 另一进程持有 `<image>.lock`，When 以提交方式调用任一写工具，Then 返回 BUSY，镜像 SHA-256 不变，且调用返回不等待锁。
- 0035-R5-AC2：Given `yan_run --disk-image X` 正在运行，When 调用写工具，Then 返回 BUSY。
- 0035-R5-AC3：Given 一次写入正持有锁，When 启动 `yan_run --disk-image X`，Then `yan_run` 以非 0 退出，并打印锁冲突原因。
- 0035-R5-AC4：Given 持锁进程被 SIGKILL，When 再次调用写工具，Then 能拿到锁并成功。
- 0035-R5-AC5：Given `yan_run` 正在运行，When 调用 0033 的只读工具，Then 行为与 0033 相同，不返回 BUSY。

### 0035-R6：原子写入与失败行为
每次成功的写入在返回前完成以下步骤：在镜像所在目录写临时副本 → 用 Host 侧 YanFS 在副本上执行操作 → fsync 副本 → rename 覆盖镜像 → fsync 目录。
- rename **之前**任一步失败：返回 IO（或 YanFS 的具体错误），删除临时副本，原镜像字节不变。
- rename 成功、目录 fsync 失败：镜像内容已经替换，只是持久性没有确认。返回 IO，并带 `committed:true`，让调用方知道不能当作「没写」去重试新建。
- 0035-R6-AC1：在 rename 之前的每一步注入失败（写副本、YanFS 写、fsync 副本、rename），Then 都返回错误且不带 `committed`，原镜像 SHA-256 不变，目录里没有残留的临时文件。
- 0035-R6-AC3：注入目录 fsync 失败，Then 返回 IO 且 `committed:true`，镜像内容等于写入后的内容。
- 0035-R6-AC2：Given 镜像已损坏（挂载返回 CORRUPT），When 写入，Then 返回 CORRUPT，不修复、不写入。

### 0035-R7：输入边界
name 沿用 YanFS 名称规则；text 必须满足 0032 的文本资格，长度 ≤ 16384 字节（与编辑器整稿上限一致，见开放问题 3）。参数错误返回 INVALID，零来源 I/O。
- 0035-R7-AC1：含 NUL、裸 CR、非法 UTF-8、超过 16384 字节、非法名称的输入，分别返回 INVALID，镜像 SHA-256 不变，且不打开镜像。

### 0035-R8：学习笔记约定
学习笔记只放在学习镜像里（0035-R11）。它的文件格式和普通笔记相同，0027 头的 tag 含 `learning`，正文用以下五个具名块（ID 固定）；这个约定只适用于学习镜像，普通笔记库不受约束：
- `criteria`：学会标准，每条一行，写成能检查的行为，例如「不看资料，讲清楚一次中断从 CLINT 到 trap handler 的路径」；
- `known`：闫博确认已懂的条目；
- `unknown`：已知不懂的条目；
- `untouched`：还没碰过的条目；
- `log`：每次核对的记录，每条写日期、Agent 出的题、闫博的回答要点、被指出的问题，以及 `[[NAME#ID]]` 形式的原文出处。NAME 指普通笔记库里的文件。

这个约定写进 README 的 MCP 一节和写工具的 description。工具**不校验**学习笔记的结构，在两个镜像上都只按普通文本写入。
- 0035-R8-AC1：仓库提供一份示例学习笔记，作为测试夹具（`tests/` 下），它在 `log` 里引用的文件名都不存在于学习镜像、只存在于普通笔记镜像夹具里；在学习实例上用 0033 的 `yanos_block_list` 能列出这五个块，用 `yanos_block_refs` 能列出 `log` 里的原文引用；这些引用的目标在学习镜像里按 0032 规则显示为缺文件（不跨镜像解析），在普通笔记实例上用 `yanos_block_read` 能逐个读到对应块。
- 0035-R8-AC2：Guest/Host 同镜像验收：Agent 侧用写工具新建示例学习笔记，再覆盖一次（`log` 加一条；测试脚本模拟闫博的明确确认后，把 `unknown` 里的一项移到 `known`）；之后 Guest 终端打开学习镜像，能读到与 Host 写入逐字节相同的内容；整个过程中普通笔记镜像的 SHA-256 不变。

### 0035-R9：学会与否由闫博判断
写工具的 description 和 README 写明：Agent 可以出题、核对、写 `log` 和 `unknown`，把条目移入 `known` 必须在同一会话里得到闫博明确确认。这一条约束的是 Agent 的行为，不能用代码强制，由审查核对 description 与 README 的文字。
- 0035-R9-AC1：description 和 README 中包含上述约束原文，以及 0035-R10 的「展示预览、得到闫博明确确认后才提交」约束；README 原有「学习与理解程度仍由项目所有者判断」的表述保留不变（以 diff 为准）。

### 0035-R10：预览（dry_run）
两个写工具都接受可选参数 `dry_run`（布尔，缺省 false）。`dry_run:true` 时：
- 只读：以 `flock(LOCK_SH|LOCK_NB)` 拿共享锁，保证读到的身份和内容来自同一个稳定镜像；拿不到（Guest 终端或一次提交正持有排他锁）返回 BUSY。不建临时副本，不写镜像；参数校验和错误优先级与提交相同（0035-R7），name 不存在时 replace 返回 NOT_FOUND，name 已存在时 create 返回 EXISTS；
- 预览时 `expected_source` 可以不带；带了且不匹配时返回 STALE；
- 成功响应含 `schema_version:1`、`operation:"note_create_preview"` 或 `"note_replace_preview"`、`name`、`source_sha256`（当前库身份，提交时原样传入 `expected_source`）、`text`（将写入的完整内容）、`diff`。

`diff` 是一段行级差异：去掉新旧内容相同的前缀行和后缀行，剩下的中间部分写成 `{"start_line":N,"removed":[...],"added":[...]}`。切行规则：每行**包含**它结尾的 LF（CR 原样保留）；最后一段没有 LF 的字节也算一行；空内容是零行。这样 `"a"` 和 `"a\n"` 是不同的行。start_line 是中间部分在旧内容里的起始行号（从 1 计）。新建时旧内容是零行，start_line 为 1、removed 为 `[]`；新旧字节完全相同时 start_line 为 0，两个数组都为 `[]`。

协议**不强制**先预览：预览返回的身份只把提交绑定到同一个库状态，不绑定提交的内容，也不证明闫博确认过。真正的确认点是 Agent 的行为约束：把预览的 `text` 和 `diff` 展示给闫博，在同一会话里得到明确确认后，才用同一 name、同一 text、预览返回的身份提交。这一条和 0035-R9 一样，写进工具 description 和 README，由审查核对。

选 `dry_run` 参数、不另加预览工具的理由：预览和提交的参数、校验、错误顺序完全相同，同一条代码路径加一个分支就够；多一个工具就要多一套 schema、注解和测试，而且两套校验容易漂移。不另发确认 token：库身份已经把预览和提交绑定到同一个库状态；token 要多存一份服务端状态，而它同样证明不了闫博确认过，所以不多拦住任何情况。

- 0035-R10-AC1：Given 镜像里有 `a.txt`，When 以 `dry_run:true` 调用 replace，Then 返回预览，镜像和 `<image>.lock` 的 SHA-256、mtime 都不变；另一进程持有排他锁时预览返回 BUSY，持有共享锁时预览照样成功。
- 0035-R10-AC2：Given 旧内容 `"x\ny\nz\n"`、新内容 `"x\nY\nz\n"`，Then `diff` 为 `{"start_line":2,"removed":["y\n"],"added":["Y\n"]}`。另外三例：旧 `"a"` 新 `"a\n"` → `{"start_line":1,"removed":["a"],"added":["a\n"]}`；新建 `"q\n"` → `{"start_line":1,"removed":[],"added":["q\n"]}`；新旧相同 → `{"start_line":0,"removed":[],"added":[]}`。
- 0035-R10-AC3：Given 预览返回身份 S，When 带 S 提交同样的参数，Then 提交成功，写后的内容等于预览里的 `text`。
- 0035-R10-AC4：Given 预览返回 S 之后库被改过，When 带 S 提交不同的内容，Then 返回 STALE，零写入。
- 0035-R10-AC5：Given 不合格的输入（同 0035-R7-AC1），When `dry_run:true`，Then 返回与提交相同的 INVALID，不打开镜像。

### 0035-R11：两个镜像，两个实例
学习镜像和普通笔记镜像各由一个 `yan_mcp` 进程服务，在 MCP 客户端里配置成两个 server。每个进程仍然只接受一个 `--disk-image`，所有工具都作用在这一个镜像上；锁文件（`realpath(image).lock`）和库身份都只属于这个镜像，因此天然独立。`yan_mcp` 的启动参数、工具名和参数都不变。学习实例照 0033 的规则也必须给出 index/model/runner 三个路径（可以指向不存在的文件，0033 的块工具不打开它们）；在学习实例上调用 `yanos_search` 会得到它现有的「索引缺失」结果。

选「两个实例」、不在工具参数里加库枚举的理由：0033 的身份、锁、错误、启动参数都是按「一个进程一个镜像」设计的，开两个进程就直接得到独立的锁和身份，不改任何协议和 schema，0033 的只读工具也自然兼容。加枚举则要给六个旧工具加参数（改 schema、改兼容测试），还要在一个进程里管理两套挂载状态和锁，换来的只是客户端少配一个 server。
- 0035-R11-AC1：Given 两个实例分别服务 `notes.img` 和 `learning.img`，When 另一进程持有 `learning.img.lock` 的排他锁，Then 普通笔记实例的提交照常成功，学习实例的提交返回 BUSY；反过来持有 `notes.img.lock` 时，学习实例照常成功，普通笔记实例返回 BUSY。
- 0035-R11-AC2：Given 两个实例都空闲（没有任何进程持锁），When 在学习实例上提交一次写入，Then 普通笔记实例上 0033 计算的 `source_sha256` 和 `notes.img` 的 SHA-256 都不变；反过来也一样。
- 0035-R11-AC3：Given 两个实例以 `--disk-image` 指向同一个文件（`realpath` 相同），Then 两者共用同一把锁，行为退回单镜像情形，不额外报错（本规格不防这种误配，见假设）。
- 0035-R11-AC4：Given 学习实例启动时 index/model/runner 三个路径都指向不存在的文件，Then 启动成功；块工具和写工具正常工作；`yanos_search` 返回 0033 现有的索引缺失结果。

### 0035-R12：只读工具对两个镜像的兼容
0033 的六个只读工具不改：在哪个实例上调用，就读哪个镜像。普通笔记实例的行为与 0033 逐字节相同（0035-R1）。
- 0035-R12-AC1：Given 不带 `--allow-write` 的普通笔记实例，When 跑 0033 原有的 `mcp_stdio`/`mcp_sdk` 检查，Then 全部通过，期望输出不改（`serverInfo.version` 除外）。
- 0035-R12-AC2：Given 学习实例，When 调用 `yanos_block_list`/`yanos_block_read`，Then 返回学习镜像的内容，`source_sha256` 是学习镜像的身份。

## 非目标

- 不做删除、改名、复制、块级补丁、追加写，也不做多文件事务。
- 不做 Guest RPC，不改传输通道协议，不改 0018/0021 格式。
- 不自动重建词项索引或语义索引；不在写入路径上启动模型或 runner。
- 不新增学习专用的文件格式、数据库、MCP 工具或评分，不让 Agent 判定「学会」。
- 不做单独的预览工具、确认 token 或服务端预览缓存（理由见 0035-R10）。
- 预览的 diff 只做「相同前后缀之外的一段」，不实现通用 diff 算法，也不做字符级差异。
- 不加 request_id 幂等表：靠「内容已相同就返回 unchanged」实现重试幂等。
- 不加审计日志文件、镜像备份或回滚命令；响应里的写前和写后身份就是审计信息。
- 不在一个 `yan_mcp` 进程里服务多个镜像，不加跨镜像的引用解析、搜索或联合身份。
- 不支持 Windows 原生运行写工具（与 0031–0033 相同，只在 Linux/WSL 上运行）。
- 不改 0033 六个只读工具的任何行为。

## 约束

- 复用 `os/yanfs.c`（Host 侧 `yan_fs` 库）和 0033 的来源身份计算；写适配放在新模块里，`yan_mcp`、`mcp_protocol`、`yan_run` 只做必要的接入。
- 测试名和 CTest 标签带 `0035-R#-AC#`（团队手册）。Guest 相关验收进 CI（0034 之后已具备条件）。
- 0002 已由闫博裁定；实施 0035-R5 前先完成「前置条件」的实测。

## 假设

- [假设] 两个实例的 `--disk-image` 指向不同的文件由使用者配置保证；指向同一文件时退回单镜像行为（0035-R11-AC3），不专门检测。
- [假设] 写这个镜像的只有 `yan_run --disk-image` 和 MCP 写工具两个入口；`yan_mkfs` 只新建镜像，不在锁的范围内。不处理硬链接或多路径别名指向同一镜像的情况。
- [假设] 实际使用的镜像在 MiB 量级以下，整份复制的代价可以忽略；不设镜像大小上限。
- [假设] 整库级身份带来的 STALE 频率可以接受：持锁期间只有一个写者，只有闫博在两次 Agent 调用之间改过库，才会出现 STALE。

## 已裁定（闫博 2026-10-20）

1. 并发路线：路线一，锁 + BUSY，写入采用临时副本 → fsync → rename（0002）。
2. 人在回路：采用「先预览，再确认提交」，形状为写工具的 `dry_run` 参数，提交必须带预览时的库身份（0035-R10）；`--allow-write` 保持默认关闭。
3. 身份粒度：用整个库的身份。
4. 单次写入上限：16384 字节。
5. flock 在 `/mnt/e` 上是否互斥：作为前置条件实测，不通过就要求镜像放在 WSL 原生文件系统里。
6. 学习笔记的位置：学习笔记和普通笔记分开，放在两个镜像里；实现为两个 `yan_mcp` 实例（0035-R11）。

## 开放问题

无。

## 实施澄清（develop-gpt，2026-10-20，回应 develop-deepseek 的探查）
- 启动参数沿用现有的 `--disk-image`。spec 原文里的 `--image` 已全部改成 `--disk-image`。
- `serverInfo.version` 不论是否带 `--allow-write`，一律为 `0035`。
- R8-AC2 的 Guest 读回用现有的 guest_* ctest 驱动方式，随 0034 PR-1 已启用的 Guest CI 一起跑。
- 两个镜像实际放在哪个目录，属于部署配置，不在本 spec 范围。测试用临时目录；README 给出示例路径，并注明镜像不能放在同一个文件上。
- 本 spec 链接了 `../decisions/0001-roadmap-A-D-B.md`，这个文件随 0034 PR-2 一起加入。如果实现 PR 比 PR-2 先合入，就把这处链接改成纯文本。
- 拆成两个 PR，不再按 R 的顺序逐条提交：PR-A 是 Host 侧模块，覆盖 R5–R7，不挂到 MCP 上；PR-B 是 MCP 接入，覆盖 R1–R4、R8–R12，以及 R5-AC1、R5-AC2。每个 PR 内部的提交仍然要能追溯到对应的 R/AC。
