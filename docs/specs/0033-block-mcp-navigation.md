# 0033：Agent 的只读块导航

状态：整体方案已于2026-10-19获项目所有者批准，授权形成规格并实施。独立第2轮确认无阻塞性契约问题，机械澄清已由总审核整理。四个块工具已实现并完成本机验证，D修正及旧控制台测试判据修正已分别完成独立技术复核，不改变本规格接口。文档与总审核材料随交付审查提供；0033已随 [PR #38](https://github.com/YanBo-maker/YanOS/pull/38) 合入main（75e057a），原本机验证范围保持。当前测量与历史结果的范围见[STATUS](../STATUS.md)。基线为main 96167d7。

## INTENTION

Agent 搜索到 irq.txt 后，能列出具名块、取 irq.txt#irq-entry 的完整内容，并沿引用读取其它块，或查询谁引用了它。YanOS 返回原文和出处，调用方负责回答。现有 Guest 块服务与 Host 检索共用文件原文。

实际流程：yanos_search → yanos_block_list → yanos_block_read → yanos_block_refs / yanos_block_backlinks。搜索返回的来源身份可传给后续查询；若保存笔记后来源改变，后续查询返回 STALE，由调用方重新搜索，避免拼接不同版本证据。

## SPEC

> 后续扩展：[0036-R4/R6](0036-yanfs-multi-block-directory.md#0036-r4上层服务在大容量下行为正确) 将本节的最多 63 文件、slot<63 和计数容量约束扩展为镜像实际容量 N（最多 4095）。来源编码、分页、MCP schema 和其余校验保持；本节原文保留对应阶段范围，交付状态见 [STATUS](../STATUS.md)。

1. 为现有 Linux/WSL stdio MCP 增加四个工具，排在原两个工具之后：yanos_block_list、yanos_block_read、yanos_block_refs、yanos_block_backlinks。list/refs 参数为 name，read/backlinks 参数为 name 与 block_id；均可带 expected_source（64个小写十六进制字符，沿0030 source身份）。未知字段、坏名称/ID/身份为 INVALID，零来源I/O。原工具参数、结果和排名保持。
2. list返回声明顺序的全部块（最多64），包括ID、marker行、内容物理行及文件绝对半开字节范围。refs按源字节顺序、backlinks按ASCII文件名再位置，均前20处加全部次数；只有backlinks报告skipped非目标非文本文件数。目标状态区分存在、缺文件、缺块。行为和错误顺序沿0032；refs只校验显示的20个目标；backlinks允许缺目标。查询源或所校验目标为非文本时报INVALID_TEXT；backlinks扫描非目标非文本文件才跳过；坏头/坏块明确失败。
3. read用可选offset指定块内字节偏移，默认0；固定每页最多65536原文字节，末尾退到完整UTF-8字符与CRLF边界。offset须在范围内且不落在UTF-8字符或CRLF内部；offset等于块长度允许空终页。offset>0时必须携带expected_source。结果带完整块范围、当前页绝对范围/物理行、原文、complete、next_offset（完成为null）。空页first/last为0。调用方按next_offset读取至complete，可拼回完整块，不隐式截断。物理行包含元数据头；页可从一条长行中间开始。
4. 每个成功对象有schema_version=1、operation、source_sha256和该操作的typed结果。版本按工具/operation解释，块对象不是0030搜索对象。source_sha256与0030同一镜像的字段逐字节相同，可直接传入expected_source：ASCII名称排序，每项含目录槽、名称长度/字节、文件大小和全部原字节，整数沿原little-endian u32。包括二进制文件。来源枚举保留现有corpus_scan的最多63文件、slot<63、槽/名称唯一、名称合法及游标严格递增校验；无效枚举返回CORRUPT，I/O失败保留实际错误，不生成伪摘要。计算身份不解析不相关文件的头，也不生成语义分段，因此不引入8192段上限或无关坏头错误。
5. 调用前获取来源身份，expected_source不符报STALE；查询后重新观察目录和原文身份，变化报STALE。先完整收集本次有界结果，核对来源并成功关闭所有资源后，才发布成功。它是变化检测，仍要求Guest已退出且调用期间镜像稳定，不宣称并发快照或事务隔离。
6. 新工具只依赖启动时绑定的镜像，执行不打开索引、模型和runner，不启动embedding进程。现有四路径启动参数暂保持，缺模型文件不妨碍新工具读取块；路径由启动者配置，tool调用只接受YanFS名称/ID。保留1MiB协议消息、2MiB队列及单任务/取消/退出回收规则。64KiB原文字节即使双份JSON最坏转义也应纳入1MiB实测；编码超限明确LIMIT；分配/读取错误IO；关闭失败仅在尚无其他错误时改为IO，无部分成功对象。
7. 新块错误按稳定独立映射呈现，保留INVALID_TEXT、METADATA、BLOCK_FORMAT、AMBIGUOUS、LIMIT、NOT_FOUND及介质/协议错误区分；不得扩改旧semantic错误语义以偷接。合法第二工具调用仍BUSY。无结果列表成功，缺块读取NOT_FOUND，断链结果沿0032。取消不发布原请求响应。

### 接口契约

六工具顺序为search、index_status、block_list、block_read、block_refs、block_backlinks（均带yanos_前缀），无工具列表分页。serverInfo.version更新为0033；协议版本不变。0033扩展0031的两工具集合，0032“不增加MCP”保留为历史阶段范围。旧规格原文保留并标注后续扩展；不改写0032已完成阶段的历史结论。旧两工具参数、结果及排名不变。

所有成功对象的公共成员是schema_version（整数1）、operation（下面表中固定字符串）、source_sha256（64个小写十六进制字符）。structuredContent放该对象，TextContent.text放同一对象的紧凑JSON，isError=false。额外成员如下；输出对象不含未列出的字段。

| operation | 额外成员 |
|---|---|
| block_list | name, total, shown, blocks；blocks每项为id,marker_line,first_line,last_line,start,end；total=shown<=64 |
| block_read | name, block_id, block, page, text, complete, next_offset；block及page均为start,end,first_line,last_line；text为原文字符串，complete为布尔，next_offset为整数或null |
| block_refs | name, total, shown, refs；每项为source,block_id,line,start,end,target,state；target为name,id；block_id为来源块ID或null |
| block_backlinks | name, block_id, total, shown, skipped, target_state, backlinks；每项为source,block_id,line,start,end；行内block_id为来源块ID或null |

name/source/id/block_id为沿0032语法的字符串；位置与行号为非负整数，文件位置是绝对半开字节范围，非空内容行从1计，空范围first_line/last_line均0。total是全部出现次数（list为块数），shown是数组长度，引用类shown=min(total,20)。skipped为跳过文件数。计数用JSON整数，受现有63文件及u32文件长度约束，小于2^53；内部total仍用u64。state/target_state固定为OK、DANGLING_FILE、DANGLING_BLOCK。refs/backlinks的line/start/end描述引用本身，不是目标正文。

分页计算中offset始终相对块首；令B=block.start、E=block.end、L=E-B。offset须为JSON整数0..UINT32_MAX，拒绝布尔、浮点、负数；预期来源校验通过并定位目标块后，超出L返回INVALID；来源不匹配时按下述优先级先返回STALE。P=B+offset用u64计算，page.start=P；候选Q=min(P+65536,E)。若Q<E且byte[Q]为UTF-8续字节，回退到字符首；若Q<E且byte[Q-1]为CR、byte[Q]为LF，令Q=Q-1，使page.end指向CR，本页不含CR；page.end=Q。P落在续字节或CR/LF之间同样INVALID。offset=L允许空终页。complete=(Q==E)，未完成时next_offset=Q-B，且必须大于offset；完成为null。

非空页first_line=block.first_line+区间[B,P)中的LF数量；last_line=first_line+区间[P,Q-1)中的LF数量。末尾LF不产生虚构的下一行。原始字节经合法UTF-8转为JSON字符串，解码并按UTF-8编码后逐页拼接应等于块原字节（含CRLF）。示例：B=100、E=70100、offset=0且无边界回退时，page=[100,65636)，next_offset=65536；第二页从65636开始，不能误把offset当绝对文件位置。

CallToolRequest沿0031：缺省arguments对块工具视为缺必需参数，返回tool INVALID；arguments显式null或非对象返回协议-32602；对象内未知字段返回tool INVALID。工具描述提供完整inputSchema/outputSchema及既有只读annotations，不引入网络引用。

错误优先级：先校验请求格式/参数（未知字段、非法名称/ID、含NUL或错误格式的expected_source、非零offset缺expected_source均INVALID且零来源I/O）；现有协议错误和忙碌规则保持。随后打开/挂载镜像并计算前身份，失败返回实际错误；合法expected_source不匹配即STALE，不进入块查询。块查询失败保留原错误，不再计算后身份；成功则重新读取目录与原文计算后身份，读失败报实际错误，身份不同报STALE。缺expected_source仍做前后观察。所有路径清理资源；关闭失败仅把尚未失败的结果改为IO，已有错误不被覆盖。成功包身份为前后相同值。offset对实际块范围/字符边界的校验发生在0032源文件预检和目标块定位之后。无自动重试。

0033为四个块工具扩展0031的tool错误token集合，原两工具的错误行为不变。块服务错误按自身token映射（包括STOPPED、NOT_MOUNTED、FAULTED、PROTOCOL、CORRUPT、UNSUPPORTED），不能把其枚举数值转成semantic枚举。适配层主动停止时记录真实原因：容量超限LIMIT、取消则抑制响应；非预期STOPPED保留STOPPED。取满一页不是停止理由：仍消费完整show流，以保留后续I/O失败检测。内部worker结果增加明确错误域或等效显式映射，并验证合法值。

取消使用现有父进程控制，Host只读适配层在读操作之间检查取消，不改0032公共接口。取消后不发布请求响应，槽在worker回收后释放；5秒是SIGTERM未退出时升级SIGKILL的间隔，不保证不可中断I/O下的总回收时限。协议输出失败沿现有退出规则；新块结果在入队前测量包含双份JSON的完整响应，超过1MiB返回LIMIT，分配失败返回IO（仅在仍能构造错误响应时；无法输出则沿现有失败退出，不伪成功）。

启动仍要求image/index/model/runner四个非空选项；新增块分支在semantic打开及llama初始化之前只打开镜像，不stat/open/hash另外三个路径。它们指向不存在文件也可执行块工具；旧search/status依赖规则不变。当前可选构建依赖保持，不新增block-only构建目标。

## 方案取舍

Host复用os/note_blocks C服务和只读YanFS适配，避免在MCP层重写解析器。新工具不调用semantic_corpus_scan：该函数含分段上限及全库头解析，会改变0032错误范围；提取或添加共享的纯逻辑来源摘要能力，并用同一镜像验证其摘要与现有搜索完全一致。共享适配改动以当前需要为边界，不建立通用插件框架。

分页面向大块和MCP有界消息，代价是每次读取重新预检与来源核验。首版无跨请求缓存，避免多一套失效状态；需在验收报告记录实际I/O和耗时。现有refs/backlinks仍前20加总数，不在本阶段另加引用分页。

## IMPLE PLAN / VERIFY

| 顺序 | 实现范围 | 必须提供的证据 |
|---|---|---|
| A | host/semantic_posix只读镜像入口；semantic_corpus共享纯来源摘要；保留旧语义扫描行为 | 相同镜像摘要兼容、二进制和坏头不影响纯摘要、缺模型仍可挂载、关闭错误 |
| B | 新Host块适配与分页模块，复用os/note_blocks；对应原生测试和独立字节oracle | UTF-8/CRLF边界、绝对范围与相对offset、逐页拼接、晚I/O失败、总数和错误范围 |
| C | tools/mcp_protocol、mcp_worker、yan_mcp及CMake链接；生产与故障测试目标都接入yan_blocks | 六工具schema、独立错误域、原工具兼容、BUSY/取消/退出、实际消息长度和零写盘 |
| D | tests/test_mcp_protocol.c、mcp_checks.py、mcp_sdk_check.py、故障与变异检查，以及Guest/Host同镜像验收 | 真实SDK调用四工具、前后来源变动、无模型场景、实际缺陷检出和负控、完整回归 |

A完成后B/C可以按文件所有权拆分；共同接口先确定，修改共享文件串行集成。独立审查读取当前实现和本次测试证据，不能以实现者自查替代。新增公共C接口的生命周期、输出所有权和失败保持规则随对应模块声明，复用现有接口时保持其契约。

编码任务按依赖顺序交给同一个dsh实现会话：A只读镜像/共享来源摘要与兼容测试；B块查询及分页C服务/独立字节oracle；C MCP schema/worker接入与错误、取消、资源清理；D实际SDK与Guest/Host同镜像一致性、故障和变异门禁。每次交付解释输入、状态变化、对应条款、验证命令和局限。

重点：UTF-8及CRLF页边界、空块/EOF/大于64KiB长行、分页串接逐字节等于Guest原文、64/65与重复ID、二进制与无关坏头、显示20/总次数、来源变动含目录与二进制改动、旧source摘要兼容、无模型仍可块查询、I/O/close/输出失败与取消回收、实际镜像零写、旧工具完整回归。真实SDK发现和执行新四工具，不以schema静态相符冒充互操作通过。
