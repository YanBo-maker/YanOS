# 0041：笔记元数据的 MCP 只读工具

- **意图**：Agent 不开 Guest 终端也能读到一篇笔记的 0027 元数据（title、created、modified、tag、link 及其悬空状态），补齐 0027 观察能力缺失的 MCP 用户面，使 Agent 能在不破坏头的前提下读懂并整文件覆盖笔记。依据：[0005 决策](../decisions/0005-note-meta-mcp-tool.md)（闫博 2026-10-30 裁定）。
- **状态**：Approved（2026-10-30，闫博）
- **supersedes**：无整份替代。本规格扩展 [0033](0033-block-mcp-navigation.md) 与 [0035](0035-agent-writable-knowledge.md) 的工具集合：新增一个只读工具 `yanos_note_meta`，由独立启动开关 `--allow-meta` 门控。0033 的六个工具、0035 的 R1–R12 与两个写工具的行为和验收条件均不改变；0035-R1-AC1 / R2-AC1 / R12-AC1 描述的仍是 0035 阶段的工具面。
- **成功标准**（从闫博的角度看）：
  1. Agent 不看终端就能说出一篇笔记声明了哪些标题、日期、标签和链接，以及哪些链接已经指不到文件。
  2. 不开任何开关时，MCP 对外表现与 0033 完全一样；要不要多这个工具由启动者显式决定。
  3. Agent 能靠这个工具把一篇带头笔记读全，再整文件覆盖回去时头不丢。
  4. 读一篇没有头的旧笔记、或正文是二进制的笔记，不被当成错误；坏头则明确报 METADATA。

## 三栏边界

| 已决定 | 禁止 | 未规定（留给实现者） |
|---|---|---|
| 只加一个只读工具，只由 `--allow-meta` 开启，与 `--allow-write` 正交 | meta 的写入面（title/tag/link/date 的增删改）；不带 `--allow-meta` 时出现在工具列表 | description 的措辞 |
| link 状态两态 `OK`/`MISSING`，只看目标文件是否存在 | 读目标正文判定链接健康；引入第三态；复用 0032 的 `OK/DANGLING_FILE/DANGLING_BLOCK` | — |
| 单文件观察只校验目标头 | 导入 0027:83 的 list 全库预检（任意坏头即 METADATA、不出半份名单） | — |
| 正文内容不参与判定，对二进制正文照常返回元数据 | 在观察路径返回 INVALID_TEXT / TOO_LARGE（二者属 0027 的 update 路径） | — |
| 本次不含 tag 筛选 | 把 `meta list` 的 AND 筛选或 `search` 的 `tag:` 操作符塞进本工具 | 另开 Issue 的编号与拆法 |

## 需求与验收

### 0041-R1：新增只读工具 yanos_note_meta，由 --allow-meta 单独开启

`yanos_note_meta` 只读，补齐 0027 元数据观察的 MCP 用户面（Guest 侧对应用户面是 `meta show`，0027:45–83）。只由启动开关 `--allow-meta` 启用，与 `--allow-write` 正交，默认关闭。`--allow-meta` 不带参数值；重复出现按现有选项解析规则返回启动 USAGE、退出 2（沿用 0031:23 的未知或重复选项规则）。

arguments：`{ "name": string, "expected_source"?: string }`。name 沿 YanFS 1–31 字节名称规则；expected_source 为 64 个小写十六进制字符（沿 0030 身份）。未知字段、非法 name、含 NUL 或格式错误的 expected_source 均 INVALID，零来源 I/O。

成功对象沿用 0033 的公共成员（`schema_version`=1、`operation`="note_meta"、`source_sha256`），另加：

| 成员 | 类型 | 说明 |
|---|---|---|
| name | string | 查询的文件名 |
| has_header | boolean | byte 0 是否有完整合法的 0027 头 |
| title | string \| null | 未声明为 null；沿 0027 的 1–256 UTF-8 字节规则原样返回 |
| created | string \| null | 严格十字节 `YYYY-MM-DD`；未声明为 null |
| modified | string \| null | 同上 |
| tags | string[]（≤16） | 文件顺序、原字节、不排序不折叠 |
| links | `[{name, state}]`（≤16） | 文件顺序；`name` 为目标文件名，`state` 见下 |
| complete | boolean（固定 `true`） | 本次观察是完整的前缀观察，不含分页或截断；为后续可能的分页保留常量位 |

`has_header=false` 时 title/created/modified 为 null、tags/links 为空数组，这是合法的旧笔记观察，不是错误（0027:15「没有完整 9 字节 `@yan-note` 前缀的文件是旧格式，元数据为空」、0027:87）。`title`/`created`/`modified` 是「声明即返回」，不做日历有效性二次判断——0027 的格式校验已在解析时完成，能走到这里就是合法值。

**link 状态两态 `OK` / `MISSING`**：`OK` 表示目标文件存在，`MISSING` 表示目标文件不存在。判定只做一次目录 stat，**不读目标正文**（0027:81「LINK 状态只看目标文件存在，不读目标正文；DANGLING 不是 FS 错误」）。`MISSING` 与 Guest 终端 `meta show` 输出的 `DANGLING` 语义完全相同（0027:76–77 的输出示例为 `LINK irq.txt OK` 与 `LINK missing.txt DANGLING`），本规格改用 `MISSING` 只因为 MCP 侧以「目标是否存在」为唯一判据。**本工具的 `MISSING` 与 `yanos_index_status` 的 `state` 枚举值 `MISSING`（索引缺失）不是同一个概念**，两者出现在不同的 operation 下，不得互相解释。目标存在但 stat 返回其它错误时，整个调用失败并报该错误，不降级成 `MISSING`。

单文件观察只校验目标头，不导入 0027:83 的 list 全库预检规则（0027:83 规定 `meta list` 必须先检查全部文件的头、发现任意坏头即 ERROR METADATA、不出半份名单；那是全库列举的规则）。头不合法（marker 缺失、版本未知、头未闭合、字段非法等）返回 METADATA；这是普通内容错误，不使 FS 进入 FAULTED，同实例后续调用仍可用。**正文内容不参与判定**：本工具只读识别并校验头所需的 prefix，不全文预检正文，因此正文为二进制的笔记照常返回元数据，观察路径永不产生 INVALID_TEXT 或 TOO_LARGE（二者属 0027 的 update 路径，0027:87、0027:154）。

错误顺序沿 0033:44 的既有优先级，第一条命中即返回：

1. 请求格式/参数错误（未知字段、非法 name、含 NUL 或格式错误的 expected_source）→ INVALID，零来源 I/O
2. 打开/挂载镜像失败 → 实际错误
3. 计算前身份（`before`）失败 → 实际错误
4. 合法 expected_source 与**前身份（`before`）**不符 → STALE，**不进入文件查询**
5. 文件不存在 → NOT_FOUND
6. 头不合法 → METADATA
7. 成功后重新观察后身份（`after`）：读失败报实际错误；`after` 与 `before` 不同 → STALE
8. 成功；`source_sha256` 为 `after`（此时等于 `before`）

第 4 步先于第 5 步，是因为 0033:44 已定「合法 expected_source 不匹配即 STALE，不进入块查询」；本工具沿用同一优先级，使调用方能用一套逻辑处理两族只读工具。第 4 步比较的是 `before` 而不是 `after`：先核对调用方持有的身份是否仍有效，再付出查询代价；`after` 只用于第 7 步的变化检测。缺 expected_source 时跳过第 4 步，仍做前后两次观察。

注解：`readOnlyHint:true`、`destructiveHint:false`、`idempotentHint:true`、`openWorldHint:false`（与 0033 的四个块工具一致，0031:55 要求如实声明）。工具描述提供完整 inputSchema/outputSchema，不含网络 $ref；`inputSchema` 与 `outputSchema` 均 `additionalProperties:false`。

本工具与 0033 的六个只读工具采用同一前提：**不拿镜像旁锁**，只以只读方式打开镜像，靠前后身份比较做变化检测，不宣称并发快照或事务隔离（0035:44 对六个只读工具的既有规定，本规格沿用）。因此它仍要求 Guest 已退出、调用期间镜像稳定；镜像被就地改写时由第 7 步报 STALE。

**顺序与计数**：

| 启动开关 | 工具列表 |
|---|---|
| 无 | 0033 六工具，JSON 数组逐字节不变（`TOOLS_0033_SHA256` 不变） |
| 仅 `--allow-meta` | 六工具 + `yanos_note_meta` = **7** |
| 仅 `--allow-write` | 六工具 + `yanos_note_create` + `yanos_note_replace` = **8**（与 0035-R2-AC1 一致） |
| 两者 | 六工具 → `yanos_note_meta` → `yanos_note_create` → `yanos_note_replace` = **9** |

`serverInfo.version` **保持 `"0035"`**，不推进为 `0041`。理由：0035 有四处把该值规定为 `"0035"`（`grep -c serverInfo.version` = 4）——「0035-R1」正文、「0035-R2」正文、「实施澄清（develop-gpt…）」节、以及 0035-R12-AC1；而本规格不改写 0035 任何正文，故必须遵守其取值规定。这与 0033 当年推进为 `0033` 不矛盾：0031 全文没有该值的取值规定（`grep -c serverInfo.version` = 0），0033 是首次定值而非违反既存规定。服务版本号表示 0035 这一层的接口基线，0041 是在其上叠加的 gated 扩展。「实施澄清（develop-gpt…）」节那条的措辞是「不论是否带 `--allow-write`」，未覆盖 `--allow-meta`；本规格保持 `"0035"`，因此不需要对该条做任何澄清。

- 0041-R1-AC1：Given 不带任何开关启动，When `tools/list`，Then 工具数 6，名称依次为 search、index_status、block_list、block_read、block_refs、block_backlinks，`tools` 成员的 SHA-256 等于 0033 六工具基线 `TOOLS_0033_SHA256`，`yanos_note_meta` 不出现。
- 0041-R1-AC2：Given 仅带 `--allow-write` 启动，When 用官方 SDK 列工具，Then 依次为 0033 六工具加 `yanos_note_create`、`yanos_note_replace`（即 0035-R2-AC1 在原位仍成立）。
- 0041-R1-AC3：Given 仅带 `--allow-meta` 启动，When `tools/list`，Then 工具数 7，第 7 个为 `yanos_note_meta`，其 annotations 恰为上述四项，inputSchema/outputSchema 非空且 `additionalProperties` 为 false。
- 0041-R1-AC4：Given 两开关同开，When `tools/list`，Then 顺序为六只读 → `yanos_note_meta` → `yanos_note_create` → `yanos_note_replace`，共 9 个。
- 0041-R1-AC5：Given 不带 `--allow-meta` 的实例，When 调用 `yanos_note_meta`，Then 返回值与调用一个不存在的工具名相同（JSON-RPC `-32602`），镜像 SHA-256 不变。
- 0041-R1-AC6：Given 一篇头为 title=中断笔记、created=2026-10-07、tag=os、tag=riscv、link=irq.txt（存在）、link=missing.txt（不存在）的笔记，When 调用，Then has_header=true、title/tags 逐字节相符、links 依次为 `{irq.txt,OK}` 与 `{missing.txt,MISSING}`，镜像 SHA-256 与 mtime 不变。
- 0041-R1-AC7：Given 一个无头、正文为二进制的旧笔记，When 调用，Then has_header=false、title/created/modified 为 null、tags 与 links 为空数组、返回成功，**不返回 INVALID_TEXT 或 TOO_LARGE**。
- 0041-R1-AC8：Given 一个头损坏的文件（如 marker 缺 ` 1\n`），When 调用，Then METADATA；随后在同实例调用其它工具仍正常工作，FS 未进入 FAULTED。
- 0041-R1-AC9：Given 文件不存在且 expected_source 与**前身份（`before`）**不符，When 调用，Then 返回 STALE（不是 NOT_FOUND，因为第 4 步先于第 5 步）；Given 文件存在且 expected_source 与前身份不符，Then 亦返回 STALE。两者都不产生成功对象。
- 0041-R1-AC10：Given 任一次成功或失败调用，Then 镜像零写入，SHA-256 不变。
- 0041-R1-AC11：Given 一个正被 `yan_run` 持有排他锁的镜像，When 调用 `yanos_note_meta`，Then 与 0033 的六个只读工具一致：不拿镜像旁锁，照常只读打开并返回观察结果（若调用期间镜像被就地改写，则由 R1 第 7 步的前后身份比较报 STALE）。

## 非目标

- 不改 0033 六个工具与 0035 两个写工具的任何行为、schema、注解或错误 token。
- 不提供 meta 的写入面：不新增 `yanos_note_meta_set` 之类工具；改 meta 仍走 Guest 终端或 0035 的整文件覆盖。
- 不提供 tag 筛选与文件名单：`meta list` 的 AND 筛选（0027:83）和 `search` 的 `tag:` 操作符（0027:97）都不在本规格，另开 Issue 并拆成两条。
- 不新增文件格式、索引、磁盘字段或身份类型；不读目标正文；不在读取路径启动模型或 runner。
- 不支持 Windows 原生运行（与 0031–0035 相同，只在 Linux/WSL 上运行）。

## IMPLE PLAN / VERIFY

| 顺序 | 实现范围 | 必须提供的证据 |
|---|---|---|
| A | `tools/mcp_meta.c/.h`：复用 0027 服务做单文件观察与 JSON 编码；错误顺序按 R1 的 1–8 步 | 头部四类边界（无头/坏头/二进制正文/正常头）、links 两态、镜像零写入 |
| B | `tools/yan_mcp.c` 的 `--allow-meta` 解析与 `tools/mcp_protocol.c` 的 gated 分支、工具顺序 | 四态门控 6/7/8/9、默认态 `TOOLS_0033_SHA256` 不变、重复开关 USAGE 退出 2 |
| C | 错误顺序与 STALE 优先级、BUSY 与锁 | 过期身份 + 缺文件返回 STALE；Guest 持锁返回 BUSY |
| D | 真实镜像与 Guest 对照 | 同一镜像上 `yanos_note_meta` 与 Guest `meta show` 逐字段一致 |

实现以 0027 服务为唯一分类来源，不改 `os/note_metadata*`；MCP 层只做 gating 与 JSON 编码。审查读取当前实现与本次测试证据，不以实现者自查替代。

## VERIFY 执行记录

### 首轮本机验收（2026-10-30，实现方报告，待总审核复核）

以下由实现方在本机报告，尚未经总审核逐条复核，也尚未进入 CI，不作为已通过的结论：

- 门控四态实测：不带开关 6 个工具、仅 `--allow-meta` 7 个、仅 `--allow-write` 8 个、两者同开 9 个；默认态 `tools` 成员 SHA-256 = `3bbd05c5c7e5adfef17122dbf46a10b2704ee960c8cddf6a163a176c72bab4a2`，与 0033 基线 `TOOLS_0033_SHA256` 相同。
- 真实镜像 `trap-entry.txt`：`title`/`tags`/`links` 与 Guest 终端 `meta show` 逐字段一致。
- 调用前后镜像 SHA-256 相同，确认零写入。

待补：完整默认套件、7/9 工具态与 `--allow-meta` USAGE 用例、远端 CI 记录。本节在补齐并由总审核复核前不得当作通过，也不把待跑项写成已通过。
