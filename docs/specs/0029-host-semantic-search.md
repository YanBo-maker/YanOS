# 0029：Host侧语义检索

> 状态：**Spec Lock**（2026-10-14 21:11 +08:00）。项目所有者已认可Host语义检索整体方案，并授权形成规格、继续实现。实现、本机完整回归、实际变异及固定小语料真实模型复验已完成，独立dsh对齐与总审核通过；功能提交9ad39ed的三项远端CI已通过，项目所有者于2026-10-15明确批准，经PR #30合入main（79fdde6），学习理解由项目所有者判断。

## INTENTION

输入“设备怎样通知CPU”，寻找讨论中断、PLIC或UART的原文段落，即使笔记没有查询中的原词。结果是可回读的候选出处，不生成答案、不操作笔记。原始YanFS文件仍是事实来源。

新增可选Linux/WSL Host工具`yan_semantic`与独立API。Host核心采用C17；模型推理在独立本地进程。Guest的grep/search/related、ISA、设备、Host块协议和YanFS v1均不变，不把模型或向量装进Guest。

## SPEC

### 入口与可观测结果

```text
yan_semantic rebuild --disk-image FILE --index FILE --model FILE --runner FILE
yan_semantic search --disk-image FILE --index FILE --model FILE --runner FILE --query TEXT
yan_semantic status --disk-image FILE --index FILE --model FILE --runner FILE
```

参数顺序自由，重复/未知参数、缺值或多余位置参数为USAGE；路径显式指定，不自动选择镜像、模型或索引。可选`--startup-timeout SEC`和`--request-timeout SEC`默认为120秒，取整数1–3600；非整数、溢出或范围外均为USAGE/退出2，零provider请求和索引写入。deadline使用单调时钟。默认核心/Guest构建不要求模型、runner、libcurl或JSON依赖。新增工具由单独构建选项启用，Linux/WSL为本阶段生产入口；不宣称Windows工具已可用。

查询TEXT为1–1023字节合法UTF-8，不允许LF/CR、NUL、DEL、C1及除TAB外C0；空或全Unicode White_Space拒绝。White_Space集合沿0027，解析不依赖locale。API接收显式长度，内嵌NUL不截断，CLI shell引号只负责传入一个参数，工具不再解释引号、转义或tag操作符。查询前错误为USAGE/INVALID，零provider请求。

成功输出以LF结束：

```text
OK rebuild segments=N skipped=N
STATUS READY segments=N skipped=N
MATCH note.txt lines=4-6 bytes=120-860 score=0.812345 CONTENT
OK search shown=N segments=N skipped=N
```

rebuild/search成功及status READY中的skipped均为本次全文件文本预检判为非文本的逻辑文件数，每文件最多一次、范围0–63，与同身份索引头skipped_count一致；合法空文件、title-only与纯空白段不计skip。rebuild发布该计数，READY/search重新核对同身份计数，不一致为CORRUPT。非READY status只输出`STATUS MISSING|STALE|CORRUPT`及LF，不输出不可靠计数；API非READY summary也不发布计数。

search最多20段，`shown=min(20,segments)`，空库可为0；segments是全库已索引有效段数，不是命中概率或相关笔记数。每个MATCH的文件名、原文件半开字节范围、物理行范围和安全摘要来自原文。正文摘要为该段起始最多160标量；控制字节/反斜线按0022 cat安全显示规则转义，不把标题前缀显示成正文。真实LF/CRLF以转义显示，单条记录不被原文换行拆开。score固定六位小数，排序使用未格式化的计算值；不将分数称为答案置信度，也不保证无关问题零候选。

普通诊断为stderr `ERROR TOKEN\n`；输出错误尽可能诊断，不能补OK。退出码固定：0成功（包括status的四种观察状态）、2 USAGE/INVALID/ALIAS，3 MISSING/STALE/CORRUPT/METADATA/LIMIT，4 DEPENDENCY/PROVIDER/TIMEOUT/UNSUPPORTED，5 IO/OUTPUT/CANCELLED。实际错误不得伪装为零结果或静默回退字面检索。

### 只读来源与文本资格

先停止使用该镜像的Guest。调用方保证源镜像、模型、runner及索引在一次命令中稳定，索引只有一个写者；没有并发重建/查询事务、全库锁或快照承诺。绕过该契约的并发修改不受支持。

独立Host文件适配以`rb`/只读描述符打开已有镜像，通过YanFS挂载和读取；write callback只能拒绝，不复用`r+b`后端冒称只读。不创建、格式化、修复镜像。所有成功/失败路径镜像与模型不变，读或关闭错误必须传播。

每文件先流式预检整个原始文件。含NUL、非法UTF-8、裸CR或禁止控制字节的文件整文件skip并计数；允许TAB、LF、CRLF，C0其余/DEL/C1不允许。非法字节出现很晚也不能留下此前索引段。通过预检的文本再按0027解析bounded头：合法无头文件直接正文，识别坏头整操作METADATA，不发布索引或任何MATCH/OK。只有整文件通过文本预检才有解析坏头的机会；“坏头+晚NUL”按非文本skip，该文件零段，其他合法文件仍可正常重建并发布整个索引。“合法文本+坏头”才使整操作METADATA、不发布索引或MATCH/OK。

头遵循0027全部规则，FEFF禁令包括title/tag值；正文FEFF允许，byte0 BOM不匹配marker仍是无头旧文本。合法头从embedding正文移除，正文保持原字节；title可构成下面固定前缀。没有编辑器16KiB文件限制，不要求完整正文驻内存。每个文件只跳过一次；无头空文件合法、不算skip。

### 确定分段与出处

按文件名ASCII顺序处理正文，按原文偏移顺序分段。每段最多512个正文Unicode标量，record.scalar_count只统计该段原正文，不含合成title前缀；embedding输入因此可超过512标量。步进目标重叠64标量。段末若落在CR与LF之间，向前收缩一标量；下一起点从段末回退64标量，若落在CRLF中再回退一标量。因此重叠64或65，始终<=512、UTF-8/CRLF完整、严格向前推进。非末段长度511或512、起点步进最小446标量；不以447当下界。例如原文标量0起步、CRLF位于446–447及511–512时，首段末缩为end=511，下一start从447回退至446，重叠65、步进446。最后一个覆盖正文末尾的段处理后结束，不因重叠生成额外尾段。

纯White_Space段不入索引，但不改变后续分段边界。空正文即使有title也无候选。最多8192个有效段，第8193段使整次LIMIT，禁止发布截断库；恰8192允许成功。

记录原文件`[start,end)`字节范围，0<=start<end<=文件长度。物理行号从1开始，仅按原文件LF累计，包含元数据头行；first_line是start所在行，last_line是end-1所在行，因此末尾LF属于它结束的行。范围不包含合成title前缀。单行超过512标量可产生多段，同文件/重叠段允许占多个top20位置，不自动合并为笔记级结果。

文档embedding输入固定为：存在title时`Title: `+原title字节+`\n\n`+本段原文，否则仅本段原文。query输入固定为`Instruct: Given a question, retrieve relevant passages from notes.\nQuery: `+TEXT；不对文档套query instruction。这些精确字节、分段/资格/摘要版本均进入配置身份。

### 模型进程与provider边界

真实验收基线为官方Qwen3-Embedding-0.6B-GGUF Q8_0，文件约639MB，使用1024维与last pooling。权重大小、量化、模型SHA-256、runner实际可执行文件SHA-256及构建版本、参数均记录；模型昵称和版本字符串不能替代真内容hash。下载/构建只在已批准的任务目录进行，不改系统全局配置；生产工具不自动下载、不接云服务。

每次rebuild/search拥有自己创建的本地llama.cpp子进程；status不启动推理进程。随机认证凭证每次重建，通过认证且确认本child存活后才接收响应，不能把已有同端口服务当作本次模型。仅连接回环，显式禁proxy和redirect；端口占用/child提前退出报错，不终止已有服务。使用libcurl和固定版本yyjson，不在core手写HTTP/JSON。HTTP错误、畸形JSON、索引/维数不符、超大响应、超时、NaN/Inf或零范数都是错误。

单次embedding对应一个输入，只接受恰1024个有限float值。响应原始大小上限1MiB，JSON所有数组/类型/计数受限；不允许隐式截断超长输入。输入不足上下文时正常返回，若runner不能接收则PROVIDER，不装作成功。startup/request deadline独立，取消和任何返回路径均回收本次child；先TERM，最多5秒后KILL并wait，只作用于本次拥有进程，不杀其他模型、dsh或用户服务。provider错误不得被callback停止或成功覆盖。

### 派生索引、身份与发布

索引是Host旁置、可删除重建的派生文件，不写Guest source_token。源SHA-256按ASCII文件顺序覆盖全部逻辑目录项的槽号、名称长度/字节、文件长度和完整内容，整数使用小端uint32；包含被skip文件、头和正文。镜像空闲块/失败遗留数据不是逻辑内容，不作为来源。物理槽变化也使身份不同，不能用mtime/大小代替内容hash。

每次search/status重新读取全部逻辑库并hash源、模型和runner身份；性能代价公开，不缓存mtime假装稳定。配置SHA-256覆盖工具索引/预处理版本、1024维、量化/pooling、query/document前缀、runner构建版本和固定推理参数，使用显式长度字段避免拼接歧义。模型/runner/配置或逻辑文件任一变化即STALE，拒绝旧候选，要求显式rebuild，不自动重建。

v1二进制索引固定小端：256字节头，magic八字节`YANSEM1\0`、version=1、dimension=1024、segment_count、skipped_count、header_size=256、record_size=4160均uint32；随后source/model/runner/config四个32字节SHA-256，32字节payload SHA-256，余64字节保留零。每record为64字节出处（name[32]规范零填充、start/end/first_line/last_line/scalar_count五个uint32、12字节保留零）和1024个IEEE754 binary32向量。未支持binary32的平台返回UNSUPPORTED，不按native struct直接写盘。

检查magic/version/维数/计数上限（segments<=8192、skipped<=63）、乘加溢出、实际文件精确长度、保留零、hash、规范名称与记录范围/行号/标量数、向量有限非零；不得按坏长度先分配或访问。记录按ASCII文件名/偏移严格有序，不允许重复段。payload hash用于意外损坏检查，不是可信签名；输入索引不能绕过当前来源和范围验证。

三命令先检查参数及路径别名，再验证可读源及model/runner身份，之后才观察索引。源打开/读取/关闭失败为IO/退出5，源YanFS格式非法为CORRUPT/退出3；model或runner缺失/不可读为DEPENDENCY/退出4。故模型缺失且索引缺失仍为DEPENDENCY，而不是STATUS MISSING。status必须完成这些前置验证才能观察四种索引状态，不能假READY。完整输入下：无索引MISSING；坏结构/校验/记录CORRUPT；结构合法但身份不同STALE；所有身份及内容/范围有效READY。按结构检查再身份比较、同身份再验证出处的顺序，不把源变化误作CORRUPT。非READY的search分别返回对应错误，零provider/零MATCH；status不生成索引。

rebuild用目标索引同目录排他创建独立临时文件，所有data写入、校验、flush/close及本次child正常结束成功后，才POSIX rename原子替换索引。发布前任何失败旧索引原字节保留；旧索引能否查询仍按当前身份判定。rename失败保留旧目标，清理本次临时文件，不删除别人的文件。发布后回执输出失败新索引保持，不承诺回滚或fsync/断电持久性。

镜像、模型、runner、目标索引与临时文件不得实际别名重叠；核对设备/inode、硬链接和符号链接，不仅比较字符串。临时创建使用排他语义；不得通过目标symlink写输入。所有路径冲突/无效参数在provider或任何索引写入前拒绝。没有多写者竞争保证。

### 向量排名与API责任

核心不依赖console、shell、Host终端输出或llama实现。建议生产模块分为`host/semantic.h/.c`（facade/索引/排名）、只读source适配、纯分段器和provider接口；工具只解析命令、管理依赖、格式化结果，不让调用方解析CLI文本作为API。

工程header固定结果enum、上下文和source/provider vtable。facade提供init/rebuild/search/status，result sink同步借用row，包含name、byte/line范围、double score和原文安全显示前raw摘要；summary仅全操作OK发布。大缓冲/向量表为调用者持有或有界Host heap，不塞Guest栈。query/输出holder/context、source/provider对象span与可检测别名必须先检查再解引用；活跃context重入BUSY，不做新增I/O/provider请求；init不得覆盖活跃对象。失效输出保持，callback false为STOPPED，CLI映射OUTPUT失败，不能补成功。

double累加dot和两个范数，用全部1024维计算cosine，结果须有限，浮点微差可钳到[-1,1]。精确遍历全部向量，不引ANN或数据库。排序score降序，相等时ASCII名称再start偏移，top20在全库排序后取；格式化后的相同score不构成原始相等。没有跨模型版本或硬件完全同浮点排序承诺。

search在首次输出前完成全部源/索引/模型验证、query embedding、全量排名、全部所选原文回读和摘要验证，失败零MATCH/OK；输出期间错误可留下精确已发送前缀，立刻终止，不补OK。不更新索引或笔记。

### 阶段关系与来源

[0021](0021-yanfs.md)文件格式/失败语义、[0025](0025-knowledge-search.md)字面检索、[0026](0026-term-search-index.md)Guest词项索引、[0027](0027-note-metadata.md)格式与资格、[0028](0028-knowledge-relations.md)关系导航保持历史与现有行为。本规格新增独立Host检索，无Guest语义替代。

模型与runner用法依据[官方GGUF说明](https://huggingface.co/Qwen/Qwen3-Embedding-0.6B-GGUF)、[Qwen模型卡](https://huggingface.co/Qwen/Qwen3-Embedding-0.6B)、[llama.cpp server](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md)；客户端依据[libcurl](https://curl.se/libcurl/c/)与[yyjson](https://github.com/ibireme/yyjson)。实施时固定真实版本/hash，不以浮动文档链接代替依赖身份或本机效果证据。

## IMPLE PLAN

1. 先固定source/provider/facade、CLI结果与索引布局；纯只读源/预检/分段tests-first语义RED，独立Python oracle核半开范围/UTF8/CRLF/LF物理行号、空白/title-only与8192/8193。
2. 实现身份hash、索引校验、精确cosine/top20和原文回读；fake provider覆盖所有失败、输出原子/前缀、重入/别名、旧索引保留与发布边界。
3. 实现独立rb适配、libcurl/yyjson及受控真实child；进程启动/占用端口/认证/退出/超时/取消及路径inode别名测试。工具可选构建、核心默认与Guest无依赖增加。
4. 在批准任务目录固定模型/runner/依赖，预标定中英文语料/问题集实测Top5、误检、零literal命中语义案例、耗时和峰值内存。fake只证明可靠性，真实缺依赖记待验，不拿fake冒充效果。
5. 完整相关回归、实际指定suite/owner/assertion变异与puregate、缺仓库输入硬1/外部依赖77、production工具实物验收；独立只读dsh对全篇当前文档与实际源码/证据做对齐，chief修后同会话复核，再exact cached九审交付。

## IMPLE

已编写纯SHA、text预检与chunker、整库source hash/文本分类/元数据/段数上限，以及POSIX只读来源与临时发布适配。上述部分及facade/索引已实现并完成定向验证；`host/semantic_llama.h/.c`提供libcurl/yyjson与受控child，`tools/yan_semantic.c`提供CLI。CMake以`YAN_BUILD_SEMANTIC`可选启用Linux工具，查找CURL/yyjson CMake package，默认核心与Guest不要求这些依赖。provider/CLI与完整生产工具已实现，冻结树默认回归、实际变异及生产OFF真实模型小语料复验通过，独立dsh对齐及总审核已完成，功能提交9ad39ed的CI与维护者合入审查分别记录。依赖准备与安装不等于实现通过。

## VERIFY

| 面 | 输入及判据 |
| --- | --- |
| 来源/资格 | rb只读、镜像与模型所有路径不变；late NUL/UTF8/control整file skip，坏头+NUL先skip且其他文件可发布，合法文本坏头METADATA；无头/BOM/正文FEFF/裸CR |
| 分段 | 511/512/513、跨块多字节、CRLF边界与64/65重叠、末段不重复、巨大文件无16KiB限制、title-only0、恰8192与8193LIMIT |
| 出处/排序 | 物理头行计数、末LF/end-1、byte半开、160标量安全原文、同file多段、分数平局、全库后top20与double独立oracle |
| 身份/索引 | 每个逻辑文件含skip者变更/rename/槽变化、model/runner实hash与配置变化STALE；坏尺寸/溢出/IEEE/哈希/有限性CORRUPT；status不启模型不假READY、缺模型与缺索引组合DEPENDENCY、READY计数同头/非READY无计数 |
| 发布/别名 | same-dir独占temp、每write/flush/close/child/rename故障、旧目标保持、成功后TX失败新保持；硬链接/symlink/inode冲突；无断电承诺 |
| provider/生命周期 | 真child与假同端口/错auth、timeout/redirect/proxy/超响应/错误JSON/维数/NaN/zero、结束回收、API BUSY/范围/输出别名与保持 |
| CLI/工程 | 精确结果/exit码、timeout非整数/溢出/0/3601为USAGE零provider、错误前零MATCH、输出后失败精确prefix不补OK、可选依赖/默认Guest不变、指定assert变异控制与硬1/77 |
| 真实效果 | 预标定中英文Top5相关出处/误检、至少一例无literal命中、固定模型runnerhash、实际加载/重建/查询耗时及峰值内存，fake与真效果分列 |

2026-10-15冻结树：Release100/100（294.77 s）、ASan/UBSan100/100（423.51 s）、Windows MSVC65/65（141.66 s），统一十五组实际变异15/15（321.87 s），均0失败、0SKIP。Linux工具/provider开启注册100默认/115总计、八puregate；facade17/SHA8/text34/corpus9/POSIX5，文本oracle690、排名oracle50、provider24、CLI31通过。semantic19个实际缺陷须命中指定suite/owner/断言，三类实际负控及puregate10项控制通过。历史专项与真实问题修复保留在[STATUS](../STATUS.md)，不混作当次全量执行。

生产OFF实际工具最终重建后，固定21笔记/10问题真实模型复验中8个有标签问题Top5各有相关出处，8个无literal命中；两无关问题仍各返回20候选。报告绑定当前工具/model/runner真实SHA及原文出处，重建7.63 s、status1.90 s、query2.62–2.84 s、峰值RSS1213312 KiB，只小语料/本机结果。`tests/semantic_model_check.py`是操作员显式运行入口，不注册默认CTest、不进入普通CI；缺工具硬失败、缺外部模型/runner/time明确77且不算通过，命令见[README](../../README.md)。fake可靠性通过不能冒充真实效果。

当前实现与本机验收、独立dsh对齐/总审核已完成。功能提交9ad39ed已建立PR #30并通过对应三项CI；远端TOOLS/SEMANTIC关闭的计数与包装后验见[STATUS](../STATUS.md)，不替代本机完整工具与人工模型验收。项目所有者已于2026-10-15明确批准PR #30合入main，merge79fdde6与已审交付树一致，阶段历史保留；不把本机、小语料、CI或合入批准当成用户理解判断。
