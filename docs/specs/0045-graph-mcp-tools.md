# 0045：Agent的只读知识图工具

- 意图：落实[决策0009](../decisions/0009-graph-agent-access.md)，让现有Agent经MCP查询显式概念和命题，再以同次来源身份读取原文；不评估用户掌握程度。
- 状态：Approved
- supersedes：无；扩展0031/0035可选工具集合，复用0044图与JSON语义。
- 成功标准：真实stdio MCP及官方SDK发现、执行两工具，返回与0044同一图/原文；默认列表兼容，来源变化明确失败，无模型读图、取消和资源回收符合现有服务规则。

项目所有者2026-11-03要求按已提出的发布、MCP接入及真实使用方向继续；总审核在既有自主设计授权内形成规格，发布/CI/主线交付分开记录。

## INTENTION

Agent查询“正方形”后取得显式is_a关系及出处，用source_sha256作为yanos_block_read的expected_source读取完整块。普通正文仍可用旧搜索工具查找；查不到显式图不代表用户不理解。前端、自动抽取、写图专用接口、推理模型与新掌握状态不在范围。

## SPEC

### R1：共享查询与只读结果

将0044 CLI的图查询生命周期与JSON编码提取为独立共享模块，由yan_graph及MCP直接调用。保留0044语法、限制、字段、排序、错误token、CLI参数/输出和零写行为，不复制整套实现或以CLI子进程替代C调用。CLI仍不要求模型/CURL/yyjson依赖，Windows图核心保持可构建；新镜像查询模块和MCP为Linux/WSL。

共享服务提供明确的输出malloc所有权，失败输出NULL/0；可指定可选expected_source（32字节）、取消观察和结果大小上限。期望来源与初次逻辑摘要不符先报STALE；成功结果仍须重新挂载观察目录及原文摘要并成功关闭资源，才返回JSON。CLI不设新结果容量限制；MCP的payload超现有1MiB返回LIMIT；图核心及共享服务自身分配失败为NOSPACE，来源/摘要组件的失败沿0044原错误映射，不改变CLI分类。最终完整MCP响应包含双份JSON，仍按0033边界测量；超过1MiB报LIMIT而非伪成功截断。普通失败不得输出半份结果。

取消复用父进程任务控制，服务在来源枚举/读取间观察取消标志；不改0044核心数据格式。已取消请求不发布响应，worker及其资源按既有退出/升级回收规则处理，不增加总时限保证。服务不启动模型、runner或额外进程。

AC：0044既有核心/CLI测试继续通过；真实相同镜像下CLI与共享服务JSON语义一致；源期望不符、真实目录变化、关闭/输出错误保持正确；结果上限失败输出为空；取消不返回成功。用受影响用例证明提取保持，不为纯移动重跑全部既有变异。

### R2：两工具、兼容与Agent调用

新增启动开关--allow-graph，无值、重复为启动USAGE。未启用时旧tools/list逐字节保持且调用新名仍为未知工具；与--allow-write/--allow-meta可独立组合。启用后按graph_list、graph_query顺序追加两个工具；其前已有工具的顺序/内容保持。

- yanos_graph_list：arguments可缺省或空对象，可选expected_source为64个小写十六进制字符，无其它字段。返回0044 list完整对象（version=1、operation=list及原字段）。
- yanos_graph_query：arguments必须含query字符串，沿0044的1..256 UTF-8字节和单行资格；可选同上expected_source，无其它字段。返回0044 query完整对象（含完整别名、声明/命题前20项及total/shown）。

通过通用tools/call结构校验后，工具参数在任何来源I/O前完成校验；非法query、expected_source和未知字段为工具INVALID。通用结构错误以及禁用/未知名沿现有0031协议错误，不改变其它工具的处理。不允许调用者提交路径、进程或模型参数。工具描述和outputSchema列明真实字段及只读属性，无记录不能翻译为用户不懂。成功text内容与structuredContent同一对象，既有版本协商/队列/单任务BUSY/取消/退出/失败输出语义保持。

启动仍沿现有四绑定路径；图分支只打开镜像，不stat/open/hash另外三项，其不存在不影响图查询。图错误使用独立worker域（4；保留域2为原非法控制），显式校验/映射0044枚举，不跨域数字强转。失败不得返回空数组冒充成功。来源身份与0030/0033一致，可直接传给块读取，保存/改名后旧身份返回STALE。

AC：默认及各开关组合的发现/调用正确，坏参数零I/O，图错误真实传播；原六工具、写/元数据工具回归保持；真实SDK调用两工具再读取出处块，原字节/行号一致；不存在模型/索引/runner也能成功，不启动额外进程；MCP真实镜像调用前后字节保持。取消/BUSY/退出用实际服务路径验收。Agent试用只验实际工具链与回答依据，不将一次试用当通用忠实性保证。

## IMPLE PLAN

1. 原dsh编码会话先提取共享图查询/编码模块，使CLI现有测试保持；小范围增补expected_source、上限与取消测试。
2. 独立tools/mcp_graph模块提供schema/适配，原protocol/worker/server只加必要开关、分支和显式错误域，CMake联动所有真实/测试worker目标。
3. 真实stdio、官方SDK及固定独立笔记试用；核心/CLI/旧MCP受影响回归、Windows核心与定向ASan。独立只读dsh最多两轮，总审核同步文档；不开展前端。

## VERIFY 执行记录

2026-11-03，dsh原编码会话已完成R1/R2及总审核发现的集成修补；独立只读审核与总审核最终验收尚待完成。本阶段尚无远端CI、PR或主线合入；0044已随PR193/195交付，不把那些CI计入本阶段。

编码者分阶段证据位于工作树build/0045-r1、0045-r2、0045-integration；总审独立证据在E:/YanOS/build/20261103-graph-mcp。R1提取保留CLI参数、JSON字段与排序，图核心源文件SHA保持63797ba0…；R1测试夹具曾错误借用setUp栈上工作区，dsh自查改为静态存储，原失败/早期通过日志保留，不作为修后充分证据。最终源码冻结与修后范围另补。

| 执行与范围 | 已有结果与局限 |
| --- | --- |
| 编码者R1提取、既有CLI逐字节对比 | 图相关4/4、对照旧CLI16例逐字节比较和BUILD_TESTING=OFF构建通过；R1早期新服务含7个用例，后补零I/O计数、STALE优先级及夹具修复，后续版本分开记录。原ASan4/4为该阶段范围，不冒称最终完整重跑。 |
| 编码者R2 stdio及SDK | 新图stdio17个功能场景组、官方mcp==2.3.0的list/query/block_read通过；两处LIMIT分别用实测733866字节对象（完整响应超1MiB）与1128667字节对象绑定父响应及服务上限。17是新功能场景数，不是仓库的17个实际变异组。 |
| 原MCP验证工具集成修补 | 总审发现旧run_mcp_mutation手动native链接漏新模块；dsh实跑得到baseline-block-worker未定义yan_mcp_graph_*符号，归harness failure，不当产品缺陷检出。补六项源/头和三库依赖及缺输入硬失败控制后，控制6/6、受影响MCP实际23个变异及基线/无效变异/错owner/错断言检查通过；不是全部17组。原始harness-failure-before-fix.log、mcp-mutation-group.log及每变体日志保留。 |
| 总审独立CLI原文oracle | root-cli-oracle.py对R1二进制执行26次真实镜像及额外输出故障，通过完整字段、字节/行号、来源身份、v1/v2、4095文件与零写入检查；root-shared-r1/receipt.json。不是后续最终二进制复验。 |
| 总审独立SDK预检 | root-sdk-oracle.py对R2冻结副本执行八种图/元数据/写开关组合，图到两份原文块、来源及错误、schema和零改写通过；root-sdk-preflight-2/receipt.json。首次脚本误用了SDK驼峰字段，实际2.3.0为snake_case；记录为验收脚本错误，修正后重跑，不记产品回归。 |

隔离控制的移除fallback导致exit5/ERROR_OUTPUT_OR_IO、改语义分支导致通用error_boundary失败，均不计指定owner的实际检出；仅默认六工具的具名断言控制获得支持。不得将构建/超时/传输失败记作变异杀死。新的服务编码分配失败断言、总审修后回归、真实dsh工具试用与正式审核结果待补齐。

新服务自身编码分配失败已由测试专用realloc注入绑定：实际触发失败、NOSPACE、NULL/0及不注入后的正常成功；query原生用例现为9例。源码与接口未改，仅该用例和PRIVATE链接开关追加，图四套复验通过。此后全量/定向最终验证待补，不冒称先前MCP23变异是追加测试后的重跑。

### 总审核修后独立验证（2026-11-03）

| 命令/范围 | 实际结果与证据 |
| --- | --- |
| `python E:/YanOS/build/20261103-graph-mcp/root-validation.py release` | 最终冻结Linux Release默认184/184通过，362.74s；配置注册201项，排除17个实际变异项，控制台门禁等默认控制仍执行。root-release-execution.json、root-release-ctest.log、root-release-LastTest.log。 |
| `python .../root-validation.py asan` | ASan/UBSan受影响图/MCP/语义/块/写服务及文档定向48/48通过，38.87s；配置注册183项，不冒称完整ASan。root-asan-execution.json及对应ctest/LastTest。 |
| `python .../root-validation.py windows` | Windows图核心、写文档检查与文档结构定向4/4通过，2.12s；配置注册91项，没有原生MCP/新POSIX服务，不冒称完整Windows。root-windows-execution.json及对应ctest/LastTest。 |
| `root-sdk-oracle.py root-release/yan_mcp root-sdk-final`（官方SDK Python） | 最终Release二进制的八种开关组合、精确Graph JSON/source、图到原文块、非法参数/STALE、错误后继续使用与镜像零改写通过；root-sdk-final/receipt.json和stdout日志。模型/索引/runner均不存在。 |
| `root-sdk-scale.py root-release/yan_mcp root-sdk-4095` | 固定4095文件镜像的MCP list/query分别2.70/2.38s，含整图构建与来源前后观察；当时与本机默认回归并发。root-sdk-4095/receipt.json；仅该样本实测，不作一般性能或Guest步数保证。 |
| `root-guest-trial.py` | 生产yan_mkfs新建固定试用镜像，真实RV32 Guest终端通过edit追加并保存geometry/scope两份笔记，正常exit。独立解析目录CRC/文件原字节与逻辑源摘要，Host图的关系/出处一致；root-agent-trial/receipt.json及终端输入、stdout/stderr。固定验收语料，不是用户真实笔记或掌握记录。 |

final-source-freeze.json逐项核对405项源码/配置输入，三种总审运行源码身份相同，图核心仍与0044一致。上述正式最终回归在追加新服务realloc失败用例后执行；此前MCP23个实际变异发生在该追加用例之前，生产代码未再变更，不混称本次重新执行23项或全部17组。

真实dsh只读工具试用、独立正式审核及九项总审仍待收口；本阶段无发布/远端CI/主线交付，不把0044的PR或CI冒充本阶段结果。

### 独立正式审核R1与真实Agent试用

原只读dsh会话实际调用yanos_graph_list、yanos_graph_query及两次yanos_block_read，source身份与原文出处一致，试用PASS；两个笔记中的is_a声明按两处独立命题展示。它准确说明对角线性质未记入笔记，不将此解释为用户不懂。固定语料的工具互操作验收不等于一般回答质量保证；report见reviewer-r1-report.md。

正式R1列出一个阻塞B1：测试注释声称省略arguments，但实际仍传空对象，未断言R2允许缺省的AC。生产实现已支持；原编码会话增补真正不带arguments的请求及完整对象等值断言，保留空对象检查并订正注释。总审独立对修后测试执行`ctest -R '^mcp_graph$' --output-on-failure`，1/1通过（最终具名断言版0.70s，root-b1-final-ctest.log；较早修后root-b1-ctest.log另存）。这是184项完整回归后的测试增补与定向后验，不冒称完整套件重新执行。

非阻塞N1为MCP把来源PROTOCOL并入IO的token细化建议，N2为补同镜像CLI/MCP直接字节比对的测试建议；发布PR后分别以follow-up Issue追踪，均不改变本阶段语义。R2仅复核B1及其文档记录，尚待结论。九项总审与本阶段远端CI仍待收口；Approved状态、无主线交付行和未归档本阶段MONITOR保持。

### R2与本地总审收口

正式独立只读dsh R2确认B1已解决、25项修前后输入仅测试与VERIFY变化、实际具名缺陷控制有效，代码与文档可提交；reviewer-r2-report.md。原编码会话仅补一个测试文件。它最初尝试写沙箱外CTest日志被拒，属于工具失败；改用工作树shadow注册执行同一E二进制后1/1通过，原始失败与补救日志均保留。总审另在原E构建注册上执行最终断言版，1/1通过，0.70s。无生产代码修改，无完整回归重跑声明。

chief-review.md九项本地总审通过：意图与R/AC追溯、模块边界、来源/错误/资源语义、兼容、证据身份及范围、变异判据、无默认跳过、文档/历史与Git/权限。独立审核共两轮，未自动新增第三轮；dsh编码及只读调用进程已结束，不重启归档实验。约400行生产diff预算的偏离将在PR说明披露：本组包含既有编码器搬移与独立MCP适配，保持内聚以避免重复；这不是明确豁免或规则修改。N1/N2待实际PR建立后开follow-up。

本地实施与验证、独立审核已完成；远端CI、PR及主线合入仍未发生，Approved状态保持。当前源码清单需在B1测试追加后另记，先前405项同一身份的三平台验证仅对应其原冻结输入，不把旧清单称为B1后的全部输入身份。

本地实现/测试提交：`471a4163fc3342d442f983dfbcec5010380714b3`；文档独立提交。精确提交的新远端CI待发布后核验。
