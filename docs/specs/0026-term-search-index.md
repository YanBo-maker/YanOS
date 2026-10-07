# 0026：词项搜索与可重建索引

> 状态：**Spec Lock**（2026-10-07 10:10 +08:00）。项目所有者已对整体方案明确“同意”，批准形成规格并继续实现。实现与本机验证已完成，独立实现审查已完成，用户实现审查待交付；理解程度由项目所有者判断。

## INTENTION

笔记保存以后，既能按字面内容定位，也能按词项查找并优先看到命中较多的行。本阶段一起完成 search、倒排索引、上下文摘要、确定排序、重建与索引失效管理。原始文件是事实来源，索引可以随时丢弃。

现有 grep 保持字节子串语义和完整行输出。新 search 采用下面的词项语义。索引、扫描两种执行路径提供相同的 search 结果；shell 选择搜索语义，通过搜索接口调用，不选择具体 backend。

## SPEC

### 命令与实际例子

新增四种操作：

```text
search interrupt
search interrupt PLIC
rebuild
index status
index clear
```

search 接受命令后的完整查询，可以用一对外层双引号包裹，例如 search "interrupt PLIC"。引号只包裹参数，不是短语操作符。没有内部引号、转义、选项或布尔运算符；裸查询首字符为 - 时拒绝。ASCII 标点、空格分隔查询组；仅折叠 ASCII 大小写后按组内容 canonical 去重，去重后的不同组最多 16 组。CPU 与 cpu 算同一组，重复 17 次 CPU/cpu 仍是合法的一组查询，不重复加分；17 个不同组返回 INVALID（CLI 为 ERROR USAGE），零文件 I/O。所有组须出现在同一逻辑行，顺序不限。空查询、纯分隔符、非法 UTF-8、超限或非法语法统一 ERROR USAGE，零文件 I/O。index 只接受 status/clear；rebuild 不接受参数。原读行器的控制字符与 1023 字节整行限制保持。独立 term API 接受 1–1023 字节合法 UTF-8 查询，允许 TAB 作分隔，拒绝 NUL、LF 和其余 ASCII 控制字节（含 DEL）；无有效组、词长或组数超限等不合法输入返回 INVALID，零 I/O。无前导空格时，CLI 裸查询最多 1016 字节，外引号查询最多 1014 字节。

ASCII 字母、数字、下划线组成完整词，最长 255 字节；interrupt 不匹配 interruption，PLIC 匹配 plic。非 ASCII 内容按合法 UTF-8 字符记录位置，不进行词典分词或 Unicode 规范化。一个组中混合的 ASCII 词和非 ASCII 字符必须依次首尾相接：

| 查询 | 能找到 | 找不到 |
| --- | --- | --- |
| search 中断 | 处理中断 | 中 foo 断 |
| search 中 断 | 中 foo 断、处理中断 | 只有中或只有断 |
| search CPU处理中断 | CPU处理中断、cpu处理中断 | CPU 正在处理中断 |
| search CPU 中断 | CPU 正在处理中断 | 只有 CPU 的行 |

ASCII 标点会分组；非 ASCII 标点保持为内容，例如 search 中，断 要求全角逗号也存在。其他语言暂按这个字符规则处理：café 是 caf 词与 é 字符相邻的一组，单查 caf 也能命中其 ASCII 部分。不做全 Unicode 大小写折叠、词干、模糊或语义扩展。

举例，以下文件内容说明锁定行为：

```text
irq.md:1:Interrupt controller handles 中断
irq.md:2:PLIC handles interrupt interrupt
trap.md:1:interruption pending
```

search interrupt 将先显示 irq.md 第 2 行，再显示第 1 行；trap.md 不命中。search interrupt PLIC 只显示 irq.md 第 2 行。短行的显示为：

```text
irq.md:2:PLIC handles interrupt interrupt
irq.md:1:Interrupt controller handles 中断
OK search total=2 shown=2 skipped=0 mode=index
```

### 文件、排序与摘要

仅检索整文件 UTF-8 合法且无 NUL 的文件。第一次发现非法编码或 NUL 就整文件跳过，skipped 加一，并停止读取该文件。此前已经发生的 I/O 错误仍须传播。文件扩展名不参与判断；grep 原有非 UTF-8 文件行为保持。

LF/CRLF 分行，终止符不参与匹配，无末换行的最后一行参与，不产生虚构空行。裸 CR 与其他允许存在于文件的 ASCII 控制字节是词项分隔符，显示时沿 cat 的安全转义规则。

每匹配行仅输出一次。每组的完整命中次数最多计 255，行分数为去重后各组计数之和，最高 4080。组内重叠匹配按不同起点计数：中中中 在 中中中中 中计两次；aa 对完整词 aaaa 不命中。按分数降序、目录物理槽升序、行号升序排序。必须遍历全部合格文件、统计全部匹配行，再确定前 20 条。total 是 uint64 行数，shown 最多 20；不能遇到第 21 条就结束扫描。

摘要以本行最早完整命中的起点为锚，向左取最近至多 40 个 Unicode 字符，连同右侧内容合计至多 160 个字符。只在完整字符边界截断，左/右截断侧分别显示 ...；不得跨行或包含 LF/CRLF 终止符。长查询本身可能显示不全，多组也不保证全部出现在同一摘要中。原内容不改写，非法控制字节按既有规则转义，无 ANSI 高亮。索引中的命中偏移足以从锚附近回读窗口：回读时遇到换行即停，不需要从文件头寻找行首。最多 640 字节是摘要内容上界，实际读取缓冲另外保留 UTF-8 对齐与换行探测余量。

### 索引保存在哪里

首版索引放在 Guest 静态内存，不占目录槽，不写磁盘，不增加块协议或数据库依赖。重启后索引为空，首次 search 自动从笔记全量重建；索引有效时通过词项位置表查找命中，回读入选结果的摘要。不要宣称未经测量的加速幅度。

索引侧明确登记两类 key：每个完整 ASCII 词（小写化）、每个非 ASCII Unicode 字符（原 UTF-8）。每次出现都记录物理目录槽、1-based 逻辑行号、字符/词起始文件字节偏移。中文多字查询用这些位置验证首尾相接，不能只求“几个字都在这一行”。完整 key 比对处理哈希碰撞。

固定容量：8192 个不同 key、256KiB key 字节池、65536 条出现记录。出现记录采用四个 uint32 字段 next/slot/line/file_byte_offset（16 字节）；key 表采用五个 uint32 字段（20 字节），开放寻址哈希表 16384 个 uint32 槽。四张固定表合计 1540096 字节。包括扫描、查询、top20、摘要与状态对象的总静态预算不超过 2MiB；实现时以 Host/RV32 sizeof、static_assert 与栈用量验证。没有按文件或行大小分配内存，也不保留所有匹配行数组。当前候选字段布局不表示完整 Guest 测量；具体对象总尺寸与栈用量须实测。

容量超限或源中出现超过 255 字节的 ASCII 词时，不发布部分索引。自动 search 改用完整流式词项扫描，返回相同的排序、总数、摘要和 skipped，并显示 mode=scan；不先打印 INDEX_LIMIT。长源词不可能匹配合法查询中的完整短词，扫描可跳过该词并继续处理文件，不计入 skipped。其他组仍须完整检查。

同一份源已经证明装不下时，后续 search 直接扫描，不反复重建；源修改后重新尝试。显式 rebuild 遇容量上限则输出 ERROR INDEX_LIMIT，属于普通错误，可以继续操作。

健康 MOUNTED 源因身份分配器耗尽而不可缓存时，显式 rebuild 返回成功并输出 OK rebuild，不执行磁盘 I/O，不发布 READY 或 LIMIT，保持 UNCACHEABLE，terms/postings 均为0。此情况不是索引容量超限；源未挂载或实际故障仍沿原错误规则处理。

### 修改与索引有效性

文件系统提供仅在内存中的源标识。成功创建、覆盖、删除、复制或不同名重命名，以及初始化、挂载、卸载和进入故障状态，使旧标识失效。同名零 I/O rename 与没有修改源的普通失败不变。纯格式编码 helper 不影响源标识。标识在进程内单调分配，不重置、不回绕，避免卸载后重新初始化或地址复用误认旧索引；耗尽时停止缓存，完整扫描仍可用。

这一检查在公开文件系统接口中实现，不能只在 shell 命令后设置 dirty。直接改结构体或绕过文件系统写原始镜像不在契约内。沿用现有“整个搜索期间源稳定”的调用方前提，不承诺新增并发事务。

```text
启动                       EMPTY
第一次 search              全量构建 → READY；查询并输出
再次 search，源未改变       使用 READY 索引
edit / w 成功              原标识失效 → STALE
下一次 search              丢弃旧索引，全量构建 → READY
index clear                EMPTY；原始笔记完全不变
索引装不下                 LIMIT；search 完整扫描
源标识耗尽                 UNCACHEABLE；search 完整扫描；健康源 rebuild 成功且零 I/O
```

重建开始先清空旧缓存并观察来源身份快照，全库只读结束后再次观察身份与状态；只有全部成功且来源仍相同、MOUNTED且可缓存，才能发布 READY。取消留下 EMPTY；容量失败留下 LIMIT；实际故障使源停用，旧索引不可再用。READY 或 LIMIT 的标识变化后状态为 STALE。

index status 不读磁盘，显示以下形态，计数仅表示完整有效索引：

```text
INDEX state=READY source=MOUNTED terms=42 postings=170
OK index
```

state 可为 EMPTY/READY/STALE/LIMIT/UNCACHEABLE；terms/postings字段始终存在，只有READY报告完整有效计数，其余均为0。源不可用时显示 UNAVAILABLE，source 显示 UNMOUNTED/FAULTED/UNINITIALIZED，不显示 READY。每种状态均输出 terms/postings 字段，只有 READY 报告有效计数，其余为 0。index status/clear 允许报告或丢弃不可用源的缓存，不恢复源。query 期间重入管理操作返回 BUSY。index clear 丢弃缓存，正常输出 OK index；不尝试恢复源。显式 rebuild 成功输出 OK rebuild；容量失败 ERROR INDEX_LIMIT；源不可用或实际故障沿既有错误映射。成功 search 在结果后输出上述 OK search 汇总，零结果仍有准确汇总。

实际 I/O、协议或输出失败不伪装成零结果、不输出 OK，不继续 I/O/输出；已显示的部分不回滚，继续沿现有 fatal 规则终止。搜索接口拒绝非法参数与可检测别名，回调/reader 同步借用、重入 BUSY、reader 错误 sticky 优先。旧 literal 接口和结构保留，新 term 入口与结果类型独立表达分数、摘要和汇总。

### 来源观察与token契约

文件系统新增内存字段 `uint64_t source_token` 与 `bool source_cacheable`。公开观察值 `YanFsSource` 包含 `YanFsState state`、`uint64_t token`、`bool cacheable`；`YanFsResult yan_fs_source(const YanFs *fs, YanFsSource *out)` 纯观察、不分配新token、不读取磁盘、不改变缓存或恢复源。

检查顺序为context与initialized、BUSY、输出holder范围/溢出/与context别名。未初始化返回INVALID，BUSY返回BUSY，错误不写out且无I/O。有效context的UNMOUNTED、MOUNTED、FAULTED均成功返回OK并报告实际状态；不能把FAULTED拒观测或自动改为MOUNTED。index管理层可在有效借用对象、已知合法内部holder条件下将未初始化映射为source=UNINITIALIZED/state=UNAVAILABLE，不能把一般INVALID参数错误吞成观察状态。term query/rebuild自行按既有来源错误规则判断能否访问文件。

进程全局uint64分配器初值0，正常身份从非零值单调分配；每次有效来源变更获得新身份。UINT64_MAX可作为最后有效身份，下一次分配返回0并设置cacheable=false，此后全部实例都不可缓存；不回绕、不重置，不把多个不可缓存token=0误当同源。不可缓存来源的search直接scan，不反复尝试构建，不发布READY或LIMIT身份缓存。进程重启清空索引与分配器，不承诺跨进程token身份。

成功init/mount/unmount、成功目录发布create/replace/remove/不同名rename/copy、实际进入FAULTED状态集中更新来源身份。普通无效输入、失败参数/容量/存在性检查、同名rename、只编码内存格式均不改变身份。mount因CORRUPT/UNSUPPORTED等普通拒挂不发布身份或状态；真实I/O/协议进入FAULTED必须更新。此扩充不改变[0021](0021-yanfs.md)既有状态优先级、[0024](0024-file-rename-copy.md)同名rename零I/O和失败不回滚契约。

生产没有reset分配器或设置token的测试API。耗尽边界通过单独测试构建的限定起始值或编译注入验证，生产默认仍0起始、单调uint64。

### 统一term接口与生命周期

[0025](0025-knowledge-search.md)的 `yan_search_query`、literal结构和grep契约保持。新增独立 `yan_search_terms` 语义入口、term backend vtable、term结果/汇总与管理接口；具体函数和对象标识符可工程细化。调用方选择literal或term语义，应用配置并注入backend，shell不创建索引或遍历文件系统。对象由调用者长期静态持有，大表和临时缓冲不进入4KiB任务栈。

term结果描述文件名、1-based行号、uint16分数、摘要原始字节范围、两侧截断标志及同步期raw reader；汇总包含uint64 total、shown、skipped与mode。先完成全库统计和top20选择再同步发布。读摘要只访问当前结果范围，不保留reader用于回调之后；范围/地址溢出、可检测对象别名与双输出重叠拒绝，嵌套读或管理/query/init重入返回BUSY，无新增I/O。读取错误sticky优先于回调STOPPED/OK，输出失败立即停止后续读与输出，沿既有fatal规则。

取消不伪称全量成功或输出OK search汇总；取消build留下EMPTY。状态观察和clear均为纯内存操作，可报告或丢弃不可用源缓存，但不能恢复FS；有效管理对象自身参数/别名/重入错误照常拒绝。

摘要回读保留UTF-8边界与CRLF探测余量，640字节只是160scalar内容上界，不是完整读取窗口尺寸。锚附近倒退最多40scalar并遇LF停止；不因缺行首字段而要求从文件头扫描，也不保存所有行数组。超出固定窗口的查询命中和其它组允许不全部显示，截断标记准确。

## IMPLE PLAN

在短期分支 `codex/knowledge-index` 实施，按逻辑变化提交：

1. `os/yanfs.h/.c`来源字段、纯观察getter、全局单调身份与`tests/test_yanfs.c`生命周期/错误优先/耗尽测试；先语义RED再实现。
2. `os/`新增term facade、共享UTF-8/行遍历、查询分组及完整流式term backend与原生测试；先独立oracle验证全部结果和错误，无索引限制。
3. 固定容量词典、出现位置表、原子全量构建、source失效管理及索引查询；测试索引/scan完整等价、容量和同token抑制重建。
4. 前20排序、摘要、safe display与`os/shell.*`管理命令；生产`apps/yanfs_terminal/main.c`注入，新API转移输入依赖同步适配，EDITING仍须w/q后使用shell命令。
5. `tests/guest/`真实健康/故障/RV32探针，原生和Guest实际变异、pure分类控制与CMake注册；BUILD_TESTING=OFF生产独立构建，最终全回归/注册/文档/cached审核。

每批明确输入、接口和验收条件，实施后对照规格复核；实现与独立审查分开记录。源码、测试、注册与对应文档同批交付。

## VERIFY

索引与扫描必须和独立参考判据对照，比较完整命中行集合、计分、前 20、摘要、总数与跳过数。生产路径共享行/UTF-8 原语，参考判据不能照抄生产 tokenizer。

重点验证中文相邻/打散、混合组、大小写去重、重叠、跨块 UTF-8、尾部非法编码/NUL、LF/CRLF/长行/无末换行；20/21 条与同分排序；容量精确边界、超长源词、LIMIT 不反复重建；所有公开写接口、普通失败、同名 rename、卸载重挂与地址复用、FAULTED、标识耗尽。

真实 Guest 验证保存后查询、清索引、重建、复制改名删除、进程重启与镜像字节不被搜索改写。默认回归、Windows、本机 sanitizer、生产无测试依赖构建与实际变异分别记录。变异只认指定 owner 的具体断言失败，构建失败、崩溃与超时不计检出。

## 设计参考

SQLite FTS5 把词项索引、位置、排序和摘要分别处理，并指出外部内容与索引失去一致性会导致异常查询结果。这支持本方案把原始笔记作为来源并显式核对有效性。[FTS5 外部内容](https://www.sqlite.org/fts5.html#external_content_table_pitfalls)

FTS5 trigram 的全文查询不能匹配少于三个 Unicode 字符的子串。因此本方案为“中断”这类短查询选择字符位置记录，而不是直接照搬 trigram。[FTS5 trigram](https://www.sqlite.org/fts5.html#the_trigram_tokenizer)

实现与本机验证已完成，Host/RV32结构尺寸、栈用量与回归按本轮树实测；没有性能普遍保证或完整嵌套栈证明，旧0025的通过记录不作为本能力证据。独立实现审核已完成，维护者实现审查仍待。


## VERIFY矩阵与批准记录

| 面 | 实际输入/状态 | 预期 |
| --- | --- | --- |
| 查询 | 17次CPU/cpu重复合法、17不同组INVALID、CPU处理中断、CPU 正在处理中断、空/非法UTF-8/TAB/长词 | canonical去重、组内相邻/组间AND；合法查询，非法零I/O |
| 词项 | interrupt/interruption、aa/aaaa、中中中/中中中中、中，断、café | 完整ASCII词，非ASCIIscalar相邻、重叠和局限符合例子 |
| 来源 | NUL/非法UTF-8首中末、跨块编码、裸CR/TAB/无末LF | 整file预检skip一次，已发生错误传播，行号/分隔正确 |
| 排名 | 0/20/21及大量结果、同分同slot、每组>255、重复组 | uint64 total、score上限4080、排序与top20准确 |
| 摘要 | 40/160scalar、4byteUTF-8、锚近行首/末、CRLF跨块、长match | 精确左右截断，安全显示、无跨行，固定内存 |
| 索引 | 8192keys/256KiB/65536posts精确上下界、hash碰撞、源长词 | 全量READY或LIMIT，不发布半表；scan不受表限 |
| 失效 | 每个成功公开mutation、普通失败、同名rename、init地址复用、fault/unmount | 正确身份变化/稳定，状态观察无I/O |
| token | UINT64_MAX最后身份、随后0不可缓存、不同实例、clear/status | 不wrap，无ABA，UNCACHEABLE直接scan |
| reader/fault | 别名/双输出/重入/过期、query/build/snippet错误与输出首中末 | BUSY/INVALID无额外I/O、sticky优先、不发OK |
| 工程 | sizeof/static_assert、Host/RV32、4KiB栈、生产OFF、镜像不变 | 完整静态预算≤2MiB、实际帧记录，不冒称嵌套峰值 |
| 检测力 | 独立oracle、indexed/scan与旧literal回归、真实缺陷归因控制 | 指定owner+断言，no-effect/wrong-owner/wrong-assert/异常优先 |

2026-10-07，项目所有者对上述整体方案明确“同意”，授权形成0026并继续实现。具体enum、struct、函数名与批次划分可在已批准行为内工程细化；实际字段布局、实现、验证和用户实现审查分别记录。本条不表示新能力已通过验收或合入。0025及更早阶段正文与历史结论保持。

### 批准补充验证：不可缓存源的显式重建

真实驱动全局身份耗尽后，对健康 MOUNTED 源执行 rebuild：精确 OK、磁盘读写增量0、状态 UNCACHEABLE、terms/postings为0，且不发布 READY/LIMIT。项目所有者于2026-10-07明确批准“返回 OK rebuild，index status 显示 UNCACHEABLE（推荐）”；该永久测试已实际构建执行通过；批准时待执行的历史与现测详见STATUS。

### 本机 VERIFY 记录（2026-10-07）

Linux注册默认67/开启实际变异79，Windows41；Release67/67（37.91 s）、ASan/UBSan67/67（75.33 s）、MSVC41/41（20.19 s），统一十二组实际变异12/12（203.40 s），均0失败、0SKIP。scan/index/index-repeat独立oracle各320例；真实Guest8健康、17故障、RV32 facade61唯一检查码通过。新term20缺陷及指定suite/owner/具体断言与三类实际负控已验证。布局、输入门禁、首次harness失败和覆盖限制见 [STATUS](../STATUS.md)。本机通过不表示远端CI完成或用户批准实现合入；独立实现审核已完成，远端CI待执行。
