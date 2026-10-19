# 0032：具名块与精确正文引用

> 状态：**Spec Lock**（2026-10-18 03:38:42 +08:00）。项目所有者于2026-10-17明确认可整体方案，授权形成规格并继续实现；独立第1轮F1–F3修后复核及规格九审已通过，规格提交2f1d07cd保留。纯格式、只读FS服务与shell/app已编写，原生定向及新增实际变异组通过；新Guest健康/故障/API与完整默认回归已通过，统一17实际组已通过，独立第3轮恢复完成集成技术审核，第4轮精确树冻结对齐及F4-D1修后复核已通过，9b59954精确树九审已通过，功能提交11b0c247及PR #36已建立，对应功能head三项远端默认CI通过，配置范围见[STATUS](../STATUS.md)。项目所有者于2026-10-18批准PR #36按merge commit合入main（d8804be），阶段历史保留，理解程度由项目所有者判断。

## INTENTION

后续扩展：[0033](0033-block-mcp-navigation.md)已获准将本阶段块服务接入Host/MCP，当前处于实施阶段。下文“不增加MCP工具”保留为0032阶段范围，块格式与Guest服务契约保持。

给正文一段内容显式命名，前文增删后仍能用同一地址定位，并反查具体哪一处文字引用它。原文件是事实来源，所有查询只读、逐次计算；不增加磁盘格式、图数据库或第二事实源。

地址`irq.txt#irq-entry`只保证同一文件内的编辑稳定：ID保留时，插入前文、移动块或修改内容不改变地址；物理位置重新计算。mv后旧名称断链，删除再创建同名文件和同ID会重新绑定，不是永久对象身份。cp到另一文件后是另一个限定地址；副本内显式引用的原NAME仍保留，不自动变成副本自身。

本阶段提供Guest命令与可复用C服务。保留[0027](0027-note-metadata.md)的头和整文件link、[0028](0028-knowledge-relations.md)原三关系命令、[0023](0023-multiline-text-editor.md)原始文本编辑，以及[0030](0030-retrieval-context-json.md)/[0031](0031-mcp-retrieval-tools.md)的原文范围和schema。它们不因此理解块ID；不增加MCP工具、答案生成、自动ID、自动修链或多文件写回。

## SPEC

### 文本资格、头与正文

所有新查询先完整判定参与文件的文本资格，再解析头，再解析正文结构。文本沿编辑器规则：合法UTF-8，允许TAB、LF、CRLF；拒绝NUL、裸CR、其余C0、DEL和C1，拒绝overlong、surrogate、超过U+10FFFF和截断序列。不丢弃、替换或规范化字节。正文FEFF合法，识别到的元数据头内FEFF仍按0027拒绝。

查询不受编辑器16384字节整稿容量限制。使用有界缓冲流式预检完整文件，包括最后字节；资格不合时不能留下此前识别的块或引用。已遇明确非文本字节可停止该文件资格检查。头只在byte0按0027识别：无完整`@yan-note`前缀则body从0开始，匹配前缀而头错误为METADATA；合法头的`header_bytes`之后才是正文。二进制资格先于坏头判断，不在非文本文件中解释偶遇marker。

位置均为从文件byte0计的半开区间`[start,end)`和从1计的物理行号；LF增加物理行号，CRLF算一次，头行也计入。正文安全显示与原字节范围分离，显示编号/转义不写回文件。

### 块声明

正文整条物理行`@block ID`声明具名块。ID为1–31字节`[A-Za-z0-9_-]`，大小写精确，不Unicode规范化。列0开始，`@block`与ID之间恰好一个ASCII空格，无尾空格；终止符可为LF、CRLF或文件末尾无EOL。

保留标记前缀是行首完整`@block`后接ASCII空格、TAB或行尾。此类行不满足上述唯一语法时为BLOCK_FORMAT：空ID、TAB分隔、多空格、尾空格、非法字节和超长ID均不容错。`@blockchain`、缩进后的`@block`等不是保留前缀，按正文文字处理。需要写字面标记可在行首加普通字符；不解析Markdown代码围栏或转义语言。

块内容从marker终止符之后开始，到下一合法marker首字节或EOF结束；marker和它的终止符不属于内容。连续marker产生空块，无EOL的最后marker也产生空块。首marker之前为未命名区，不能猜成隐式ID。最后块保留已有末EOL或无末EOL。同一文件可混合LF/CRLF，均不改字节。

同文件ID唯一；任何本文件结构查询发现重复都报AMBIGUOUS，不选first或last。最多64个不同ID。按声明顺序逐条检查：先检查新声明是否与已记录ID重复，再检查容量；已有64时的重复为AMBIGUOUS，第65个不同ID立即LIMIT。已遇到LIMIT不继续扫描以寻找后面的另一个错误。跨文件相同ID合法，不是全库歧义。

空块`start==end`，其内容首/末行号都为0；非空块首/末行号指实际内容行，终止LF后不合成一条空行。marker自身物理行号单独保留。空文件没有块。

### 正文引用

只将完整合法的ASCII串`[[NAME#ID]]`识别为精确引用，NAME沿YanFS 1–31字节`[A-Za-z0-9._-]`且非`.`/`..`，ID沿上述规则。引用不跨物理行。未闭、拼错、过长、非法名称或带显示别名的串均按字面保留，不报告坏引用错误；这不是lint，不猜测修正目标。

按字节从左至右识别：当前位置若开始一个完整合法串则记录该出现并跳至其结束，否则前进一个字节。完整合法串可在正文任意位置，包括代码围栏内；不解释引用转义、相对地址、显示别名、URL或标题锚点。只扫描正文，不把元数据头中的文字当引用。

每个出现报告源文件、可选源块ID、引用起始物理行号、包含两端括号的原字节范围及目标NAME/ID。归属块只表示当前出现位置落在该具名内容区；首marker前或无marker文件中的合法引用仍记录，源块ID缺省。marker行不属于块内容，也不含合法引用。重复出现、自引用和同一行多次引用分别计数，不按文件或地址去重；没有引用数量上限或256条限制。

### 命令与固定输出

仅SHELL模式提供以下命令，EDITING先w/q返回；输入仍沿既有1023字节整行规则。恰好所列参数，以ASCII空格分隔，不解释引号/变量/转义；NAME#ID恰好一个`#`。语法、名称、地址或arity错误为`ERROR USAGE\r\n`，零文件I/O。以下所有数字均无前导零十进制，所有输出行CRLF。

| 命令 | 结果 |
| --- | --- |
| `block list NAME` | 声明顺序列完整64项以内块表 |
| `block show NAME#ID` | 预检后显示完整指定块内容，不限20行 |
| `block refs NAME` | 源字节顺序列前20处出向引用，统计全部出现；只解析显示项的目标 |
| `block backlinks NAME#ID` | 源文件ASCII顺序、再源byte位置，列前20处匹配引用并统计全部出现；允许缺目标 |

list每行：`BLOCK NAME#ID marker=N first=N last=N bytes=S:E`；空块first/last为0。回执：`OK block list total=N shown=N`，total==shown<=64。

show每条实际内容行输出`N TEXT`，N为整个文件的物理行号，TEXT按0023 p的安全显示：不输出原EOL、合法UTF8保持、反斜线双写、TAB转义，其余沿已选控制显示规则。服务原文仍保留全部LF/CRLF；空块不输出编号行。回执：`OK block show total=N shown=N`，total/shown为实际内容行数，不是块数或字节数。

refs每行：`REF source=NAME block=SOURCE line=N bytes=S:E target=NAME#ID state=STATE`。SOURCE有归属时为`#ID`，无归属为`NONE`，因此实际ID `NONE`与缺省不混淆。STATE为`OK`、`DANGLING_FILE`或`DANGLING_BLOCK`。回执：`OK block refs total=N shown=N`；total是全部合法引用出现次数，shown=min(total,20)。未显示目标不读取、不声称其健康，不按目标状态筛掉计数。

backlinks每行：`BACKLINK source=NAME block=SOURCE line=N bytes=S:E target=NAME#ID`。SOURCE规则相同。回执：`OK block backlinks total=N shown=N skipped=N target=STATE`；STATE为所查地址状态，total是全部精确匹配出现次数，shown=min(total,20)，skipped是库中因非文本整文件跳过数。缺文件/缺块仍成功列反链并明确断链；自引用不排除。

例如无块的`notes.txt`正文只有`[[irq.txt#irq-entry]]`，list为0块，refs为1处且block=NONE。不能从“无marker”推导“无引用”。

### 读取、错误优先与发布

源state/参数守卫先于介质I/O。list/show/refs先确认源存在，缺源NOT_FOUND；现存源资格不合INVALID_TEXT，合法文本坏头METADATA，随后正文按出现顺序报告BLOCK_FORMAT/AMBIGUOUS/LIMIT。show在完整源结构验证后查ID，缺ID为NOT_FOUND；不把空块与缺ID混淆。

refs完成源全文资格/头/块/引用扫描、计数并保留前20行，再按显示顺序逐个解析目标的文本/头/块。缺文件DANGLING_FILE，存在合法文本且无该ID为DANGLING_BLOCK；现存binary为INVALID_TEXT，坏头/块/重复/容量分别报对应错误，不能误报断链。普通内容错误发生在任何显示项时，整命令无REF行或OK；未显示目标不解析。

backlinks先观察目标存在性；若存在，完整检查其文本/头/块并决定OK或DANGLING_BLOCK，任何非文本/损坏错误先返回，不把目标当库中binary跳过。缺目标为DANGLING_FILE。随后按文件名ASCII顺序完整预检所有其它现存文件；非文本整文件skip并计数，合法文本的坏头/坏标记/重复/超容量均错误，即使该文件无匹配或在第20项之后。目标文件合法时参与自引用扫描且不重复计数。缺块地址仍可反查；任何普通内容错误均无BACKLINK行或OK。

list/refs/backlinks须完成相应扫描和显示行目标状态解析后才callback/输出；不建全库缓存，只保必要表。show先完整预检源，再按已确定range流式读取原文；该阶段真实I/O或输出失败可留下已发前缀；检测失败后不继续发送成功回执或进行后续文件I/O。新show不继承0028旧关系服务“callback后无FS读取”的保证。

终端最后字节可能已接受后才在post-write检查失败，因而已交付前缀也可能包含完整OK文本；shell/app仍须致命失败、不得健康返回或继续I/O，不能仅凭看到OK判断成功。

错误格式为`ERROR TOKEN\r\n`；新内容错误TOKEN为BLOCK_FORMAT、AMBIGUOUS、LIMIT、INVALID_TEXT，沿用METADATA及既有FS映射。内容错误普通返回，不停用健康FS；NOT_MOUNTED/FAULTED/IO/PROTOCOL/底层CORRUPT/UNSUPPORTED和输出拒绝沿既有终端致命规则，停止收命令且不返回成功。输出失败不能再可靠保证ERROR行；已有内容不回滚。实际读错优先于callback STOPPED，不能吞错装空列表。

所有操作零写盘。稳定FS借用仍是调用方责任，callback期间不得修改借用FS；不承诺快照、强制全查询锁、自动重试/修复。mv/rm不扫描别的文件改引用；cp保持所有原字节。编辑器仍可保存坏标记，只在新查询时诊断。

### 公共C责任与内存

caller须在service、parser/scanner及text-validator长期对象首次init前将该对象零初始化，随后由相应init接管；这项责任不泛化为结果row/summary等typed holder必须清零，它们按各API的有效空间与失败保持契约处理。

新增独立纯格式层`os/note_blocks_format.h/.c`和只读FS服务`os/note_blocks.h/.c`；纯层不include YanFS/shell，服务仅借用YanFs和纯层，shell只解析与格式化、应用静态注入。新独立`YanBlockResult`承载OK/INVALID/BUSY/STOPPED、上述内容错误与FS映射，不修改`YanNoteResult`数值或旧服务返回语义。

候选公共入口`yan_blocks_init/list/show/refs/backlinks`，分别提供类型明确的块描述/引用行、原文chunk与summary；工程精确结构可在本文行为内细化。块描述必须含ID、marker_line、first/last_line、start/end；引用行必须含源名、has_source_id/源ID、line/start/end、目标名/ID和必要状态。总引用数uint64，shown/skipped及文件位置uint32；file大小沿YanFS u32，先检查范围再计算端点，不绕回。

参数守卫：context完整可表示span→initialized→BUSY→借FS完整span/initialized/BUSY/FAULTED/MOUNTED→其它必要holder/sink/名称及完整字符串可检测span与别名。全typed holder/context/FS跨度在读bool或写out前检查，声明的保护对象不得重叠；借用字符串含终止符不得别名context/FS或被写out。init先验证context/FS跨度与交叠，活动对象BUSY，闲置对象可按现服务生命周期重新绑定，不I/O。无效参数不发布out、不I/O。

summary与类型明确的结果holder仅完整OK后一次发布；所有失败包括STOPPED保留原字节。行/chunk仅同步callback期间借用，不可保留指针，调用方自行复制；同context重入包括init为BUSY且零额外I/O。大块必须按chunk或等价受限reader访问，不许把整块长度配一个短暂连续缓冲指针；show的reader错误为sticky并按前述失败优先处理。opaque sink_context的对象总大小未知，不许虚称可验证任意caller对象；实现只承诺已声明可检测跨度保护。

每file64个ID/range表、20个引用输出行及4KiB扫描缓冲放长期caller context，不建63×64全库表或完整refs表。新增context上限64KiB，index/note/shell/relation及新增块context的总静态预算<=2MiB；实际Host/RV32 sizeof、生产ELF与单函数栈帧在实现后测量，单帧不冒充完整调用链/ISR峰值。旧编辑容量/运行时栈/传输协议不变。

## IMPLE PLAN

1. 纯流式格式接口与tests-first：UTF8资格、marker/引用跨4096边界、行计数/range、无marker有引用、逐声明重复优先及64/65。保留独立Python参考算法作语法/字节oracle。
2. FS服务与native tests：完整资格→头→结构、单file表/top20/u64、source/target状态/全库skipped、无部分表发布、rawchunk、alias/BUSY/错误holder；只经公开FS API，不按byte重复整块I/O。
3. shell可选绑定与生产app静态注入，新block族/help，未绑定保持旧UNKNOWN_COMMAND/help；EDITING路由不变，失败映射/生产启动错误明确固定，旧命令字节oracle保持。
4. 真实Guest健康/故障、wholeimage零写、跨重启/插入移动/mv/cp/重建行为、RV32API守卫、生产BUILD_TESTING OFF、实际sizeof/ELF/栈，来源缺失硬1/外依赖77控制。
5. 指定suite/owner/assert实际缺陷与no-effect/wrong-owner/wrong-assertion、异常优先负控；全量回归计数现测，独立角色对当前源码/接口/测试证据与全文reader对齐，chief exact九审后PR/真实CI/owner合入。

本阶段协作沿用户明确子Agent替代繁忙dsh授权，4主轮及最多1必要复核：1规格、2实施（各自必要修正同轮）、1冻结对齐；复用角色并关闭终态，不重启dsh/AGY旧任务。正式reader只由总审核落笔。

## IMPLE

规格已按准审树提交2f1d07cd。纯格式note_blocks_format、只读服务note_blocks及shell/app已实施；当前ASan格式25/服务17/shell9用例定向通过，Windows三native与新puregate通过，新增26项实际缺陷检出。CMake与生产OFF构建已通过；新Guest健康6组、故障43项、RV32 API及完整默认Release/ASan114组、Windows70组通过；统一17实际组17/17（322.77 s）通过。原独立第3轮已恢复完成并正式集成技术PASS，额度中断仅保留其历史；第4轮精确树冻结对齐及F4-D1修后复核已通过，9b59954精确树九项总审已通过，功能提交11b0c247与该树相同，已推送并建立PR #36；对应功能head三项远端默认CI通过，最终head b58432b的三项远端默认CI亦已通过，PR #36经项目所有者批准合入main（d8804be）。0031及旧阶段保持已交付历史。

## VERIFY

| 范围 | 必须观察的断言 |
| --- | --- |
| 格式与位置 | LF/CRLF/无末EOL、连续/末空marker、首未命名区、@block空格/TAB/尾空格保留边界、@blockchain普通；ID1/31/32、同文件重复/64后重复/第65不同立即LIMIT；引用完整合法才识别/跨行不识别/解码字节不改 |
| 文本与头 | >16KiB及100KiB长行可查、跨4096 UTF8/CRLF/marker/ref；末NUL/裸CR/非法UTF8整file资格优先，头FEFF拒/正文FEFF合法，badhead与binary优先，未命名区引用不丢 |
| 定位与反链 | 前插100行/移动/改内容不改ID仍定位，show空与NOT_FOUND区别，物理全文件行与半开range oracle；同一行重复/自引用、多于20仍fulltotal；refs第21目标坏不读，前20坏错误无行；backlinks晚无关坏结构无行、binary跳过及现存binary目标INVALID_TEXT |
| 公共生命周期 | context wrapping先于bool、BUSY优先其它参数、typedholder后缀alias/字符串NUL、错误out全保持、callback借用/重入、sticky读错优先STOPPED、所有普通/致命返回释放busy正确 |
| 失败与介质 | preflight首中末IO/协议，table输出前零结果，show流读及首中末TX失败后不再发送回执；已接受末字节后的postcheck失败可留下包含完整OK的前缀，但必须致命、无健康成功，源/目录wholeimage零写；不把timeout/signal/SAN/夹具失败当指定检出 |
| 兼容与工程 | 旧meta links/backlinks/related输出不变，edit仍原16KiB/可存坏语法，mv断链/cp地址独立且自引用字节不改/删重建重新绑定；0030原文/MCPschema不变；OFF生产/Host/RV32预算/硬1与外依赖77 |

验证矩阵列出验收要求；已执行的默认/实际组、耗时与覆盖边界见下方具名阶段记录，远端CI和所有者交付审查独立于本地测试。

早期局部阶段实证（2026-10-18，历史）：格式compiled stub RED25/25失败→strict GREEN25/25与ASan25/25；服务compiled stub RED14例13失败→strict GREEN14/14。Host纯格式parser3536B/text16B/最大单帧80B仅当前局部测量，不是整体/RV32预算或调用链证明；服务随后扩至17例ASan/UBSan通过；RV32去hosted-string最小修正后的当前源有独立6项strict ASan/UBSan边界通过，完整定向构建绑定仍待。独立格式1005输入/4020分块比较通过。CMake已声明目标不等于实际注册；shell stub/Guest/故障/全回归/实际变异未验。日志、职责与剩余范围见[STATUS](../STATUS.md)“0032 局部实施与定向验证”。上方VERIFY为后续验收矩阵，局部通过不代表矩阵已全覆盖。

完整Guest/默认回归前的集成阶段（2026-10-18，历史）：ASan三native3/3（0.11 s，25/17/9用例），Windows三native+puregate4/4（5.10 s），新增块实际26/26（25.901 s，0存活/0harness）及旧Guest gate3/3（13.610 s）通过；不是新Guest/全回归/统一17实际组通过。OFF五生产目标构建成功。Host/RV32 block26440/26424B，规格五context合计1645568/1645488B；RV32格式/service/shell/app单帧64/192/256/240B，ISRframe80B，不作完整栈峰值证明。旧阶段待验语句由此具名更新，最终独立因额度中断仍待，日志及边界见[STATUS](../STATUS.md)。

统一实际终局前的集成实证（2026-10-18）：31输入SHA一致；完整默认Release114/114（111.66 s）、ASan/UBSan114/114（172.64 s）、Windows70/70（67.44 s）零失败零SKIP。新健康6组（5.220 s）、故障43项（4基线/38注入/1启动失败，18.465 s）、RV32 API35个唯一检查码+marker/实tohost1通过，新Guest gate4项（15.535 s）分列。原driver上界22误判BLOCK_INIT23为harness已同步修正，unknown24仍harness，不算产品错误或检出；IO seam仅私有副本。新26actual及资源/OFF已验范围保持，统一17实际组未终局，原独立第3轮中断、第4冻结审核未做，不出当前完整独立PASS或CAN COMMIT。源码与日志、使用边界见[STATUS](../STATUS.md)“0032 当前集成默认回归与Guest范围”。

第3轮恢复前的统一实际终局记录（2026-10-18，历史时点）：17/17（322.77 s）零失败/零SKIP；完整明细已另存，新块26项指定suite/owner/assertion及实际负控通过，组CTest14.94 s与先前专项25.901 s分列。31冻结输入SHA后验保持。本文已执行验证的当前pending由此更新，旧具名阶段待验语句保持历史；剩余独立集成/冻结审因额度中断未完成，exact九审、提交/PR/CI/维护者合入仍待，无CAN COMMIT。

独立集成恢复完成（2026-10-18）：原角色同第3轮正式集成技术PASS、31输入SHA一致，无阻塞源码/框架finding。现存注册JSON分别Release131/114默认/17实际/10pure，ASan114/114默认/0实际/10pure（mutation OFF），Windows70/70默认/0实际/9pure；查询非新回归。前阶段中断/待验记录保持历史，由本条更新当前事实，不改批准/锁定/SPEC行为。第4轮精确树冻结对齐及F4-D1修后复核已通过；9b59954精确树九项总审已通过，功能11b0c247/PR #36及对应head三项远端默认CI已完成，最终head b58432b三项远端默认CI通过，PR #36已获批准合入main（d8804be）；此前待批时点保留历史，报告与配置边界见[STATUS](../STATUS.md)“0032 第3轮恢复完成与注册核对”。
