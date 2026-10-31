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

> 后续扩展：[0041](0041-note-meta-mcp-tool.md) 新增两个只读工具 `yanos_note_meta`、`yanos_note_list` 与启动开关 `--allow-meta`。本文件 R1–R12 的验收条件保留 0035 阶段范围；R1-AC1、R2-AC1、R12-AC1 描述的六工具与八工具态继续成立，两开关同开时的十工具态见 0041-R1-AC4。本扩展不改变 `serverInfo.version`（仍为 `"0035"`，见 0035-R1）；服务版本号表示 0035 这一层的接口基线，0041 是在其上叠加的 gated 扩展。

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

- 0035-R10-AC1：Given 镜像里有 `a.txt`，When 以 `dry_run:true` 调用 replace，Then 返回预览，镜像的 SHA-256、mtime 不变；已存在的 `<image>.lock` 在 dry_run 前后 SHA-256、mtime 不变，不存在时允许 dry_run 创建空的 `.lock`；另一进程持有排他锁时预览返回 BUSY，持有共享锁时预览照样成功。
- 0035-R10-AC2：Given 旧内容 `"x\ny\nz\n"`、新内容 `"x\nY\nz\n"`，Then `diff` 为 `{"start_line":2,"removed":["y\n"],"added":["Y\n"]}`。另外三例：旧 `"a"` 新 `"a\n"` → `{"start_line":1,"removed":["a"],"added":["a\n"]}`；新建 `"q\n"` → `{"start_line":1,"removed":[],"added":["q\n"]}`；新旧相同 → `{"start_line":0,"removed":[],"added":[]}`。
- 0035-R10-AC3：Given 预览返回身份 S，When 带 S 提交同样的参数，Then 提交成功，写后的内容等于预览里的 `text`。
- 0035-R10-AC4：Given 预览返回 S 之后库被改过，When 带 S 提交不同的内容，Then 返回 STALE，零写入。
- 0035-R10-AC5：Given 不合格的输入（同 0035-R7-AC1），When `dry_run:true`，Then 返回与提交相同的 INVALID，不打开镜像。

### 0035-R11：两个镜像，两个实例
学习镜像和普通笔记镜像各由一个 `yan_mcp` 进程服务，在 MCP 客户端里配置成两个 server。每个进程仍然只接受一个 `--disk-image`，所有工具都作用在这一个镜像上；锁文件（`realpath(image).lock`）和库身份都只属于这个镜像，因此天然独立。`yan_mcp` 的启动参数、工具名和参数都不变。学习实例照 0033 的规则也必须给出 index/model/runner 三个路径（可以指向不存在的文件，0033 的块工具不打开它们）；在学习实例上调用 `yanos_search` 返回 0033 现有的 DEPENDENCY 错误。

选「两个实例」、不在工具参数里加库枚举的理由：0033 的身份、锁、错误、启动参数都是按「一个进程一个镜像」设计的，开两个进程就直接得到独立的锁和身份，不改任何协议和 schema，0033 的只读工具也自然兼容。加枚举则要给六个旧工具加参数（改 schema、改兼容测试），还要在一个进程里管理两套挂载状态和锁，换来的只是客户端少配一个 server。
- 0035-R11-AC1：Given 两个实例分别服务 `notes.img` 和 `learning.img`，When 另一进程持有 `learning.img.lock` 的排他锁，Then 普通笔记实例的提交照常成功，学习实例的提交返回 BUSY；反过来持有 `notes.img.lock` 时，学习实例照常成功，普通笔记实例返回 BUSY。
- 0035-R11-AC2：Given 两个实例都空闲（没有任何进程持锁），When 在学习实例上提交一次写入，Then 普通笔记实例上 0033 计算的 `source_sha256` 和 `notes.img` 的 SHA-256 都不变；反过来也一样。
- 0035-R11-AC3：Given 两个实例以 `--disk-image` 指向同一个文件（`realpath` 相同），Then 两者共用同一把锁，行为退回单镜像情形，不额外报错（本规格不防这种误配，见假设）。
- 0035-R11-AC4：Given 学习实例启动时 index/model/runner 三个路径都指向不存在的文件，Then 启动成功；块工具和写工具正常工作；`yanos_search` 返回 0033 现有的 DEPENDENCY 错误。

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

## 实施澄清（2026-10-20，答 develop-deepseek PR-B）
- 成功响应：`source_sha256` 为写后身份，`previous_source_sha256` 为写前身份（create 时为 null）。
- replace 的 dry_run 预览：旧文件不是合法 UTF-8 时返回 INVALID_TEXT。
- R11-AC4：沿用 0033 现有行为，index、model、runner 缺失时返回 DEPENDENCY，不新增“索引缺失”这种错误码。
- 只有重试时内容与现有内容相同，响应才带 `unchanged:true`。
- 目录 fsync 失败时，MCP 层按 R6 返回 IO，并带 `committed:true`。
- （第二轮）不抬高 0033 的帧上限 YAN_MCP_INPUT_MAX=16384。R7-AC1 的超长情形在工具层测试；MCP 层收到超长帧或非法 UTF-8 时，沿用 0033 现有的 JSON 解析错误 -32700。
- rename 成功后重新计算身份失败时，返回 IO，并带 `committed:true`。
- 错误响应里的 `committed:true` 放进 content 的 text JSON，形如 `{"code":"IO","committed":true}`。
- 报错顺序：先检查库身份（STALE），再做按笔记的检查（EXISTS 等）；预览和提交都按这个顺序。所以 create 遇到同名且 expected_source 过期时，返回 STALE；身份一致时，再按名字已存在返回 EXISTS。
- 拆分 PR：PR-B 保持 eea97c8 和 6280209 两个提交（R1–R7、R10–R12）；PR-C 做 R8 的夹具和 Guest 测试、R9 的 README、锁文件说明、#53 和 #54。
- （第三轮，答 #56）判定顺序：先判 unchanged（内容和现有完全相同就返回 unchanged:true，因为提交成功后再重试，expected_source 必然已经过期），然后判 STALE，最后做按笔记的检查（EXISTS 等）。
- 镜像旁边还没有 `.lock` 文件时，允许 dry_run 创建它。锁文件是空文件，只用于部署，不算对知识库的写入。
- spec 负责人 develop-gpt 批准 PR-B 的非测试代码超过 400 行（约 450 行），并同意把剩下的工作拆到 PR-C。


## IMPLE PLAN：R8 学习笔记闭环（2026-10-21）

项目所有者批准继续补齐既有 R8-AC1、R8-AC2。示例学习笔记和普通原文放在 `tests/` 下，由独立验收脚本创建一份普通镜像和两份学习镜像（AC1 只读夹具、AC2 写入流程）。先用普通实例读原文；学习实例按 R10 预览、模拟确认后提交创建与覆盖，再重启实例并通过生产 Guest 终端读回。学习镜像中的引用仍按 R8 报缺文件，由调用者选择普通实例定位依据；不扩展跨镜像解析。

验收对比夹具原字节、五块及引用位置、预览不写、提交后身份、重启持久化和普通镜像不变，并核对 Guest 精确输出（含 `yanfs: exit`）与磁盘字节。测试仅模拟人工确认，不验证或推断项目所有者的真实理解。实现集中在测试夹具和独立脚本，CMake 只增加必要注册；没有新增生产接口或格式。独立 dsh 对照当前实现、测试与文档，总审核负责正式文档。

## VERIFY 执行记录：R8 补齐

本机实现与验证已完成，后续发布状态见本节末尾。独立 dsh 两轮技术核对已确认修复，文档对齐意见 M1–M4 由总审核落实；修订与新增验证记录已交回同一只读会话复核通过，不重开代码审核。报告与总审核证据保存在本阶段证据目录；规格整体仍为 Approved。

- 新增 `tests/learning-note/learning-note.md`、`learning-note-updated.md` 与 `notes/{irq,plic,trap}`；具名块、原文和引用按 R8-AC1/AC2 验收。
- 新增 `tests/guest/run_learning_note.py` 与 `tests/test_learning_note_gate.py`。生产 `yan_mcp`、`yan_run`、终端 ELF 和现有协议不变；CMake 仅增加测试注册和 AC 标签。双实例使用同一份普通笔记镜像，核对创建与覆盖预览不写镜像、模拟确认后的提交、身份、重启后的五块和引用，以及真实 Guest 的输出、exit 标记和磁盘原字节。普通库始终保持不变。
- 第一轮 dsh 指出 Guest `exit` 标记未被独立断言，已补齐；同时采纳辅助文件输入清单、读取页范围、从预期字节预计算身份及示例陈述等改进。没有更改 R8 的行为或确认规则。

总审核运行的默认回归（`-j 3`，全部零失败、零跳过）：

| 配置 | 注册 / 通过 | CTest 实耗 |
| --- | --- | --- |
| Linux Release，tools、semantic/MCP、Guest 与 SDK 开启 | 140 / 140 | 290.59 s |
| Linux Debug + ASan/UBSan，同上 | 140 / 140 | 394.37 s |
| Windows MSVC Debug，semantic/MCP 与 Guest 关闭 | 77 / 77 | 193.93 s |
| Linux Release，实际变异配置 | 注册 158，选择实际组 17 / 17 | 570.30 s |

构建目录分别为 `build/20261021-learning-loop/chief-release`、`chief-asan`、`chief-windows`、`chief-mutation`。默认命令为 `ctest --test-dir <目录> -C <Release或Debug> --output-on-failure -j 3`；实际变异命令追加 `-R '_mutation$'`，不是运行全部 158 项。完整配置命令、CTest 汇总和注册清单保存在本阶段仓库外证据目录 `E:/YanOS/build/20261021-learning-loop/`，以 `verified-results.json` 关联原始日志。变异分类门禁保留在默认套件，受控坏接线不计入十七实际组。

347 个非文档输入先冻结；完整回归启动后，修订限于新验收脚本、两份学习记录及 CMake AC 标签，其余 343 项（含全部生产代码）不变。修订完成后复验 `-R '^(learning_note_gate|guest_learning_note)$'`：Release 2/2（4.08 s）、ASan/UBSan 2/2（3.99 s）；Windows 复验 `-R '^learning_note_gate$'` 为 1/1（5.12 s），均零失败零跳过。最终输入逐项哈希复核；不把早期夹具或旧标签的日志称作最终修订结果。

总审核另以真实进程做正负对照：正确工具链路返回 0；关闭 MCP 写入能力、让 Guest 执行器消费输入但返回空输出，两种坏接线均触发 `:FAIL:` 并返回 1，未输出通过标记。六项纯 Python 检查另守住错误预期、篡改预览、缺本地输入与不扫描无关目录。缺仓库输入和显式 ELF 必须失败；新联合验收不使用 77。

限制：只验证所提供的固定学习记录与普通原文，测试中的确认和理解状态均为模拟；不证明真实用户理解、生成答案的忠实性或任意 Agent 的确认行为。块工具与写工具不启动模型，本阶段不做搜索质量验收。现有索引、磁盘格式、跨库解析和生产接口未扩展。Linux 默认 CI 会在满足现有构建条件时执行新验收；远端是否通过须核对 PR 当前精确 head 的实际日志，不能从本机结果推导。


### R8 发布记录（2026-10-21）

项目所有者已明确批准本阶段推送、创建 PR 和运行 CI。测试提交 `f93bcfd`、相邻文档提交 `0aee7d2` 已发布为 [PR #85](https://github.com/YanBo-maker/YanOS/pull/85)，base 为 main。功能与本机验证范围未变；发布事实文档更新后由远端再次验证最新 head。当前精确 head 的三项检查和原始任务日志在该 PR 记录，不以旧 head 结果冒充最新结果，不在执行结束前宣称全部通过。尚未合入，0035 整体保持 Approved。


## VERIFY 执行记录：dsh 真实双库试用（2026-10-22）

项目所有者选择先接 dsh，沿用既有 R8–R11、0031/0033 工具和格式。本轮没有修改生产代码、工具 schema 或模型服务；配置和试用镜像保存在本机任务目录中，不纳入版本控制，公开接入步骤见 [dsh 双库接入](../dsh-mcp-quickstart.md)。独立 dsh 对准备材料完成一轮只读审核，无当前试用阻塞项；总审核已修正试用脚本的空库容量、耗时和进程归属判断。正式文档与实际执行记录已由原独立只读 dsh 会话核对，总审核完成本地收口。

- 环境：dsh `0.2.0-rc.2`，普通与学习实例分别命名 `yanos_notes`、`yanos_learning`。专用 profile 来自 headless 模板，通过任务 overlay 接入；原日常 profile、所有者未提交修改及生产输入保持。普通库使用仓库公开夹具；学习库初始为空且有可用数据空间。没有接入日常个人笔记库或新增生成模型。
- 准备探针用官方 Python `mcp==2.3.0`，两实例实际协商 `2026-07-28`，分别列出六个只读工具和八个含写入工具；索引 READY、原文块和预览差异与独立字节预期相符。准备过程没有真实 dsh 消费者调用，不把它算作 harness 互操作。
- 真实 dsh 首次调用成功完成 `index_status`、搜索“设备如何叫醒处理器”、列块与读取 `irq#entry`，随后在预览前退出 1，输出停在下一步开始，没有完成原因。总审核未确定退出原因；保留原始记录，仅复用同一会话重试一次，重试退出 0，重新读块后成功获得预览。没有以新会话替换失败记录。
- 总审核逐项核对真实工具记录：只调用绑定的 MCP 工具，逐次等待；搜索返回的二十项原文与实际夹具字节范围、物理行号相符；`irq#entry` 为文件第 5 行、字节区间 `[74,157)`。本次不以固定样本结果宣称通用检索质量。
- 预览为 `learning-note.md`，完整正文 470 UTF-8 字节、十三行，差异为第 1 行起全部新增、无删除；`known` 为空，引用只指向实际读取的 `[[irq#entry]]`。预览前后普通镜像、索引、学习镜像和已有旁锁的 SHA-256 与 mtime 均不变，且没有提交调用。
- 总审核在当前会话提供完整正文与差异，闫博明确回答“确认保存这份试用记录（推荐）”。总审核将这条真实确认转交原 dsh 会话，用预览的同一 name、text 和 source 提交一次；不是夹具模拟，不是对“已学会”的确认。提交退出 0，工具报告 `note_create`、470 字节、新来源 `89b24c1653d53690e6339f36770cf50ba6b225535b365dcf8713bec7c38f2532`；随后读回五块，`known` 仍为空。
- 关闭该进程后，总审核另启只读官方 SDK 会话，核对五块全文、来源和学习库断链状态，再用生产 `yan_run`、`yanfs_terminal.elf` 对同一学习镜像执行列块、cat 和 show log，并断言 `yanfs: exit` 与退出 0。新 SDK 读回耗时 0.656 s，Guest 读回 0.111 s。磁盘中的完整笔记逐字节等于已确认预览；Guest 查询前后镜像相同，普通镜像和原索引始终不变。

本轮没有重新声称默认回归或实际变异通过；它们仍属于上节 R8 的既有提交与对应 CI。PR #85 精确 head `30916a6` 的三项原始任务日志已经总审核核对通过（运行 `39596620235`）；本轮接入文档提交 `a532995` 经所有者明确批准，已推送并创建 [PR #86](https://github.com/YanBo-maker/YanOS/pull/86)，暂以 `codex/0035-learning-loop` 为 base，仅展示本轮文档。该 PR 已触发远端 CI；发布事实更新后核对最新精确 head 的三项检查与原始任务日志，并在 PR 摘要记录，不把父阶段或旧 head 结果算作最新结果。尚未合入，0035 整体仍为 Approved。

证据入口：`E:/YanOS/build/20261022-dsh-usage/` 中的 `chief-preflight.json`、`consumer-events.jsonl`、`consumer-retry-events.jsonl`、`chief-live-audit.json`、`live-preview.json`、`owner-confirmation.json`、`confirmed-commit-events.jsonl` 与 `persisted-check.json`。准备源码、独立审核及探针在隔离工作树 `build/20261022-dsh-usage/`；Guest 的实际 argv/stdout/stderr/rc 在其 `guest-readback/`。初次失败记录保留，后续重试与确认提交分别记录；没有复活归档实验或新增 Codex 子 Agent。

限制：仅为独立样本库的一次真实确认新建，没有真实问答测验、理解验收、日常个人库迁移、客户端长期稳定性或真实确认覆盖写验收。引用仍按实例各自解析；这次确认不授权 Agent 自动标记已懂或跳过后续写入预览。


## VERIFY 执行记录：写工具文档一致性检查（2026-10-22，Issue #61）

本轮验证既有 R2/R9/R10 的文档与真实接口是否一致，没有新增需求或改变工具行为。新增独立纯 Python 检查器 `tests/mcp_write_docs.py`，从[写工具说明](../mcp-write-tools.md)读取两个工具参考段，与真实 `tools/list` 的名称、description、annotations、inputSchema、outputSchema 比较；create 的共享输入结构须有明确引用。对象键序和缩进不影响比较，嵌套字段、数组次序及类型纳入检查，布尔值不与数字相等。缺文档、坏 JSON 或字段不符均失败，不返回 77。

`mcp_note` 在原写工具测试中接入真实进程核对；新检查使用不存在的镜像与依赖路径，只请求工具列表。检查器在 main 执行路径才导入，借用 Server 的学习记录验收不新增文档或 helper 依赖。`mcp_write_docs` 是配置找到 Python 时注册的纯检查器自测，不等同 R2-AC1 的官方 SDK 工具发现验收；后者继续由 `mcp_sdk` 负责。README 与其他用法段落不属于本检查器的覆盖范围，仍由独立文档审核对齐。

总审核先独立采集真实服务的八工具列表，按当前文档逐字段核对两个写工具，基线一致；没有以此冒充新增自动检查已经通过。dsh 编码子任务与独立只读子任务各一，技术与文档统一审核最多两轮；重试复用原会话，不复活旧试用或归档实验。

执行结果均为本轮重新运行，默认命令为 `ctest --test-dir <构建目录> -C <配置> --output-on-failure -j 3`；Linux 省略 `-C`，实际变异命令增加 `-R '_mutation$'`。F1 修补前的三平台默认回归与实际组选取均零失败、零跳过：

| 配置 | 注册 / 本次选择并通过 | CTest 实耗 |
| --- | --- | --- |
| Linux Release，tools、semantic/MCP、Guest 与 SDK 开启 | 141 / 141 | 182.42 s |
| Linux Debug + ASan/UBSan，同上 | 141 / 141 | 259.48 s |
| Windows MSVC Debug，semantic/MCP 与 Guest 关闭 | 78 / 78 | 195.92 s |
| Linux Release，实际变异配置 | 159 / 17 | 340.12 s |

Linux 默认集合中的 `mcp_note`、`mcp_sdk`、`mcp_write_docs`、学习联合验收均实际 Passed；Windows 的 `mcp_write_docs` 实际 Passed。最终纯检查器含十八项自测，实际接口比较在原 `mcp_note` 中，不把纯检查器标签当作官方 SDK 的完整 R2-AC1 验收。

总审核另按名复制所需文件，在临时副本运行真实写工具测试：正常接线退出 0，篡改 description、删除文档、删除 helper 分别退出 1；把比较器改为直接通过后，纯检查器负例退出 1。原仓库文件保持。这些是受控接线与检查器负例，不计入十七个既有实际变异组，也不证明所有读者文档已自动检查。

构建目录复用 `build/20261021-learning-loop/chief-release`、`chief-asan`、`chief-windows`、`chief-mutation`，本轮重新配置、构建并生成日志。344 个代码与测试输入在完整回归期间保存哈希；当时修改文件的 mtime 均早于测试启动，完整回归完成后逐项保持。随后 F1 仅修改两个测试模块，其余 342 项逐项保持；最终两个模块另按下述定向检查验收。生产源码没有变化。原所有者修改、既有试用镜像和索引不属于本轮写入对象。

证据入口：`E:/YanOS/build/20261022-mcp-doc-alignment/` 下的 `verified-results.json`、四份 `*-ctest.log`、`*-registration.log`、`frozen-inputs.json`、`chief-controls.json` 与对应原始输出。Windows 首次启动因当前 shell 的 PATH 无 cmake 而在配置前失败，未执行测试；随后使用原缓存指向的 Visual Studio CMake/CTest 绝对路径完成本表回归，未更改全局配置。

独立审核第一轮确认当前文档与实际工具一致，并指出 create 小节若同时保留共享引用与另一份 inputSchema 声明，检查器会忽略后者。总审核将这一项提升为 F1：被检查字段的矛盾声明不能静默通过。原 dsh 编码会话只修补 `tests/mcp_write_docs.py` 和 `tests/test_mcp_write_docs.py`，遇到共享引用外的显式 inputSchema 声明即报歧义；不拒绝无关 JSON 示例，字段和实际 MCP 接口保持。

总审核从修后代码移除该补丁，在私有副本精确还原旧 helper，并核对 SHA-256 等于完整回归时的冻结值。同一矛盾文档与实际工具列表在旧 helper 下误通过，在新 helper 下明确拒绝；正常文档与无关 JSON 示例均仍通过。证据 `post-f1-results.json`。修后只重跑受影响的检查，没有声称完整三平台回归和实际变异在新 helper 上重跑：

| 修后配置 / 范围 | 实际通过 | CTest 实耗 |
| --- | --- | --- |
| Linux Release，mcp_note 与 mcp_write_docs | 2/2 | 0.70 s |
| Linux ASan/UBSan，同上 | 2/2 | 2.64 s |
| Windows Debug，mcp_write_docs | 1/1 | 0.29 s |

上述检查均无失败或跳过，最终纯自测为十八项。原报告保持；文档派生的纯检查器正控只验证检查器，实际服务由 mcp_note 核对；额外展示字段如 title 不在所定义五字段范围内。这两项为已声明边界，不列作待修产品缺陷。第二轮仅复核 F1 及对应记录，无新完整审查轮次。

审核状态：原独立只读 dsh 完成两轮核对，第二轮仅复核 F1 修补及对应记录，无剩余阻塞项；总审核九项提交前检查完成。

本轮实现提交 `31a88e0`、文档提交 `12f9096` 经所有者明确批准，已推送并创建 [PR #87](https://github.com/YanBo-maker/YanOS/pull/87)，暂以 `codex/0035-dsh-usage` 为 base，仅展示本轮改动。远端 CI 已触发；发布事实更新后按最新精确 head 的实际检查及原始任务日志验收，结果记入 PR 摘要，不使用父 PR #86 或旧 head 的通过结果替代。尚未合入。0035 状态保持 Approved；README、规格索引、决策与既有工具说明约束未扩展。

### PR #85/#86 合入与 PR #87 状态来源（2026-10-22）

项目所有者明确批准按 #85 → #86 → #87 依次合入 main，并保留阶段提交。#85 已按 merge commit 合入（`3377a50`），随后 #86 改以 main 为 base 并合入（`fae32e8`）；两次合入后的文件树分别与已验证的 `30916a6`、`135d272` 完全一致。

#87 已改以 main 为 base。已验证提交 `349896e` 的三项 CI 及原始日志已核对：Linux Debug ASan/UBSan 141/141（835.22 s）、Linux Release 141/141（356.88 s）、Windows Debug 78/78（318.42 s），均无失败或跳过；这是 F1 修补后的完整远端回归，先前本地 F1 前后记录保持原范围。后续提交的检查与合入状态以 [PR #87](https://github.com/YanBo-maker/YanOS/pull/87) 的实时记录为准；以上静态结果仅证明 `349896e`，不证明尚未运行的提交。

上文各次“尚未合入”保留其发布时状态；#85/#86 已完成的合入由本节接续，#87 的当前状态通过该 PR 查阅。此次仅修正父 PR、目标分支及状态来源，不改实现或接口，0035 整体仍为 Approved；阶段合入后按 0034 R7 归档监控通道并保留入口。


### PR #85／#86／#87 合入回执（2026-10-23 核对）

GitHub 当前记录确认：PR #85 于 2026-10-22 以 `3377a50b743a57876ad249cf9f0142bed4cf2a20`、#86 以 `fae32e811060be0c9e066f60be24e93546bca2cf`、#87 以 `0948b94dfe54cc44006cb453c9e821400b0f01a2` 依次合入 main，保留阶段提交。#87 的核对记录接续上文发布快照，不改写过去的“尚未合入”。该历史随后保留在 `26c25fc` 的 main 中；新 main 的验证记录见 [0036 VERIFY](0036-yanfs-multi-block-directory.md#verify-执行记录)。本回执不扩展真实 dsh 试用范围，也不把 0035 整体提升为 Implemented。


### 用法入口与平台覆盖说明核对（2026-10-30，Issue #57／#60）

[Issue #60](https://github.com/YanBo-maker/YanOS/issues/60)的补充放在README的 `yan_run --disk-image` 示例旁：Linux/WSL在打开镜像前取得旁锁并持有到退出，锁冲突诊断及退出码5直接可见，同时链接完整锁说明；Windows原生 `yan_run` 未实现该旁锁。实际依据为 `tools/yan_run.c` 的 `__linux__` 条件与加锁顺序，以及 `host/note_write.c` 的非阻塞flock。既有写工具章节保留，程序行为不改。

[Issue #57](https://github.com/YanBo-maker/YanOS/issues/57)允许评估Windows移植或明确Linux覆盖。本轮采用已批准的Linux/WSL支持范围，README与STATUS显式说明：Windows不构建MCP服务、不运行四项MCP服务／SDK验收，只运行纯Python的 `mcp_write_docs` 检查器自测。`CMakeLists.txt` 的非Linux SEMANTIC配置拒绝及workflow的Linux检查分支保持，不将此文档补充表述为Windows原生MCP已实现或已覆盖。

本轮仅做文档、链接、历史与实际源码的一致性核对，未新跑产品回归或实际变异，也未改变本规格状态、接口或验收标准。主目录同步与旧现场备份属于本机交接，记录在仓库外材料；文档审核和后续发布事实随本轮PR与交付材料维护。
