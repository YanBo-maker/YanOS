# 0028：知识关系导航

> 状态：**Spec Lock**（2026-10-11 16:24 +08:00）。项目所有者明确批准“认可整体方案，形成规格并继续实现”。规格已批准，实现与本机验收已完成；功能提交be99b8e的三项远端CI已通过，项目所有者于2026-10-14明确批准，经PR #26合入main（8349b7d），学习理解由项目所有者判断。

## INTENTION

> 后续扩展：[0036-R4](0036-yanfs-multi-block-directory.md#0036-r4上层服务在大容量下行为正确) 将容量改为镜像实际 N，并按项目所有者 2026-10-21 裁定，把普通 backlinks 统一改为前 20 项、同时报告完整 total 与实际 shown（v1、v2 均适用）。本文件原来的全量输出示例和 total=shown 是对应阶段记录；全库预检与错误优先级保持。

从现有笔记的link和tag声明导航知识关系：查看向外链接、追查谁引用一个名字，并用直接链接和共同标签解释相关笔记。所有关系来自原始文件的0027元数据头；不读取正文做相似度，不接入语义模型，不改变YanFS v1、Host块协议、ISA或运行时。

本阶段只读查询，不自动修复断链、不改写其他文件，也不建立持久图数据库。

## SPEC

### 命令与结果

```text
links NAME
backlinks NAME
related NAME
```

NAME沿YanFS的1–31字节ASCII名称规则，区分大小写。三个命令都只接受一个名称，缺失、多余或非法参数输出`ERROR USAGE\r\n`，零文件I/O。仅shell模式路由，编辑态须先w成功或q退出。命令行上限、整行拒绝与输出失败沿0022。

**links**要求源存在，读取源的合法元数据头，按原始声明顺序输出每个link；目标只查目录存在性，不解析其头或正文：

```text
LINK irq.txt OK
LINK missing.txt DANGLING
OK links shown=2
```

自身链接按原声明保留且目标存在时为OK。旧无头文件没有声明，直接`OK links shown=0`，不额外输出NONE。一个存在但头损坏的目标不影响源links观察；源头损坏返回METADATA，无结果行或成功回执。

**backlinks**把NAME作为被引用的合法名字，允许目标不存在。预检全库头后，按来源文件名ASCII升序输出恰好声明了该名字的现存文件，每个文件一次：

```text
BACKLINK a.txt
BACKLINK z.txt
OK backlinks shown=2 target=DANGLING
```

回执target为目录存在性的OK或DANGLING，不取决于是否有人引用。不存在的目标不报NOT_FOUND，也不生成虚构节点；零结果仍成功。若目标存在且声明自身，目标文件作为来源保留。

**related**要求源存在，排除源本身。候选是至少一个方向有直接链接，或与源具有至少一个共同tag的现存文件。只列现存候选，源声明的悬空名字不成为结果行。对每个候选：

- out为源是否声明候选名，值0或1；in为候选是否声明源名，值0或1。
- tags为精确共同tag数，范围0–16。沿0027原字节身份，不折叠大小写、不Unicode规范化；空集合之间的共同数为0。
- 排序依次为`out+in`降序、tags降序、文件名ASCII升序。每个文件一行，不计算传递关系或正文相似度。

```text
RELATED b.txt out=1 in=1 tags=0
RELATED c.txt out=1 in=0 tags=3
RELATED d.txt out=0 in=0 tags=2
OK related total=3 shown=3
```

双向链接即使没有共同标签，也排在有三个共同标签的单向链接前。最多显示20项，total必须统计全部候选而不是已截断的20项；目录最多63文件，所以related total最多62。无候选为`OK related total=0 shown=0`。

所有行使用CRLF；合法ASCII文件名可直接显示。普通错误输出既有`ERROR <token>\r\n`，不能同时输出成功回执；真正I/O/协议错误与输出故障沿0021/0022致命停用，不继续请求块设备或接收命令。查询不写盘，任何返回路径都必须零write。

### 输入、预检与可见状态

links只校验源头；backlinks与related必须先完整验证所有现存文件的头，包括非候选、不会显示的第21项以及目标本身。任意识别坏头返回METADATA，开始输出前就失败，不提供半份名单。真实读错误同样必须在输出前传播；输出开始后不再执行文件I/O。

related的源不存在优先NOT_FOUND，不先扫描远处文件并用其坏头遮盖缺源。backlinks不检查目标存在作为前置成功条件，但仍按全库规则校验所有现存头。links只查目标是否存在；不能把目标的坏头扩大为本命令错误。

沿0027元数据观察契约，只读取识别和验证头需要的bounded prefix，上限4096字节。旧无头文件合法且元数据为空；合法头后的正文可以是任意二进制，也不受编辑器16KiB上限限制，不做全文件UTF-8/NUL预检或skip统计。读取物理块可能同时带出正文，不承诺设备只读取头字节。marker、日期、tag/link、FEFF及所有坏头规则完整复用0027。

每次查询重新计算，不持有可复用的关系缓存；meta更新、create/replace、mv/cp/rm后，下一查询从当前文件重新观察。链接仍是原名字：mv/rm不会自动改写其他文件的link，cp携带原头和其声明。无需source token缓存；即使身份耗尽，仍可只读计算。

整个查询期间FS稳定借用由调用方保证，不能并发修改或在callback中绕过服务变更FS；本阶段不提供事务快照或强制全库锁。

### 公共服务与接口责任

新增独立`os/note_relations.h/.c`，借用调用方YanFs，复用纯格式`note_metadata_format`。不依赖shell、Host、tests或正文search；不通过嵌套调用公开meta管理接口实现关系。shell只解析参数、调用服务、格式化callback和summary，禁止在shell遍历FS或排序关系。

```text
yan_relation_init(context, fs)
yan_relation_links(context, name, sink, sink_context, optional_summary)
yan_relation_backlinks(context, name, sink, sink_context, optional_summary)
yan_relation_related(context, name, sink, sink_context, optional_summary)
yan_shell_bind_relations(shell, relation_context)
```

`YanRelationContext`独立持有borrowed FS、initialized/busy、头prefix、parser workspace、源info及最多63文件的有界关系/排序scratch。返回结果复用纯格式层既有`YanNoteResult`，不引入与其冲突的错误数值。统一同步row字段为`name[32]`、`exists`、`outgoing`、`incoming`、`shared_tags`：links只填写name/exists，其余为0；backlinks的name为来源、exists=true、incoming=true，其余为0；related填写候选name/exists=true及三个解释字段。summary字段为`total`、`shown`、`target_exists`；links total=shown<=16且target_exists=true，backlinks total=shown<=63并反映目标存在性，related total<=62、shown<=20且target_exists=true。只有OK发布summary。具体公开struct布局由工程header固定并测试，必须保持上述语义，不引入新磁盘格式。

sink必须有效；opaque `sink_context`原样传给同步callback，不转移对象所有权。callback返回false时服务停止，结果STOPPED且不发布成功summary。summary允许NULL，其他错误也保持有效summary原值；callback行及其名称仅本次同步回调期间借用，不得保留指针。callback可查询其他对象，但重入同一关系服务必须BUSY、零新增I/O。输出层将拒绝写字节映射为致命退出，不将其混作普通成功或补写OK。

对象须零初始化后init并长期静态持有。init可复用闲置对象，active对象BUSY；init失败不写对象，不进行文件I/O。沿0027健康FS初始化及状态映射；服务调用检查context完整uintptr_t跨度，先于读取bool，再initialized、BUSY、借用FS完整跨度/initialized/BUSY/FAULTED/MOUNTED，随后其他参数、别名与完整名称，再源存在性和头预检。未初始化INVALID、未挂载NOT_MOUNTED、已停用FAULTED；底层FS结果按既有token传播，不把METADATA当FS损坏状态。

输入、summary和可检测的callback holder跨度不得溢出uintptr_t或覆盖服务context/借用FS；名称与summary不得重叠。错误不改有效输出holder，BUSY优先且不写holder。名称逐字节读取时也必须检查边界，不先无界扫描或为规避别名拒绝合法邻接短名。底层真实错误不能被STOPPED/OK吞掉，busy在所有返回路径释放。

关系context自身上限64KiB，持有至多63个row及有界解析/排序scratch。独立关系对象及全部大scratch静态分配，不把约2KiB info/parser或4096prefix累积在4KiB任务栈。现有index、note、shell加关系对象的静态总预算仍不超过2MiB，Host/RV32实际sizeof、生产ELF和单帧栈测量后记录；单帧结果不冒称调用链/ISR嵌套峰值。每个现存头可顺序解析，不要求存整张图或全库正文。

shell绑定为可选扩充，旧unbound命令仍UNKNOWN_COMMAND、旧help字节保持；绑定后help可加三命令入口，生产应用静态持有并注入服务。不强改旧初始化签名。shell绑定对象需遵守现有FS同源、pairwise context跨度及BUSY约束。

### 阶段关系

[0027](0027-note-metadata.md)保留其元数据管理与标签搜索历史，本规格扩充关系导航；[0026](0026-term-search-index.md)排名和正文词项查询不变。[0021](0021-yanfs.md)单根目录及失败语义、[0022](0022-terminal-file-operations.md)交互/输出故障、[0023](0023-multiline-text-editor.md)编辑态、[0024](0024-file-rename-copy.md)原名字链接随文件内容携带的行为保持。

## IMPLE PLAN

1. 固定公共header、callback/summary及guard契约；native tests-first验证声明顺序、缺目标backlinks、缺源优先、双向/共享tag排序和top20/fulltotal，先记录语义RED。
2. 实现独立只读服务、全库预检后发布、静态scratch与source稳定借用；注入prefix/末文件I/O与协议错误、BUSY/别名/summary原子、零write。
3. shell绑定与命令解析/精确输出测试，旧unbound/help保持；生产应用注入，适配所有手工编译的传递输入，实际Guest重启及变更后重查。
4. 独立关系oracle，覆盖63文件、相同评分ASCII平局、晚坏头、binary正文、self/dangling与20之后候选；RV32 API/跨度、Guest故障与输出首中末。
5. 绑定指定suite/owner/具体断言的实际mutant与puregate，保留no-effect/wrong-owner/wrong-assertion及harness最高优先；仓库缺输入硬1、外部依赖77。冻结后完整Linux/ASan/Windows默认与全部actual、production OFF实物验收及预算/单帧测量；总审核文档与exact cached九项后交付。

## IMPLE

已实现`os/note_relations.h/.c`独立只读服务、shell可选绑定与生产应用静态注入。解析、排序及全库预检在关系服务内完成，shell仅解析参数和格式化同步结果。旧unbound命令/help保持，实际启动失败使用RELATION_INIT应用原因码22。公共header固定完整sink_context签名；补全依据见[STATUS](../STATUS.md)。

## VERIFY

| 面 | 必要输入与判据 |
| --- | --- |
| links | 声明顺序不同于ASCII、零声明、自引用、缺目标、坏目标头仍只按存在性OK、坏源METADATA无输出 |
| backlinks | 存在/不存在目标、零结果、ASCII来源顺序、自引用保留、晚坏头含非候选无部分输出 |
| related | 缺源优先NOT_FOUND、out/in各0/1、双向优先、tags次序、exact case/UTF8身份、ASCII平局、自身排除、全量total超过20 |
| 观察范围 | 无头与binary正文、巨大文件、bounded头、FEFF/unknownversion/坏marker沿0027；无正文preflight、全路径零write |
| 变更可见 | meta增删、replace、rename/copy/delete后下一查重算；原名断链不自动修复；source耗尽仍可观察 |
| API/失败 | Host/RV32 extent先bool、名字末端/邻接/别名、状态优先、NULLsummary、callback STOPPED与错误holder、重入BUSY、真实IO/协议sticky |
| shell/Guest | 三命令golden/零结果回执/USAGE零IO、unbound help旧字节、编辑态路由、全库预检后输出、TX首中末失败不再IO/OK |
| 门禁/工程 | 指定suite/owner/assert、实际三负控/harness优先、缺源/脚本/ELF硬1/工具77、生产OFF无tests依赖、static总<=2MiB/单帧边界 |

2026-10-14完整默认复测：Release90/90（92.36 s）、ASan/UBSan90/90（154.99 s）、Windows MSVC58/58（42.50 s）；统一十四组实际变异14/14（274.04 s），均0失败、0SKIP。核心38例、shell25例、耗尽1例、独立oracle237查询、Guest健康8项、RV32 API35个唯一检查码及实际tohost1通过。Guest故障23项为3健康基线、19故障注入与1真实生产ELF启动失败；末回执postcheck失败按精确已发送前缀验收，不禁止已发出的OK字节。

新增18个实际缺陷要求指定suite/owner/具体断言，默认puregate8项及三类实际负控通过；原始变异日志累计留存1556份，包含10月11日保留记录，不全部算作当日生成。生产OFF实际ELF健康8项通过，静态总预算Host/RV32为1619120/1619060字节；关系对象12144/12136字节，RV32关系单帧192字节，不代表完整调用链峰值。默认套件、统一变异与生产健康验收于2026-10-14复测；布局与单帧沿用10月11日测量，首轮执行结果另在 [STATUS-history](../STATUS-history.md) 保留。完整日志、失败修补记录和边界见[STATUS](../STATUS.md)。

本机验收与远端CI、维护者合入批准分别记录：功能提交be99b8e已建立PR #26并通过对应三项CI，TOOLS OFF覆盖范围见[STATUS](../STATUS.md)；项目所有者于2026-10-14明确批准PR #26合入main，合并提交8349b7d与已审交付树一致，阶段提交保留。规格批准历史保持，理解程度不由Agent代判。
