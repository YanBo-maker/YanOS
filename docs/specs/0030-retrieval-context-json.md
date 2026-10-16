# 0030：可核验的检索上下文包

> 状态：**Spec Lock**（2026-10-16 15:21:58 +08:00）。整体方案已由项目所有者批准，独立只读规格审核通过。规格已提交3e84826；evidence与JSON层已编写并通过初步定向验证，夹具修复后Release、ASan、Windows完整复验与统一实际变异通过；独立最终对齐及九项总审核通过，功能5299136的PR #32三项CI已通过；实现交付与合入审查待进行，理解程度由项目所有者判断。

## INTENTION

把一次Host语义查询的原文、物理出处和身份交给后续程序核验。例如查询“设备怎样通知CPU”，调用者既能回读结果的文件字节范围，也能确认结果对应哪个逻辑笔记库、模型、runner和配置。上下文包提供候选证据，不生成答案或置信概率。

本规格扩充[0029 Host语义检索](0029-host-semantic-search.md)的输出与证据API。默认文本输出、查询资格、分段、排序、模型输入、配置hash、旁置索引版本及失败规则保持；Guest、YanFS磁盘格式与Host块协议不变。不引入常驻会话或模型回答。

## SPEC

### CLI与格式选择

```text
yan_semantic search --disk-image FILE --index FILE --model FILE --runner FILE --query TEXT [--format text|json]
```

`--format`只属于search，省略等同`text`。显式`text`与0029默认输出及退出码完全一致。重复`--format`、缺值、除`text`/`json`外的值，以及rebuild/status携带该参数，均为USAGE、退出2；在文件I/O、身份hash或child启动前拒绝。原查询1–1023字节及UTF-8约束、其他参数检查保持。

JSON模式成功时stdout恰好一个UTF-8紧凑对象，随后一个LF；没有BOM、日志、MATCH行或OK回执混入。对象成员名固定如下，成员顺序不作为调用契约，results顺序为原排名顺序：

```json
{"schema_version":1,"query":"设备怎样通知CPU","source_sha256":"<64 lowercase hex>","model_sha256":"<64 lowercase hex>","runner_sha256":"<64 lowercase hex>","config_sha256":"<64 lowercase hex>","segments":1,"skipped":0,"shown":1,"results":[{"file":"irq.txt","first_line":1,"last_line":1,"start":0,"end":15,"score":0.5,"text":"设备通知CPU"}]}
```

上例hash字符串是字段宽度说明，不是合法实例值；实际值必须恰64个小写十六进制字符。上述示意text为15个UTF-8字节，范围为`[0,15)`。

| 字段 | 契约 |
| --- | --- |
| schema_version | 整数1 |
| query | CLI接收的原始TEXT；JSON解码后的UTF-8字节与输入一致，不含合成query指令 |
| 四个sha256 | 同次成功搜索已验证的逻辑来源、模型、runner与配置身份，按32字节顺序编码为64小写hex |
| segments / skipped / shown | 与同次READY summary一致的非负整数；shown等于results长度，最多20 |
| results[].file | 原YanFS ASCII文件名 |
| first_line / last_line | 0029从1开始的物理LF行号，包含头行；last_line按end-1定位 |
| start / end | 原文件半开字节范围，不含合成title前缀；0<=start<end |
| score | 原排序double的有限JSON数值，足以往返保持double精度，不先按文本显示的六位小数舍入 |
| text | 完整命中段原字节，最多512正文标量、2048字节；不是160标量显示摘要，不含合成title |

字符串依JSON语法转义引号、反斜线与控制字符。解码后query/text必须精确恢复原UTF-8字节；允许原CRLF、TAB等资格内字节，不能替换为屏幕显示转义文本。数字序列化不依赖locale，禁止NaN/Infinity。无候选或空库成功时results为[]、shown为0；其余身份和计数仍完整。相同文件的重叠段允许分别出现，沿0029全库精确cosine、原始double排序与稳定平局规则，不再重排、合并或去重。

### 证据API与生命周期

在`host/semantic.h`新增如下公开类型和函数；复用已有类型，不改变旧API或YanSemanticSummary布局、enum数值：

```c
typedef struct {
    YanSemanticSummary summary;
    YanSemanticIdentity identity;
    uint8_t source_sha256[32];
} YanSemanticEvidence;

YanSemanticResult yan_semantic_search_evidence(
    YanSemantic *ctx, const uint8_t *query, size_t length,
    YanSemanticRowFn sink, void *sink_context,
    YanSemanticEvidence *evidence_out);
```

核心仍为C17，无JSON或yyjson依赖。旧`yan_semantic_search`继续沿原契约发布summary；新函数复用同一搜索路径，不执行第二次查询或额外来源扫描。

context的有效地址跨度在任何字段解引用前检查；沿0029的initialized/BUSY防御顺序，活跃对象返回BUSY、零新I/O，即使其它参数无效也不覆盖该状态。正常闲置对象才验证其余参数。`evidence_out`必须非NULL且整对象跨度有效；其任何字节与ctx、所借source/provider/store上下文保护跨度或query范围可检测重叠，都返回INVALID、零I/O。仅前缀不重叠而后缀相交也必须拒绝，使用完整sizeof而不是summary大小。其它query/sink/保护span约束沿0029；不对无长度sink_context承诺任意内存别名检测。

所有失败保持整个evidence_out逐字节不变，包括参数错误、BUSY、STALE、CORRUPT、provider/source/store关闭失败、取消和sink返回false的STOPPED。仅在全部行callback成功、搜索收尾成功后一次发布完整evidence。summary必须READY且shown等于成功交付行数。source_sha256取本次corpus扫描结果，并已与索引校验相等；identity来自本次已验证的context副本，不在callback后另读索引或再取外部身份拼接。调用者继续承担整个query期间来源与借用身份稳定的前置条件，本API不增加跨进程事务或锁。

行callback仍同步借用YanSemanticRow/raw，仅在本次callback期间有效。core在首次callback前完成来源/索引校验、全部排名、所选完整原文回读及现有关闭流程；close_source失败不得交付callback。callback重入返回BUSY、零新增I/O。失败或STOPPED可以已有先前callback，不能把callback收到行等同evidence已成功发布。

### JSON输出与资源关闭

CLI的sink在有界私有存储中复制最多20行的出处、double及每行最多2048字节完整原文；不保存借用raw指针。20×2048字节为原文数据上限，另有元数据、query和JSON编码空间，不宣称整个内存占用只有40960字节。内存对象使用堆或静态上下文，不放Guest任务栈。

顺序为：参数检查 → 同次搜索及结果复制 → core成功/evidence发布 → 所有source/provider/store适配资源关闭与child回收成功 → 检查取消 → yyjson0.13序列化到完整内存 → stdout写入并flush。序列化及首次stdout输出都在资源成功关闭之后；不得先输出对象再发现close失败。保存/编码内存失败映射IO、退出5；在序列化前、首次stdout写入前及write/flush结束后检查取消；发现取消返回CANCELLED、退出5，已输出字节沿前缀规则处理，不能因JSON构建或写入已完成而误报成功。yyjson仅为可选CLI层私有依赖，默认core构建不增加JSON依赖。

首次输出前任意失败，stdout为空，stderr输出原有ERROR诊断及沿0029对应退出码；不发布部分JSON、成功空结果或OK。stdout写入/flush失败为OUTPUT、退出5，可留下任意已发送前缀，甚至全文已写但flush失败；不承诺stdout回滚。接收方必须同时检查进程exit0和完整JSON解析成功，再消费对象。text模式保持既有前缀失败契约，不因JSON缓存改变其输出。

### 不变量与阶段关系

只读源、索引身份校验、无自动重建、单写者/稳定来源、关闭错误传播及取消规则沿0029。新输出不改变embedding内容、配置身份或索引版本，不要求重新rebuild已有合法索引。0029原文保留为对应阶段与默认text契约，本规格只扩充search格式选择与evidence返回；未知schema_version由消费者自行拒绝，本阶段仅输出1。

## IMPLE PLAN

1. `host/semantic.h/.c`先补新API的原生RED：完整evidence跨度/后缀别名、BUSY优先、全部失败保持、source和identity同次绑定、关闭先于callback；实现共享搜索路径后GREEN，保留旧summary API覆盖。
2. `tools/yan_semantic.c`增加严格格式参数和有界结果收集；需要时分私有JSON模块。CMake仅在现有可选CLI层链接yyjson，不让core依赖序列化库。
3. 新CLI JSON脚本用独立解析器核完整对象、原文bytes/hash、顺序/精度/计数、字符串转义、空库及20上限；fake provider控制身份和排名。记录默认text逐字节兼容。
4. 覆盖STALE/CORRUPT/provider/source/adapter close、取消、内存/输出/flush失败，确认预输出stdout空及后输出失败须exit非0。扩充既有semantic实际变异与puregate，不新增独立组；每个缺陷绑定指定suite/owner/具体断言，保留no-effect、wrong-owner、wrong-assertion和harness优先控制。
5. 独立只读对齐规格、实际源码/接口、测试与全文当前文档；本阶段按用户明确授权由独立子Agent替代繁忙dsh，不称dsh通过。总审核修订后同角色复核，再exact cached九项审核、PR/CI及维护者合入审查。新能力未获合入批准前不自动merge。

## IMPLE

规格锁定提交3e84826已保留。`host/semantic.h/.c`已编写新evidence API并共享搜索路径；`tools/yan_semantic.c`已编写格式参数、最多20段完整原文收集与yyjson序列化。原生22例与JSON专项已通过；分配失败验证、26项semantic实际变异扩充及命名输入夹具隔离已完成，本机完整复验及JSON真模型验收已通过，独立最终对齐及九项总审核通过，功能提交5299136对应PR #32三项CI已通过；维护者交付合入审查尚待。

## VERIFY

| 面 | 实际输入与必须断言 |
| --- | --- |
| 格式解析 | 缺省/text一致；json成功；重复/缺值/大小写变体/未知值/rebuild与status携带format均USAGE2，zero file I/O/child |
| JSON语法 | 单紧凑对象+LF、无额外回执、独立解析、schema1、精确keys/types；query与全文UTF-8反解字节等于来源 |
| 原文范围 | 含头物理行、CRLF、TAB、引号/反斜线、多字节跨块、512标量/2048字节，末LF/end-1；不带合成title或160标量截断 |
| 身份与排序 | 四SHA精确小写hex；同次扫描及已验证identity；top20、重叠段、空库、double近邻分数不六位round，旧索引可复用 |
| API防御 | NULL/地址回绕/前后缀部分别名/各借用context别名，BUSY组合优先；失败整个out snapshot保持，重入无额外I/O |
| 关闭与发布 | source close发生在callback前；provider/store/适配cleanup失败不输出JSON；callback STOPPED不发布evidence；全部成功才发布 |
| 错误与输出 | STALE/CORRUPT/provider/取消/内存失败stdout空；首中末write与flush失败OUTPUT5，不补OK，全文已写仍可失败 |
| 工程与检测力 | core无yyjson依赖、默认Guest无变；新原生/CLI/实际变异与负控、硬缺输入/外依赖77，独立文档对齐及真实注册数 |

历史阶段定向证据（2026-10-16，最终复验前）：原生RED为原17例通过、新5例UNSUPPORTED失败，GREEN为22/22；JSON旧CLI实际USAGE2未通过，新实现专项CTest1/1（5.30 s）通过。日志及运行权限未执行事件的区别见[STATUS](../STATUS.md)。完整回归、分配失败/实际变异、独立实现审核、远端CI与维护者交付合入审查仍待；不将这些专项写为整体通过。0029已有本机、实际变异、真实模型与CI记录保留为历史，本次不改变检索模型或效果承诺。

历史阶段补充（2026-10-16，夹具修复前）：JSON独立oracle40场景、1个Unity用例内部16个分配失败注入点，以及semantic26个指定断言实际缺陷专项已通过。首轮完整默认回归Release/ASan/Windows分别有2/4/2个旧gate失败，原因为复制整个仓库时纳入无关动态文件；夹具输入隔离修复与完整复验尚待。详细计数、日志及分类见[STATUS](../STATUS.md)，本规格行为与锁定时间不变。

历史修复后阶段记录（ASan及统一变异终局前）：Release102/102（270.73 s）、Windows65/65（123.03 s）通过，ASan与十五组实际变异尚待最终结果；独立增量审核通过。生产工具显式JSON真模型验收10查询/200出处通过，模型效果仍限固定小语料；完整记录与各类hash区分见[STATUS](../STATUS.md)。先前失败记录保留为执行历史。

2026-10-16最终本机复验：Release102/102（270.73 s）、ASan/UBSan102/102（406.37 s）、Windows65/65（123.03 s），统一十五组实际变异15/15（380.18 s，-j3），均0失败、0SKIP。semantic26项指定断言缺陷、JSON40场景、1个Unity内16个分配失败点及显式真实JSON10查询/200原文出处通过。以上阶段待验快照保留，当前验证状态由本条取代；详细日志、注册范围与真实小语料限制见[STATUS](../STATUS.md)。独立增量实现审核通过，独立最终对齐及九项总审核通过，功能提交5299136对应PR #32三项CI已通过；维护者交付合入审查尚待。

功能提交5299136172ab30594501cb978240ed69298e6acc、PR #32及特定head远端CI的实际配置与68/68/65计数见[STATUS](../STATUS.md)。独立只读第4轮与必要机械修复复核关闭F1–F4，规格锁定3e84826与行为保持；未以CI成功代替维护者合入批准或用户理解判断。
