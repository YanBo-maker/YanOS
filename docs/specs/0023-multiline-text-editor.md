# 0023：多行文本编辑

> 状态：**Spec Lock**（2026-10-05 09:22 +08:00）。项目所有者已批准完整方案并授权形成规格、继续实现。整体实现与验证完成，交付与理解程度待用户审查。外部行为或不变量变更须重新走 INTENTION / SPEC / IMPLE 审核；公开enum、struct与确认token可在已批准行为内工程细化。

## INTENTION

终端保存和修改实际多行笔记。编辑期间只改变内存草稿，保存时一次创建或整文件覆盖；取消不写盘。复用 [YanFS](0021-yanfs.md)、[生产终端](0022-terminal-file-operations.md)、[任务运行时](0019-cooperative-runtime.md) 和 [持久化后端](0020-persistent-block-image.md)，不增加文件系统append、原地修改或事务恢复。

输入 `edit note.txt` 进入编辑，追加两行，替换第二行，查看草稿，再保存。重新启动后读回字节与保存稿一致；再次加载并取消，镜像不变。已有文件的LF、CRLF和无末换行状态不因载入被规范化。

## SPEC

### 模式与命令

应用模式为SHELL和EDITING；它们是应用路由状态，不改变任务的RUNNABLE/BLOCKED调度语义、UART独占owner或中断等待。SHELL保持0022原八命令，增加 `edit NAME`；help补充编辑入口。生产应用识别edit并路由独立编辑模块，原shell模块不增加编辑状态。

edit要求恰好一个合法名称，沿0021的1–31字节ASCII名称规则。存在则完整载入，不存在则新建空草稿；其它普通错误保持SHELL，实际FS或输出故障沿0022停止。载入成功才进入EDITING，不能把读失败前缀变为可保存草稿。

| 编辑命令 | 行为 |
| --- | --- |
| a TEXT | 追加以LF结束的新行 |
| r N TEXT | 替换第N行内容，保留该行原终止符 |
| d N | 删除第N行及其终止符 |
| p | 按1开始的行号显示草稿 |
| w | 一次创建或整文件覆盖，成功后退出编辑 |
| q | 丢弃草稿，不写盘，返回SHELL |

N是从1开始的十进制行号；非法数字、算术溢出、越界、arity错误或未知编辑命令报告普通错误，保留原草稿和模式。EDITING不把命令转交原shell执行；退出编辑用w或q。确认token与提示符可工程细化并由测试逐字节固定。

命令与TEXT沿0022单行输入：最多1023字节，超长或非法控制整行拒绝，不执行前缀；退格按byte，TEXT不解释转义、引号或变量。a在命令后、r在行号后消耗一个空格，其后TEXT保留额外空格与尾空格。空TEXT表示空行内容；不把编辑命令文本写入文件。

### 草稿容量与内存

最大草稿16384字节，包括所有行终止符；不是字符数。context由静态或调用者长期持有内存提供，不把16KiB稿放4KiB任务栈。借用FS与byte-output sink，禁止重入活动操作；可检测别名和地址溢出在修改前拒绝，不通过API修改调用者的FS缓存或输入行。

载入先取得文件大小，超过上限完整拒绝，不自动截断。完整读回并通过文本检查后才发布活动稿。读失败的部分前缀不可保存；新建空稿也不得创建磁盘文件，直到w。已存在标志在进入编辑时记录。

每次a/r/d先计算整个结果长度和所需移动范围，再改内存；容量或输入失败必须保留整个原草稿。无需第三份整稿作为临时工作区；小的UTF-8或行扫描状态不得演变为大任务栈分配。

### 文本与行终止符

只编辑合法UTF-8文本。允许LF、CRLF和已有TAB；CR必须紧邻LF，裸CR拒载。NUL、其它C0、DEL、U+0080–U+009F的C1控制码拒载。overlong、surrogate、超过U+10FFFF、孤立续字节和最终截断编码拒载；不丢弃、替换或规范化字节。二进制或不支持的文本仍可由0022 cat安全查看，不能载入后截断保存。

空文件没有行。LF或CRLF终止一行，行内容不含终止符；最后终止符后不凭空增加一行。非空无末换行的剩余字节形成最后一行。例如 `A\nB` 为两行，第二行无EOL；`\n` 为一空行，`A\n` 为一行。

- a总追加TEXT与LF。若非空旧稿末行无LF，先补一个分隔LF，再追加TEXT与LF；两LF和TEXT费用一起检查，失败不部分改稿。其余旧行EOL不变。
- r只替换目标行内容，原行LF、CRLF或无EOL保持。替换末行不隐式新增LF。
- d删除目标行及它的EOL；删除无EOL末行只删除内容。删光后为空文件。

TEXT本身不含键入的LF/TAB/C0；LF由编辑操作生成，已有TAB可载入并保留。输入UTF-8因byte退格变坏时，完整拒绝此次编辑，不修复编码或破坏原稿。

### 显示

p每行输出 `N TEXT\r\n`，行号为十进制，原EOL不裸发。TEXT沿0022的安全字节显示：反斜线双写，已有TAB为 `\x09`，合法UTF-8可读。编号、显示转义及CRLF不写入草稿。p只读内存、不做FS I/O，输出失败立即停止，不能因展示失败改稿或声称保存成功。不承诺完整Unicode宽度或光标编辑。

### 保存、取消与故障

进入edit时记录existing：已存在文件w只调用replace，新稿w只调用create，不因EXISTS或NOT_FOUND偷偷换操作。完整稿包括空文件，以一次整文件API保存，成功才返回SHELL；不逐行提交，不在a/r/d时写介质。

普通INVALID、BUSY、EXISTS、NOT_FOUND、DIRECTORY_FULL或NOSPACE等保存错误报告后保留整个草稿，仍可编辑或q。覆盖仍需同时容纳新旧extent，碎片和峰值空间沿0021，不借用旧extent原地覆盖。

q不调用写API，镜像不变；放弃的稿没有自动恢复。输出、实际IO/PROTOCOL、FAULTED或挂载故障沿0022致命规则停止。保存发布后输出失败可能已改盘；失败不能保证旧metadata或数据可读，不回滚、自动重试、重挂或修复。Host退出/close优先级、EOF不可见Guest、无deadline和空闲耗CPU保持0022。

不提供光标编辑、历史、剪贴板、append API、子目录、多窗口、自动保存或断电恢复。

### 公共接口与应用结果

工程接口位于 `os/editor.h`，使用C17与已有 `YanShellOutput` 字节sink：

```c
typedef enum {
    YAN_EDITOR_OK = 0,
    YAN_EDITOR_EXIT = 1,
    YAN_EDITOR_FATAL = 2,
    YAN_EDITOR_INVALID = 3,
    YAN_EDITOR_BUSY = 4
} YanEditorResult;

typedef struct {
    YanFs *fs;
    YanShellOutput output;
    bool initialized, busy, active, existing;
    uint32_t length;
    char name[YAN_FS_NAME_MAX + 1u];
    uint8_t draft[16384];
} YanEditor;

YanEditorResult yan_editor_init(YanEditor *, YanFs *, YanShellOutput);
YanEditorResult yan_editor_start(YanEditor *, const uint8_t *name,
                                 uint32_t name_length);
YanEditorResult yan_editor_execute(YanEditor *, const uint8_t *line,
                                   uint32_t length);
bool yan_editor_active(const YanEditor *);
```

调用者将context零初始化，借用的FS、sink与context持续有效；操作期间不修改其字段。init不做I/O或输出，地址重叠在读取bool前拒绝；空参数、空sink与可检测地址溢出返回INVALID。重入正在执行的实例返回BUSY，不输出、不增加FS调用；空闲实例可以重新初始化并丢弃当前内存会话。活动编辑会话不能再次start；未活动会话不能execute。

name与line是显式长度的可读byte range，不要求末尾NUL；名称仍须完整符合1–31字节规则，内嵌NUL不能变成合法前缀。输入与整个editor或借用FS可检测重叠、范围超过uintptr_t时返回INVALID，不输出、不做I/O。空编辑行允许NULL加零长度，只请求下一提示；输入有效期保持至调用返回。原输入不可被修改。

OK表示调用已处理，也可能输出普通ERROR；调用者通过active区分成功载入与普通载入错误。EXIT仅表示健康w或q返回SHELL；FATAL要求终止应用。INVALID和BUSY是API级拒绝，生产应用不将它们当普通命令继续。普通命令错误仍返回OK并保留活动稿。

提示符固定为 `edit> `；成功确认分别为 `OK edit\r\n`、`OK a\r\n`、`OK r\r\n`、`OK d\r\n`、`OK p\r\n`、`OK w\r\n`、`OK q\r\n`。普通错误为 `ERROR TOKEN\r\n`：USAGE、UNKNOWN_COMMAND、OUT_OF_RANGE、TOO_LARGE、INVALID_TEXT、LINE_TOO_LONG、INVALID_INPUT，FS结果沿0021枚举名称输出。p先输出编号正文，再确认；确认任意byte失败沿致命规则处理。

生产应用保留0022原因码1–17，新增18 EDITOR_INIT、19 EDITOR_FATAL、20 EDITOR_INVALID、21 EDITOR_BUSY，致命结果为 `0x71000000 | reason`。w/q只切回SHELL，不退出进程；健康shell exit仍按0022的unmount、close和最后byte检查成功后写tohost 1。

## 实际输入与状态

空镜像执行 `edit note.txt`，draft为空且磁盘无此文件；`a 第一行`、`a 第二行`产生两条LF行。`r 2 修订`替换第二行但保留LF。p只显示编号；w调用一次create，保存后新进程读回相同UTF-8字节。

已有 `A\r\nB` 载入不改字节；`r 1 C`得到 `C\r\nB`；`a D`得到 `C\r\nB\nD\n`。q则原文件仍是 `A\r\nB`；w才整文件replace。满容量a失败，原稿全部保留；普通NOSPACE保存失败仍保留稿，实际IO失败沿FS停用而不继续编辑。

## IMPLE PLAN

1. 新增生产编辑核心与公共接口（候选os/editor.h/.c）及native tests。固定长期context、16384容量、完整载入发布、UTF-8/EOL验证、行扫描、原子内存编辑、alias/BUSY和borrowed sink责任；enum/字段及精确错误token由工程细化。
2. 新增编辑命令dispatcher并由apps/yanfs_terminal/main.c路由SHELL/EDITING，help补入口。复用现终端读行，生产源不得依赖tests，原八命令保持。
3. 真实Guest整体脚本执行新建、载入、a/r/d/p、w/q、新进程读回；独立whole-image与EOL字节oracle，不复制生产编辑状态机。OFF生产构建再次验收，实际ELF核静态RAM与栈预算。
4. 注入载入失败、普通保存错误、FS/outputfatal；具名具体owner变异、puregate、noeffect/wrongowner、缺源硬1/外部77与异常优先控制。只认断言，不把timeout或sanitizer当检出。
5. 最终默认/ASan/实际变异、规格/实现与reader状态复核；总审核检查链接、计数、授权、历史、MONITOR和cached九项，代码与文档同批交付。

## VERIFY计划

空文件、空行、首/中/末行、LF/CRLF/混合EOL/无末LF、中文跨输入和读块边界、TAB保留、C0/DEL/C1拒载、非法及截断UTF-8、16384/16385、原子容量失败、行号溢出/范围、可检测alias与重入。p与q不改盘；载入失败前缀不能保存；w固定create/replace分支，普通失败稿byte-identical，致命失败不假成功；真实新进程读回与wholeimage比较。

变异至少覆盖丢LF/额外LF、CRLF规范化、超限截断、取消写盘、修改立即写盘、错create/replace、载入读前缀允许保存、容量失败部分移动和输出失败假成功，各绑定owner及具体断言。验收计划不表示已通过。

### 本轮执行结果（2026-10-05）

核心64项、生产Guest15项与故障33项通过，实际编辑器变异11项全部命中指定owner及具体断言，0存活、0harness error。puregate核同owner无关断言、marker只在非FAIL位置、完整Unity计数、duplicate与异常优先；实际no-effect与wrong-owner副本也正确拒绝检出。名称显式长度NUL边界已按原契约修复并独立验证，未修改名称规则。

整体Release默认47/47（28.88 s）、ASan/UBSan默认47/47（48.86 s）、十组实际变异10/10（130.76 s，均-j3），0失败、0SKIP。生产OFF配置构建且执行同15项Guest验收。注册、保存日志、RAM与编译器静态帧报告见 [STATUS](../STATUS.md)；静态帧报告不等于完整ISR嵌套栈证明。

故障验收包含加载/保存下一块请求IO失败、加载第一/第二块错tag、输出首/中/末byte和提交后回执失败；未单独注入保存第二次metadata写失败。该路径仍沿0021失败契约，未增加回滚、修复或断电承诺。

## 审批与阶段状态

项目所有者2026-10-05明确认可完整方案并授权形成规格继续实现，涵盖a/r/d/p/w/q、16KiB静态稿、合法UTF-8与已有TAB、保留LF/CRLF/无末LF、完整稿一次save、取消不写、普通错误留稿及fatal沿已有不回滚契约。总审核据此落笔；精确输入和EOL细化属于批准范围，不改0021/0022历史结论。

规格已锁定，公共核心、native测试、生产应用路由、真实Guest整体、故障与变异验收已完成；用户交付审查和理解程度待用户判断。read状态优先修复 `1daae9f` 为独立基线，其历史通过数不代替本轮编辑器验证。
