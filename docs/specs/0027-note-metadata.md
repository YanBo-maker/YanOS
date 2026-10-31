# 0027：笔记元数据与标签筛选

> 状态：**Spec Lock**（2026-10-07 17:56 +08:00）。项目所有者已批准整体方案并授权继续实现。整体已实现并完成本机验证，独立实现审查已完成；功能提交180d18e的PR #24三项CI通过，项目所有者于2026-10-11明确批准，经PR #24合入main（7451c85），保留阶段提交；学习理解由项目所有者判断。

## INTENTION

为笔记声明标题、标签、日期和文件链接，并用标签缩小词项搜索范围。元数据与正文保存在同一个普通文件，原文件仍是事实来源。只发布一个文件，不改变YanFS v1目录格式、块协议、ISA或运行时。

本阶段完成meta观察/更新/列表与search标签AND筛选。related/backlinks、自动链接修复、现实时间、语义模型和Host索引不在本阶段。

## SPEC

> 后续扩展：[0041](0041-note-meta-mcp-tool.md) 把本规格的元数据观察接到 MCP 只读面上：`yanos_note_meta` 返回单文件的 title/created/modified/tag/link，`yanos_note_list` 提供本文「命令及可观察输出」一节的 `meta list` 名单与 tag AND 筛选（含全库预检），两者由 `--allow-meta` 门控。本规格定义的格式、解析规则、全库预检与错误语义保持不变；`search` 的 `tag:` 操作符（本文「标签搜索与既有查询的关系」一节）仍不在 0041，另开 Issue。

### 文件头及实际状态变化

识别只发生在byte0。没有完整9字节`@yan-note`前缀的文件是旧格式，元数据为空；具有该前缀但marker不合法、版本未知、头未闭合或字段不合法时返回METADATA，不能猜测为空。

唯一合法marker为ASCII `@yan-note 1\n`，以ASCII `@end\n`闭合；识别到的头严格使用LF，不接受CRLF、缩进或注释；byte0不是marker的BOM前缀文件按无头旧文件处理，不跳过BOM寻找头。识别到的头内出现UTF-8 BOM字节序列（U+FEFF）为METADATA，禁令也适用于title/tag字段值的首、中、末位置，不忽略或保存该值。整个头包括marker/end最多4096字节，正文从end之后开始。正文出现相同文字不再解析。一个空合法头占17字节。

```text
@yan-note 1
title: 中断笔记
created: 2026-10-07
tag: os
tag: riscv
link: irq.txt
@end
interrupt controller
```

字段格式为固定小写键、冒号、一个ASCII分隔空格和完整值。字段只能是title/created/modified/tag/link；未知字段、重复单值、重复tag/link均METADATA。合法输入头可采用不同字段顺序，修改时可规范化为title、created、modified、各tag、各link；列表保持原有/add顺序，不排序标签或链接。缺省字段不序列化UNKNOWN或空值，删除最后一个字段仍保留空合法头。

| 字段 | 范围和身份 |
| --- | --- |
| title | 单值，1–256 UTF-8字节；单行、合法编码，禁止C0、DEL和C1，包括TAB；前后ASCII空格保留。空输入表示清除 |
| tag | 最多16个，每个1–63 UTF-8字节；合法编码，无空白和控制；精确原字节身份，不折叠或规范化 |
| link | 最多16个，沿YanFS 1–31字节ASCII名称规则，区分大小写 |
| created/modified | 各单值，严格十字节Gregorian YYYY-MM-DD，年份0001–9999，月份/闰年/日期真实有效；未声明为UNKNOWN |

C0为U+0000–001F，DEL为U+007F，C1为U+0080–009F。tag额外拒绝固定Unicode White_Space集合：U+0009–000D、0020、0085、00A0、1680、2000–200A、2028、2029、202F、205F、3000；不用Host locale决定。除上述控制/空白规则及头内U+FEFF禁令外，未列的合法字符按原字节保存，不承诺所有不可见字符的终端安全。正文不再按元数据头解析，正文中的U+FEFF保持原字节；marker前BOM仍按无头旧文件处理。

created/modified是用户声明的日期，不自动更新、不取CLINT instruction ticks作现实时间。mv/cp自然带走原头，不改其中日期/链接。链接只引用文件名；rename/delete后可能DANGLING，不自动改写其他文件或承诺稳定对象身份。

旧文件内容`interrupt\n`原在物理行1；添加tag os后变为marker/tag/end/body四行，正文命中行4。grep及无tag的plain search仍检索整个原文件，头文本也可命中，行号保持物理行号。编辑器继续编辑原始整文件，允许用户把头改坏；meta读取时再明确诊断，不偷偷修复。

### 命令及可观察输出

```text
meta show NAME
meta set NAME title TEXT
meta set NAME created DATE
meta set NAME modified DATE
meta tag NAME add TAG
meta tag NAME del TAG
meta link NAME add TARGET
meta link NAME del TARGET
meta list [tag:TAG ...]
search tag:os interrupt
search tag:os tag:riscv 中断
```

meta只在shell模式执行；编辑态先w成功或q退出。命令行仍沿0022的1023字节与整行拒绝规则。非法子命令、额外参数、缺参数或非法值为ERROR USAGE，零文件I/O；公开API相应返回INVALID。

title是title后首个分隔空格以后的整段TEXT，保留额外空格；无TEXT/空TEXT清除。DATE为单token日期或`-`清除。tag/link参数为单个原字节token，不解释引号/反斜线；列表不覆盖或排序。set同值、add已有值、del不存在值成功零写；清除缺省字段也无变化。名称须合法、源须存在、头须合法，不能通过无变化操作绕过坏头。

实际更新、清除或合法无变化成功，分别输出`OK meta set\r\n`、`OK meta tag\r\n`或`OK meta link\r\n`。普通错误输出既有`ERROR <token>\r\n`；发布成功后的回执输出故障沿0022致命退出，不回滚文件。

show成功输出固定顺序，值沿0022 cat安全显示：

```text
META note.txt
TITLE 中断笔记
CREATED 2026-10-07
MODIFIED UNKNOWN
TAG os
TAG riscv
LINK irq.txt OK
LINK missing.txt DANGLING
OK meta show
```

未声明title/date显示UNKNOWN，无tag/link分别输出`TAG NONE`、`LINK NONE`。LINK状态只看目标文件存在，不读目标正文；DANGLING不是FS错误。show完整校验当前头后才开始输出字段。输出换行均CRLF，不把编号/显示转义写回文件。

meta list按ASCII文件名升序，每文件一行名称，最后`OK meta list shown=N`。最多16个去重后不同tag筛选，多个AND；无过滤列出所有合法头/无头文件，旧无头文件标签为空。必须先检查全部文件的头，发现任意坏头则ERROR METADATA，无名单或OK；不是输出半份列表。空名单仍成功。输出失败立刻沿0022终止，不再读文件或写OK。

### 观察与更新的读取范围

show/list仅流式读取识别/验证头所需的prefix，不全文预检正文；旧无头文件正文可以是任意二进制，观察仍合法。没有16KiB文件大小上限，不扫描数MiB正文来显示头。头解析I/O错误仍传播；4096字节内未闭合则METADATA。

更新的顺序为参数/名称、源存在、合法头、是否真的变化。set同值/add已有/del不存在在这一阶段直接OK，不为正文继续发起读请求、不写盘、不规范化头、不分配新source token；即使正文是binary或文件超过16KiB仍可无变化成功。底层4096字节块读可能同时带出头后正文，限制的是观察范围及不继续扫描正文，不保证物理设备只读头字节。

只有实际变化才要求整个旧文件符合0023编辑器文本规则：合法UTF-8，允许已有TAB、LF和CRLF，拒绝其他C0/DEL/C1和裸CR。整个旧文件及候选新文件（头+正文）都不超过16384字节。先检查旧大小，超限TOO_LARGE；随后完整读/文本校验，非法INVALID_TEXT；构造完整候选、计算大小、验证格式后，调用yan_fs_replace恰好一次。正文原字节、LF/CRLF和无末换行原样保留。无头旧文本实际变更时插入canonical头。

NOT_FOUND、TOO_LARGE、INVALID_TEXT、METADATA、NOSPACE、其他既有普通FS错误不修改源。不存在的源不自动创建。实际I/O或协议错误沿0020/21停用/不回滚，不承诺失败目录写恢复。FS成功发布后输出失败也不回滚；缓存只在成功发布后更新，失败不得提前公开候选。

### 标签搜索与既有查询的关系

只有已注入元数据能力的shell在裸查询中把独立、大小写精确的`tag:TAG` token识别为过滤操作符；TAG规则与元数据一致。多个tag AND，精确去重后最多16个不同标签，必须仍有至少一个0026关键词组。tag-only为ERROR USAGE，文件级仅标签查询用meta list。空tag、超限、非法值均USAGE零I/O。tag可含标点/引号等合法非空白字节，suffix原样处理，不新增转义；先判断整条外引号，再拆独立tag token；tag suffix中的引号是合法原字节，不交给旧keyword grammar拒绝。移除tag token后，关键词自身仍受0026语法限制，包括禁止内部引号。

整条外引号search仍走0026词项语义，不识别tag操作符；它不是逐字节子串通道。要真正查询`tag:os`字面字节，用grep。关键词canonical去重后最多16组，标签数独立计算，评分只计关键词，不给标签加分。没有标签过滤时不解析元数据头，保持原plain search/grep的行为与暖索引读界限。

带tag的查询先沿0026对整文件UTF-8/NUL预检：非法文本/binary整文件skip并计一次，遇首个非法字节可短路。仅合格文本再解析头；任意合法文本文件坏头为普通METADATA错误，不能当成无标签假0，不能因其不在top20或标签不匹配而隐藏。先完成全部文件资格检查，再统计/排序/发布匹配；METADATA时无match回调、无OK summary。

无头文件资格为空标签。候选筛选必须进入统一term API/backend，shell只解析和格式化，不遍历FS，不先取20再过滤。SCAN、INDEX、暖查询、LIMIT/UNCACHEABLE回退的资格、total、score、top20和skipped一致；meta更新通过FS的source token同步使索引/派生资格失效。允许每次filtered query流式生成63槽资格，不承诺暖tag查询零额外I/O。整个query的FS稳定借用前提仍是调用方责任，不新增事务或强制锁。

### 公共接口、状态与内存

新增纯格式`os/note_metadata_format.h/.c`，包含共享info/filter/result类型与parser/encoder，不包含YanFS或shell。`os/note_metadata.h/.c`才是借用YanFS的服务层，不依赖shell输出类型，通过公开YanFS接口访问文件；term facade仅包含纯格式filter类型，保持独立于FS。pure parse由caller提供工作区，出错out保持，不用约2KiB自动临时对象累积栈。对象由应用长期静态持有并注入shell/term backend；解析info约2KiB、4096读buffer、16KiB候选工作区和list/filter scratch不放4KiB任务栈。不按文件大小分配，不缓存全库正文。索引及其metadata派生资格的总静态预算仍不超过2MiB，Host/RV32实际sizeof和栈测量后记录。

公共数据包括`YanNoteInfo`（has_header/header_bytes、title及存在标记、两日期及存在标记、最多16tags/links及长度、link_exists）、`YanNoteTagFilter`（至多16个不同标签）、`YanNoteContext`（借用YanFs、initialized/busy、静态buffer）。数组容量采用上述常量，UTF8长度按字节，链接沿32字节NUL终止FS名；缺省字段通过标记表达，不把UNKNOWN当真实值。

建立这些明确入口，具体公开enum/struct字段布局可在已批准契约内工程细化，但必须先定义header及独立测试，再实现：

```text
yan_note_init(context, fs)
yan_note_show(context, name, info_out)
yan_note_set_title(context, name, bytes, length)    # length=0清除
yan_note_set_date(context, name, created_or_modified, bytes, length)
yan_note_tag(context, name, add_or_del, bytes, length)
yan_note_link(context, name, add_or_del, target_name)
yan_note_list(context, optional_filter, sink, optional_summary)
yan_search_terms_filtered(terms, keyword_bytes, length, filter, sink, summary)
yan_shell_bind_metadata(shell, note_context)
```

date API length=0清除，其他必须十字节日期；管理mode枚举只允许created/modified及add/del。note结果区分OK/INVALID/BUSY/STOPPED/METADATA/TOO_LARGE/INVALID_TEXT与底层FS错误；错误token按名字输出，空间不足沿现有NOSPACE。term结果新增METADATA普通错误，保留旧值/旧接口语义；filtered vtable扩充独立入口，不强改旧query签名。旧yan_shell_init与旧term query原行为保持，通过新绑定启用meta与filter；未绑定不偷偷遍历FS。

公共调用检查顺序：context地址跨度有效（先于读取bool）、initialized、BUSY、借用FS initialized/FAULTED/MOUNTED状态、其他参数/跨度/可检测别名、源名/目标名、源存在、头、无变化、实际变化大小与文本、候选、发布。FS状态错误优先，BUSY不得写holder；无效初始化不写对象。所有输入/输出跨度不得溢出uintptr_t或重叠服务/context/backend/借用FS，双输出不得交叠，错误保持有效输出holder；输入有效至返回且不修改。观察结果/列表回调对象仅同步期借用，回调重入管理或query为BUSY、零新增I/O。初始化对象须零初始化。

list错误/STOPPED不公开成功summary；show错误不公开半份info。metadata解析是普通内容错误，不使FS FAULTED；真实底层I/O/协议沿旧fatal mapping。reader实际错误sticky优先STOPPED/OK，输出故障停止后续操作。source不可缓存时任何派生metadata身份缓存也不得复用；source getter仍纯观察，不新增reset。

### 阶段关系与不变量

[0021](0021-yanfs.md)与[0024](0024-file-rename-copy.md)格式及单文件发布保持；[0022](0022-terminal-file-operations.md)输出故障和读行、[0023](0023-multiline-text-editor.md)原文编辑与草稿、[0025](0025-knowledge-search.md)literal、[0026](0026-term-search-index.md)无tag查询及索引历史原文保留。本规格只扩充文件内容头、metadata服务和明确启用的标签筛选，不改旧阶段结论。

## IMPLE PLAN

1. 固定纯格式公共header、caller workspace、bounded UTF8头parser/canonical encoder/date/tag校验和纯内存tests-first；边界和坏头先RED。
2. note服务prefix观察、全库list预检、幂等/实际变化分流、单次replace与source失效；native注入I/O/协议/输出/回调重入测试。
3. filtered term入口、vtable扩充、63slot资格，SCAN/INDEX/LIMIT/UNCACHEABLE独立oracle；旧无tag调用和transitive inputs全部适配。
4. shell绑定、meta语法/输出、tag操作符、应用静态上下文，旧init兼容；生产OFF构建与真实Guest观察/保存/重启。
5. Guest故障与RV32跨度、真实owner-specific mutation/puregate、缺输入hard1/外部77；冻结后Linux/ASan/Windows完整默认、全部actual及Host/RV32布局/栈，最后文档与cached九项。

## IMPLE

纯格式`note_metadata_format`与服务`note_metadata`已实现。格式层通过caller workspace解析头并生成canonical头；服务通过公开YanFS接口完成prefix观察、全库list预检、无变化零写及实际变化单次replace。filtered term、shell绑定与meta命令、生产Guest接入和门禁均已实现；独立实现审查完成；该实施记录形成时，维护者实现交付与合入审查仍待。其后项目所有者已批准PR #24合入main（7451c85），见本规格的批准记录。旧0026的67/79/41及所有通过记录仅属交付历史，不作为本能力证据。

## VERIFY

| 面 | 实际输入/判据 |
| --- | --- |
| marker/头 | 无头、空头17B、4096边界/超限、unknownversion、prefix坏marker、CRLF/未闭合、未知/重复字段、title/tag首中末FEFF拒绝及正文FEFF原样；精确METADATA，无假空标签 |
| 字段 | title256/257bytes与保留空格；tags16/17及63/64bytes、White_Space全集合、case/不同UTF8规范形不折叠；links名边界/顺序/重复 |
| 日期 | 2000-02-29合法、1900-02-29/0000/无效月日拒绝；unknown缺省、清除、无自动tick |
| 观察 | 数MiB/binary正文show/list仅prefix、无头空元数据；合法头binary正文不误拒，目标存在binary LINK OK，dangling明确 |
| 更新 | 大/binary文件same-value/add重复/del不存在OK不读正文不写；真实change TOO_LARGE/INVALID_TEXT；16384总长边界、body原字节不改、一次replace/no source变化拒绝 |
| 输出/list | 全部头先检再输出，末槽坏头无部分名单；ASCII排序、无过滤/多tagAND、UNKNOWN/NONE、输出首中末故障与成功发布后不回滚 |
| 搜索 | tag精确AND且keyword至少一组、16重复/17不同tags、quoted旧语义/grep真literal、物理行号含头；晚坏头METADATA无callback/OK、binary先skip |
| 一致性 | scan/index/warm/LIMIT/UNCACHEABLE完整oracle；20后候选仍进total/top20，修改/rename/copy/clear/restart/source耗尽无陈旧资格 |
| API | RV32 UINTPTR_MAX、对象/FS/span别名、双输出、guard优先、BUSY reentry/输出保持、sticky真实错误、零初始化与生命周期 |
| 门禁/工程 | exact suite/owner/assertion及no-effect/wrong-owner/wrong-assertion/mixedharness；全部缺输入硬1/外部77、MSVC/ASan/productionOFF、总static<=2MiB与单帧非全栈说明 |

### 当前本机验收（2026-10-08）

Release81/81（54.70 s）、ASan/UBSan81/81（93.49 s）、Windows MSVC52/52（20.63 s）通过；统一十三组实际变异13/13（190.50 s），0失败、0SKIP。Linux当前81默认/94总计，六puregate不计实际变异组。原生格式55、服务27、缓存故障6、filtered13、shell21通过；独立格式oracle627，filtered oracle108夹具对照三backend路径。生产OFF构建与实际健康7项、19故障注入加3健康基线、RV32 API26检查通过。静态预算及单帧边界见[STATUS](../STATUS.md)，不冒称完整嵌套栈证明。

功能提交180d18e的PR #24三项远端CI已通过，配置与实测见[STATUS](../STATUS.md)；本机与远端结果分别记录，项目所有者已批准PR #24合入main（7451c85），学习理解仍由其判断。Spec Lock及FEFF所有者裁定保留。

### 分项推进历史（2026-10-08）

以下是当时的专项与待办快照，已由上述完整本机验收取代当前状态，保留原记录。

格式专项在Linux Release、ASan/UBSan及Windows MSVC通过：原生55例、独立Python oracle619例，覆盖编码容量17/16字节、title控制字符边界及完整字段规则。Linux两配置分别CTest2/2通过，日志 `build/search-meta-format-review-green.log` 与 `search-meta-format-review-asan.log`。服务Linux Release专项CTest1/1、原生27例通过，日志 `build/search-meta-service-green.log`；覆盖头观察、列表预检、幂等与真实更新、holder/别名、状态优先和底层错误。

filtered首轮Linux专项CTest4/4通过，原生13例及108个独立oracle夹具对照scan/index/index-repeat，含LIMIT回退；日志 `build/search-meta-filter-first.log`。真实source耗尽filtered专项通过，日志 `build/search-meta-exhaustion.log`。index跨度防御与缓存写回调观察修后，Linux专项3/3通过（filter13、fault6及真实耗尽）；cache/filter在ASan和Windows各2/2通过。RV32服务API23个检查与facade探针通过。metadata实际变异14个指定断言检出仅属格式/service/filter分项，尚未覆盖shell或全部变异组；真实Guest故障16项通过，但健康夹具和shell失败仍待修复复验，整体未验收。证据与具体边界见[STATUS](../STATUS.md)。

上述为专项结果。完整服务多配置、shell/健康Guest修后验收、完整默认、完整actual mutation、远端CI以及Host/RV32最终布局与栈验证仍待执行。项目所有者于2026-10-08裁定保留头内FEFF禁令并明确字段值也拒绝，授权记录见[STATUS](../STATUS.md)；新增八个独立oracle夹具后Linux专项627例通过，日志 `build/search-meta-format-current-rule.log`。此分项不代表整体已验收。

规格整体与上述工程具体化已获本轮授权；实施、验证、用户交付/合入审查分开记录。学习理解不由Agent代判。
