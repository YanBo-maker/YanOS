# 0044：有原文依据的概念与命题图

- 意图：落实[决策0008](../decisions/0008-evidence-knowledge-graph.md)，让Agent读取已记录知识的概念、具体关系及原文范围；笔记内容按用户声明视为已理解，不做掌握评估。
- 状态：Approved
- supersedes：无；扩展0032正文解释，不改变原块、关系和MCP接口。
- 成功标准：显式声明能跨文件合并概念、保留有向多重命题和原文出处；精确查询不猜测未知，不写盘；独立dsh及总审核完成，新增行为有真实断言。

项目所有者2026-11-02授权设计、dsh编码和验收；具体设计由总审核在该授权内形成。发布与合入状态另记，不以本地验收代替CI。

## INTENTION

一个文件可包含多个概念，同一概念也可在多处解释。以全库显式概念ID聚合，以每个关系出现作为独立命题，以具名块作为完整上下文。只有明确记录的关系进入结果。概念名称出现不表示其所有性质都已记录；无结果只表示当前记录无依据。无掌握分数、模型调用、自动推理或前端。

## SPEC

### R1：正文声明与图核心

仅在0032合法具名块内容中识别列0的保留前缀`@kg`后接空格、TAB或行尾；其它正文和旧文件不变。不解释代码围栏或转义。头不解析为图。语法为：

```text
@block square-kind
@kg concept square 正方形
@kg concept rectangle 矩形
@kg alias square 等边矩形
@kg relation is_a square rectangle
正方形满足矩形定义，并且四条边等长。
```

命令词及每个固定字段之间恰好一个ASCII空格。concept/alias的最后字段是1..256字节UTF-8标签，内部可有普通空格，首尾不得是ASCII空格或TAB，不得含控制字符；保持大小写与原字节，不做Unicode规范化。ID及关系类型为1..31字节`[A-Za-z0-9_-]`，大小写精确。relation恰好三个字段，不接受尾空格。声明行接受LF/CRLF/末行无换行；无块的保留行、未知命令和非法声明返回GRAPH_FORMAT。

全文件先按0032资格、头和块规则验证；非文本整文件跳过并统计；文本坏头或坏块报实际错误，不能发布部分图。普通长行无新上限；图声明行受字段上限约束。所有现存文件均预检，包括没有声明的文本；按文件名ASCII及文件字节位置确定顺序。

concept的ID为库内逻辑身份，不取决于声明文件名。重复相同ID和相同名称合并，保留每次概念声明的出处；同ID不同名称为CONFLICT。alias与concept可以在关系之后声明；解析完成才解析引用。alias或relation引用未声明概念为UNRESOLVED。相同别名可指向多个ID，查询须显式报告AMBIGUOUS，不能自动合并概念。重复相同ID/alias字节可去重，但不能丢失concept声明出处。

每个relation出现都是独立命题，包含type、subject/object概念ID、文件名、块ID、声明行号/半开字节范围、完整块范围及首末物理行号。自环、环、相同端点不同类型及完全相同的重复关系均保留。原文块包含声明本身，描述和条件以原文为准；不把整块自由文本自动拆成额外命题。

最多8192个概念、16384条概念声明出处、16384个不同(ID,alias)、16384条命题。超过任何上限为LIMIT，不截断伪成功。内存分配失败为NOSPACE。提供独立Host C17核心，动态内存由graph拥有并明确释放；名称/ID精确索引和正反向邻接索引避免每次查询扫描整个图。构建失败不发布候选图，已有成功图保持；不落盘缓存。

### R2：只读镜像与Agent JSON入口

Linux/WSL工具`yan_graph --disk-image IMAGE list`、`yan_graph --disk-image IMAGE query TEXT`。一次打开稳定离线镜像并构建图，不依赖embedding模型、runner、语义索引或额外第三方JSON库。复用已有只读YanFS适配及源摘要，不改变盘上格式、Guest或现有MCP工具集合。C核心通过可注入的来源接口支持原生测试；Linux适配与核心分离。

TEXT沿0029的单行查询资格，另外限制为1..256字节：严格UTF-8，无LF/CR及禁止控制字符，不能全为Unicode空白；TAB沿原查询资格处理。精确ID优先，否则精确名称或别名。命中多个ID返回AMBIGUOUS；无命中返回NOT_FOUND，均不宣称用户不懂。list概念按ID ASCII排序，返回前20项及total/shown。query返回选中概念和别名、该概念声明的前20处出处及总数、全部直接入边/出边的前20条命题及总数。命题按文件名ASCII及声明byte排序，自环只列一次，附方向self/out/in；不展开传递关系，不添加隐含边。每条命题含端点ID/名称、类型和完整出处，后续可用既有块工具读全文。

成功stdout恰好一个UTF-8 JSON对象加LF，含`version:1`、`operation`、`source_sha256`、`skipped`及该操作结果；query另含`query`。字符串完整JSON转义，不混入人读OK/MATCH；score、mastery或unknown状态字段不存在。发布前失败exit1、stdout为空、stderr输出错误token；参数错误exit2、零镜像I/O。输出故障可能留下已发前缀，调用方必须同时检查退出码与完整JSON。显式列出的JSON字段和C类型以实施前头文件/工具测试固定并在使用说明举例。

构建前后重新观察逻辑源摘要，相异返回STALE，不发布JSON；这是变化检测，输入稳定仍是调用者责任。关闭资源成功后再输出结果，真实输出失败不得报成功。镜像与附件零写入；文件新增/修改/删除或改名后下一次调用重建，旧文件名块地址沿0032规则失效；概念ID可以由其它声明继续支持。来源身份借用0033摘要定义。

### R2接口字段锁定

公共C接口见`host/knowledge_graph.h`：`yan_kg_build/destroy/lookup`管理和查询图，`yan_kg_concept_at/relation_at`及count访问完整只读视图，`yan_kg_summary`返回上次成功构建计数。graph首次使用前零初始化，借用视图在成功重建或destroy前有效，失败重建不使旧视图失效。summary.files包含全部访问的逻辑文件，skipped为其中非文本文件数。lookup的candidate_count统计不同概念，不能把3项以上歧义恒报为2。

list的概念行含id、name、alias_count、declaration_count、relation_count；根字段为version、operation、source_sha256、skipped、total、shown、concepts。query的根字段为version、operation、query、source_sha256、skipped、concept、declarations、relations；concept含id、name和完整aliases字符串数组。declarations与relations分别为`{total,shown,items}`。

出处对象固定字段为file、block_id、declaration_line、declaration_start、declaration_end、block_marker_line、block_first_line、block_last_line、block_start、block_end；声明范围不含CR/LF，块范围沿0032。命题行含type、subject、object、direction和where；subject/object均为`{id,name}`，where为上述出处对象。版本号1的这些字段按真实CLI JSON测试固定。

错误诊断第一行为`ERROR TOKEN`加LF。USAGE退出2，可在错误行后附用法，便于人工修正调用；其它普通错误为该单行，退出1。坏头为METADATA，坏块分别为BLOCK_FORMAT/BLOCK_AMBIGUOUS/BLOCK_LIMIT，与概念查询AMBIGUOUS和图容量LIMIT区分；图语法、冲突、未定义引用分别为GRAPH_FORMAT/CONFLICT/UNRESOLVED。实际I/O、关闭和输出错误为IO；其它来源错误按C枚举映射，不数字强转。输出前应完成全部普通错误检查，输出失败仍可能留下前缀。

## IMPLE PLAN

1. dsh在独立`host/knowledge_graph.h/.c`实现R1和可注入来源构建，复用0032/0027解析器，新增独立语义测试；保持os/Guest不变。
2. 独立`tools/yan_graph.c`实现R2及JSON编码，CMake仅注册目标和默认测试；新源码不散入旧MCP/搜索模块。相关适配改动保持小范围。
3. 用真实YanFS镜像验证CLI、原文byte/line、跨文件/别名/多重边/错误/零写；原生核心在Windows及Linux编译，Linux定向ASan。测量大容量图查询，不宣称沿用Guest计步门槛。
4. 独立只读dsh最多两轮、原会话最多一次传输重试；总审核核对测试是否真正断言AC、文档四动作和历史保持。代码与文档分开提交；发布/远端CI未发生前明确待验。

## VERIFY 执行记录

2026-11-02，R1/R2已由同一个dsh编码会话实施。源码与当前差异在`build/20261102-knowledge-graph/`保存验证材料；独立只读dsh与总审核尚待收口，远端CI、发布与主线合入均未发生。以下只记录本机实际范围，不视为已合入能力。

| 命令/范围 | 实际结果与依据 |
| --- | --- |
| `python build/20261102-knowledge-graph/root-validation.py release` | Linux Release：197项注册，排除17个实际变异项后180/180通过，332.48s；源码在整轮内不变。`root-release-execution.json`、`root-release-ctest.log`、`root-release-LastTest.log`。 |
| Windows首次构建 | MSVC将新增长行测试夹具的两处strcpy/strcat诊断为C4996，构建失败，未当通过；原始`root-windows-build.log`保留。dsh仅将夹具构造改为已知长度memcpy，保留原字节和全部断言，生产源码与CMake均未因此改变。 |
| `python build/20261102-knowledge-graph/root-validation.py windows windows-final` | 修后Windows Debug完整91/91通过，97.98s；`root-windows-final-execution.json`、`root-windows-final-ctest.log`、`root-windows-final-LastTest.log`。 |
| `ctest --test-dir build/20261102-knowledge-graph/root-release --no-tests=error -R knowledge_graph --output-on-failure` | 修后Linux Release图核心与CLI两套2/2通过，0.45s；`root-release-final-graph-build.log`、`root-release-final-graph-ctest.log`、`root-release-final-graph-LastTest.log`。 |
| `python build/20261102-knowledge-graph/root-validation.py asan-targeted asan-final` | 修后ASan/UBSan定向2/2通过，1.39s；核心内含39个原生用例，CLI内含15组真实镜像检查。`root-asan-final-execution.json`、`root-asan-final-ctest.log`、`root-asan-final-LastTest.log`。不是完整ASan重跑。 |
| `python build/20261102-knowledge-graph/root-validation.py mutation` | 当前树的既有17组实际变异17/17通过，695.72s，运行期间源码保持；原始输出见`root-mutation-execution.json`、`root-mutation-ctest.log`、`root-mutation-LastTest.log`。这些是原有模块的既定组，不宣称覆盖新图；已在完整默认套件通过的控制台门禁未重复执行。 |
| `python3 build/20261102-knowledge-graph/root-cli-oracle.py build/20261102-knowledge-graph/root-release/yan_graph root-cli-final` | 总审独立26次CLI调用及额外真实/dev/full输出故障通过。独立编码v1/v2镜像、计算逻辑来源摘要、解析JSON并核对原字节/物理行、重复关系/自环、中文/引号/反斜线、前导连字符ID、错误与零写入；`root-cli-final/receipt.json`及逐次stdout/stderr。 |
| 4095文件样本 | 独立预检的list/query分别2.17/2.22s；最终复验与实际变异并发时为4.04/4.10s，均含整库构建及两次来源观察。记录分别在`root-cli-preflight-2/receipt.json`和`root-cli-final/receipt.json`，不宣称一般性能或套用Guest步数门槛。 |
| `python3 build/20261102-knowledge-graph/root-graph-defect-control.py root-graph-defect-control-final` | 隔离副本将歧义查询改为OK；基线39例全绿，缺陷副本在指定`test_shared_label_is_ambiguous`断言报Expected 13 Was 0，生产源码前后SHA保持。`root-graph-defect-control-final/receipt.json`及完整日志；不把它算作既有17组之一。 |
| 干净生产/默认配置 | `root-production-off-configure.log`、`root-production-off-build.log`、`root-production-off-targets.log`：BUILD_TESTING=OFF能构建yan_graph且无测试探针目标。`root-clean-config-final.log`、`root-clean-registration-final.json`：不显式设置BUILD_TESTING的首次配置同时注册核心与CLI，TOOLS开启/provider关闭配置共150项。 |

完整Linux回归结束后才修改Windows夹具构造；此后的图两套定向复验与Windows完整回归对应修后文件，不将先前180项冒称修后完整重跑。编码者早期失败、ASan暴露的问题、独立三概念歧义及前导连字符ID失败证据保留在阶段目录，没有当成最终通过结果。

### 2026-11-03：正式R1修补与冻结

独立只读dsh正式R1报告一项测试阻塞B1和两项文档阻塞D1/D2，未报告图实现的正确性或安全缺陷。B1要求实际断言NOSPACE及旧图保持；dsh新增Linux专用的分配故障测试，真实链接核心，对本夹具一次重建的26个malloc/calloc位置逐次失败。总审核另发现原出处断言比较同一内存，交原编码会话改为按值保存独立快照。生产源码保持，不加入故障开关。D1将STATUS改为分别记录已完成本地回归与待收口审核；D2补明JSON命令仅Linux/WSL、Windows仅图核心；旧当前段落原文归档，history换行统一LF。

| 修后命令/范围 | 实际结果与依据 |
| --- | --- |
| `python build/20261102-knowledge-graph/root-r1fix-validation.py release` | 配置、构建及定向3/3通过，0.35s；当前配置198项注册。`root-r1fix-release-execution.json`、同前缀ctest.log及LastTest.log。不是修后全量回归。 |
| `python build/20261102-knowledge-graph/root-r1fix-validation.py asan` | ASan/UBSan图核心、分配失败与真实CLI定向3/3通过，1.39s；本次配置180项注册。`root-r1fix-asan-execution.json`及同前缀日志，未宣称完整ASan。 |
| `python build/20261102-knowledge-graph/root-r1fix-validation.py windows` | Windows重新配置/构建及图核心定向1/1通过，0.54s；当前仍91项注册。`root-r1fix-windows-execution.json`及同前缀日志；此前91项全量结果对应前一个源码冻结，不混称本次全量。 |
| `python3 build/20261102-knowledge-graph/root-alloc-defect-control.py` | 隔离副本将NOSPACE改为LIMIT，基线1例通过，指定分配测试在第1/26个位置明确断言失败；编译/崩溃不计为检出。`root-alloc-defect-control/receipt.json`及baseline/defect日志。 |
| `python3 build/20261102-knowledge-graph/root-alloc-preservation-control.py` | 隔离副本在分配失败时改写旧图出处，指定第253行快照断言报告Memory Mismatch，Expected 0x03 Was 0x04。`root-alloc-defect-control/preservation-receipt.json`及完整日志；两项控制均未改生产源码。 |

修后冻结`final-r1fix-source-freeze.json`包含398项源码/配置输入，三个平台范围对应相同身份；相对前次冻结仅新增分配测试和CMake注册，生产代码逐字节保持。没有重跑先前完整Linux、完整Windows或既有17组变异，以上定向证据补充而不替代历史执行。编码者曾临时改生产文件做控制后恢复，SHA与原冻结一致；总审核不以其摘要作为独立控制证据，正式控制均在隔离副本执行。

2026-11-03，原独立只读dsh完成正式R2，B1/D1/D2均已核对通过；两轮正式审核结束，总审核九项本地检查通过。报告与最终身份见阶段目录reviewer-r1-report.md、reviewer-r2-report.md及review-r2-manifest.json。本地实施与验收完成，项目所有者意图验收及主线交付仍待实际合入。本阶段没有远端CI、PR或main合入；规格/索引仍Approved，发布拆分尚待安排，当前整组规模未视为满足暂定PR约400行预算。已交付能力行及MONITOR阶段归档待实际主线交付。本阶段不新增生成模型、MCP工具或前端，也未开展这些范围的效果验收。
