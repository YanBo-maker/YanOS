# YanOS

YanOS 是一个用于学习计算机系统的项目，包含 C17 编写的 RISC-V RV32IM 功能模拟器、Guest 上的操作系统与文件系统实验，以及终端笔记和本地知识检索工具。

开发从指令执行逐步推进到设备、异常、中断和持久化存储，保持 Host / Guest 与软硬件边界，每项能力以明确规格和可复现验证交付。

## 状态

YanCPU 已实现 RV32IM 整数指令、CSR、M-mode 异常与中断；平台包含 RAM、CLINT、PLIC、UART 和 Host 传输通道。`os/` 已有控制台驱动、块请求协议与协作式任务运行时。`yan_run --disk-image FILE` 接入已有块镜像，持久化后端已验证 Guest 写入与新进程读回；[0020](docs/specs/0020-persistent-block-image.md) 锁定成功前刷新、失败后停用、失败写不保证回滚的契约。

[YanFS](docs/specs/0021-yanfs.md) 已实现并验证格式化、挂载、列目录、读取、创建、整文件覆盖、删除和空间回收。初版采用单根目录、最多 63 文件和连续 extent；覆盖需要新旧内容同时容纳，碎片可能导致 NOSPACE。元数据写失败可能拒挂，不保证断电一致性，不自动修复。实现与验证已完成。

[Guest 终端文件操作](docs/specs/0022-terminal-file-operations.md) 已实现并验证：独立应用提供 help、ls、stat、cat、create、write、rm 和 exit，使用 UART 中断等待输入，接入已有 YanFS 镜像。create 创建新文件，write 整文件覆盖；单行最多 1023 字节，超长或非法控制字节整行拒绝。cat 保留合法 UTF-8，并转义指定控制字节和非法编码。实现与验证完成。

[多行文本编辑](docs/specs/0023-multiline-text-editor.md) 已实现并验证：edit加载或新建16KiB内存草稿，用行式命令追加、替换、删除和显示，一次保存或取消；保留已有换行格式。

上述能力已于2026-10-05经项目所有者批准，通过 [PR #16](https://github.com/YanBo-maker/YanOS/pull/16) 合入main，保留阶段提交。学习与理解程度仍由项目所有者判断。

[重命名与复制](docs/specs/0024-file-rename-copy.md) 已实现并完成本机验证，经项目所有者批准，通过 [PR #18](https://github.com/YanBo-maker/YanOS/pull/18) 合入main：mv保留原目录槽和数据块，cp新建独立副本，均拒绝覆盖已有目标。复制支持任意二进制文件，不受编辑草稿容量限制；文件系统接口扩充与终端两命令见0024。

[知识检索基础设施](docs/specs/0025-knowledge-search.md) 已实现并完成本机验证，经项目所有者于2026-10-07批准，通过 [PR #20](https://github.com/YanBo-maker/YanOS/pull/20) 合入main：统一Search API接入无索引线性backend，grep按字面字节子串返回完整匹配行，支持跨块与长行，NUL文件整文件跳过。原始笔记为事实来源，不修改磁盘格式或建立索引。

[词项搜索与可重建索引](docs/specs/0026-term-search-index.md)已实现并完成本机验证：search按词项组做同一行AND查询，返回评分最高的20行及上下文摘要；RAM倒排索引随文件变更失效，超出容量时完整扫描。原始笔记保持为事实来源，grep语义不变；功能提交77f03fd已通过 [PR #22](https://github.com/YanBo-maker/YanOS/pull/22) 的三项CI，项目所有者于2026-10-07明确批准，经PR #22合入main（c84b2e7）；学习理解由项目所有者判断。

[笔记元数据与标签筛选](docs/specs/0027-note-metadata.md)已实现并完成本机验证：同文件版本化头声明title、tag、日期与文件链接，meta提供观察、单文件更新和列表，search先按标签AND筛选再排名。不改变YanFS磁盘格式或既有无tag查询；功能提交180d18e已通过[PR #24](https://github.com/YanBo-maker/YanOS/pull/24)三项CI，项目所有者于2026-10-11明确批准，经PR #24合入main（7451c85），阶段提交保留；学习理解由项目所有者判断。

[知识关系导航](docs/specs/0028-knowledge-relations.md)已实现并完成本机验收：links按声明顺序观察向外链接，backlinks可反查不存在的目标名，related按链接方向与共同标签解释排序，显示前20项并统计全部候选。查询只读现有元数据头，不改变磁盘格式或正文搜索；功能提交be99b8e已通过[PR #26](https://github.com/YanBo-maker/YanOS/pull/26)的三项远端CI；项目所有者于2026-10-14明确批准，经PR #26合入main（8349b7d），阶段提交保留，学习理解由项目所有者判断。

[Host语义检索](docs/specs/0029-host-semantic-search.md)整体方案已批准并锁定，提供独立可选Linux/WSL工具，以本地模型为只读YanFS原文段落建立旁置向量索引，返回带物理出处的前20个候选。Guest检索与磁盘格式保持。已实现并完成本机完整回归、实际变异及固定小语料真实模型复验；独立只读dsh对齐与总审核已完成，功能提交9ad39ed已通过[PR #30](https://github.com/YanBo-maker/YanOS/pull/30)对应的三项远端CI；项目所有者于2026-10-15明确批准，经PR #30合入main（79fdde6），阶段提交保留，学习理解由项目所有者判断。

[可核验检索上下文包](docs/specs/0030-retrieval-context-json.md)整体方案已于2026-10-16批准，为Host search增加可选JSON输出，携带完整命中段、物理出处和同次来源/模型/runner/配置身份；默认文本输出保持。规格锁定提交3e84826已保留；evidence API与JSON层已实现，原生及JSON定向验证通过，旧门禁夹具复制范围问题已修复；Release/ASan/Windows完整复验及统一十五组实际变异通过，显式JSON真模型验收亦通过。独立最终对齐及九项总审核已通过，功能提交5299136已通过[PR #32](https://github.com/YanBo-maker/YanOS/pull/32)对应三项远端CI；项目所有者于2026-10-16明确批准，经PR #32合入main（5170cf0），阶段提交保留，学习理解仍由项目所有者判断。

[面向Agent的MCP检索入口](docs/specs/0031-mcp-retrieval-tools.md)已实现Linux/WSL只读服务，Agent可发现和调用`yanos_search`与`yanos_index_status`，复用0030完整原文与出处身份；回答由调用方生成。既有本机回归与检测力验证通过后发现继承信号状态问题，现已补齐初始化并重验官方SDK与固定语料；修后完整回归、扩充变异、原独立角色必要复核与精确树九项总审已通过。功能提交`4452e11`已建立[PR #34](https://github.com/YanBo-maker/YanOS/pull/34)，对应功能head的三项远端CI通过；项目所有者于2026-10-17明确批准，经PR #34按merge commit合入main（`a9a1e9c`），阶段历史保留；学习理解仍由项目所有者判断。

S/U 模式、分页与抢占式调度尚未实现。覆盖边界、实测与用户审查状态见 [STATUS](docs/STATUS.md)。

## 构建与测试

需要支持 C17 的编译器和 CMake 3.21 或更新版本。Windows 可使用 WSL 中的 GCC，也可以使用 Visual Studio 的 C 工具链。

在项目目录执行：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

`build` 只是构建目录的名字，可以换成任意路径；同一份工作树里并行开发时，各自用独立目录（例如 `build/<任务名>`）可以避免互相覆盖缓存——直接对一个没有缓存的目录运行 `cmake --build` 会报 `could not load cache`。

测试采用 Unity 2.6.1，由 CTest 运行。首次配置会下载固定版本并校验 SHA-256；仅构建核心库可添加 `-DBUILD_TESTING=OFF`。

GCC / Clang 的 Unix 构建可在配置时添加 `-DYAN_ENABLE_SANITIZERS=ON`，启用地址与未定义行为检测。

构建 Guest 验证工具可添加 `-DYAN_BUILD_TOOLS=ON`。`yan_run` 支持 RV32 ELF、最大步数、`tohost` 退出、签名区导出和逐条架构状态记录；`yan_difftest` 让 YanCPU 与外部参考模型逐条比较架构状态；`yan_gen` 生成随机 RV32IM 指令流。

Linux 工具配置另提供 `yan_mkfs`，排他创建新 YanFS 镜像，已有文件或链接会被拒绝：

```sh
cmake -S . -B build/yanfs -DYAN_BUILD_TOOLS=ON
cmake --build build/yanfs --parallel
build/yanfs/yan_mkfs --image new.img --blocks 16
build/yanfs/yan_run --image guest.elf --disk-image new.img
```

`guest.elf` 是调用文件系统接口的 Guest 程序；公开接口见 [os/yanfs.h](os/yanfs.h)，Guest 块适配见 [os/yanfs_block.h](os/yanfs_block.h)，可运行的整体例子见 [Guest 验收程序](tests/guest/yanfs_check.c) 与 [验收脚本](tests/guest/run_yanfs.py)。mkfs 与当前工具验证使用 Linux/WSL 路径；Guest 不自动格式化镜像。

Linux/WSL 中具备 RISC-V 交叉编译器时，可构建生产终端应用；`BUILD_TESTING=OFF` 也能构建此目标，不需要 Unity：

```sh
cmake -S . -B build/terminal -DYAN_BUILD_TOOLS=ON
cmake --build build/terminal --target yanfs_terminal yan_run yan_mkfs --parallel
build/terminal/yan_mkfs --image terminal.img --blocks 16
python3 tools/yan_shell.py --run build/terminal/yan_run \
  --guest build/terminal/guest-apps/yanfs_terminal.elf --disk-image terminal.img
```

已有镜像可直接传给启动器；上面的 mkfs 仅用于创建新镜像。输入 `create hello.txt hello`、`cat hello.txt`、`write hello.txt world`、`rm hello.txt`，最后用 `exit` 结束。TEXT 不隐式添加换行，退格按字节处理。启动器临时调整 Unix TTY 并恢复完整属性；stdin EOF 不传给 Guest，必须显式 exit。默认使用很大的有限步数上限，可通过 `--max-steps` 缩小；空闲仍消耗 CPU。文件发布后输出失败不回滚，不自动修复或重新挂载。

多行笔记使用同一应用和启动器：

```text
edit note.txt
a 第一行
a 第二行
r 2 修订
p
w
cat note.txt
mv note.txt diary.txt
cp diary.txt backup.txt
exit
```

编辑时提示符为 `edit> `。`a`追加LF行，`r N TEXT`替换第N行并保留原行终止符，`d N`删除一行；`p`只显示内存稿。`w`一次保存并返回shell，`q`取消且不写盘。草稿最多16384字节，命令行最多1023字节；容量或输入错误保留原稿。仅载入合法UTF-8文本，允许已有TAB和LF/CRLF，二进制仍用cat查看。公开接口见 [编辑核心](os/editor.h)。

回到shell后可用 `mv OLD NEW` 改名、`cp SRC DEST` 复制。mv源存在且新旧名称相同时成功而不写盘，cp同名报EXISTS；已有目标均不会被覆盖。复制需要新的连续空闲extent，目录满或碎片可能使其失败。编辑态须先w成功或q退出，才能执行两命令。

保存后可用 `grep 修订` 或 `grep "第二行 修订"` 查询字面字节子串。结果按目录槽和行号排列，以 `FILE:LINE:CONTENT` 显示完整匹配行，同一行只显示一次；零匹配仍输出 `OK grep`。LF/CRLF 分行，裸 CR 保留为内容。含 NUL 的文件整文件跳过，检索不写镜像；编辑态须先保存或取消，才能执行 grep。长行不受 16KiB 草稿容量限制。接口见 [Search](os/search.h) 与 [线性 backend](os/search_linear.h)。

保存并退出编辑态后，还可用词项搜索：

```text
search 修订
search CPU 中断
index status
rebuild
index clear
```

ASCII词按完整词匹配、忽略ASCII大小写，interrupt不会命中interruption；中文组内字符必须相邻，“中断”可命中“处理中断”，不能命中“中 foo 断”。空格分隔的组在同一行做AND，去重后最多16组。结果按分数、目录槽、行号排序，显示最多160字符的安全摘要及总匹配数；零匹配也成功。索引只在RAM，重启为EMPTY；clear只丢弃索引，容量不足回退scan，均不修改笔记。接口见 [term facade](os/search_terms.h) 与 [索引backend](os/search_terms_index.h)。

保存并退出编辑态后，可为笔记声明元数据：

```text
meta set note.txt title 中断笔记
meta tag note.txt add os
meta link note.txt add irq.txt
meta show note.txt
meta list tag:os
search tag:os 中断
```

日期由用户声明，不自动更新时间；不存在的链接显示DANGLING。meta更新只处理已有合法文本文件，整文件含头最多16384字节；show/list及标签搜索不受此大小限制。相同值、重复add或不存在的del成功而不写盘。实际更新保留正文原字节；元数据头仍属于原始文件，编辑器可改坏它，观察与标签搜索明确报告METADATA。无tag查询继续检索原字节和物理行号。接口见 [纯格式](os/note_metadata_format.h) 与 [元数据服务](os/note_metadata.h)。

查看声明关系时，先保存或取消编辑草稿，再执行：

```text
links note.txt
backlinks irq.txt
related note.txt
```

links保留声明顺序，目标缺失显示DANGLING；backlinks按ASCII文件名列出引用者，允许查询已删除的名字。related先按双向/单向链接排序，再比较共同标签数和文件名，排除自身。backlinks与related在输出前检查全库头，任意坏头均报METADATA；links只检查源头及目标是否存在。正文可以是二进制，每次查询重新观察，不自动修复重命名或删除留下的断链。接口见 [关系服务](os/note_relations.h)。

生产入口见 [应用](apps/yanfs_terminal/main.c)，公共接口见 [shell](os/shell.h)、[读行器](os/line.h) 和 [UART 终端](os/terminal.h)。应用依赖 `os/`，不依赖 `tests/`。

Linux/WSL可选构建Host语义工具。需要提供带CMake package的libcurl与yyjson（本轮JSON依赖基线为0.13.0），默认构建不要求它们：

```sh
cmake -S . -B build/semantic -DBUILD_TESTING=OFF -DYAN_BUILD_SEMANTIC=ON \
  -DCMAKE_PREFIX_PATH="/path/to/curl-prefix;/path/to/yyjson-prefix"
cmake --build build/semantic --target yan_semantic --parallel
build/semantic/yan_semantic rebuild --disk-image notes.img --index notes.index --model model.gguf --runner /path/to/llama-server
build/semantic/yan_semantic search --disk-image notes.img --index notes.index --model model.gguf --runner /path/to/llama-server --query "设备如何叫醒处理器"
build/semantic/yan_semantic search --disk-image notes.img --index notes.index --model model.gguf --runner /path/to/llama-server --query "设备如何叫醒处理器" --format json > context.json
build/semantic/yan_semantic status --disk-image notes.img --index notes.index --model model.gguf --runner /path/to/llama-server
```

运行前停止使用该镜像的Guest，并保持镜像、模型、runner和索引稳定；工具不下载模型或自动格式化。旁置索引在来源或模型身份变化后STALE，须显式rebuild。结果是原文候选，不生成答案；没有相关内容的问题也可能返回20段。首次加载、每次完整身份校验与模型内存有成本，固定小语料复验不代表任意知识库质量。实现与验证边界见[0029](docs/specs/0029-host-semantic-search.md)和[STATUS](docs/STATUS.md)。

search省略格式或使用`--format text`保持人读输出；`--format json`输出完整命中原文、物理行号与半开字节范围，以及同次来源、模型、runner和配置四身份。只有search接受该参数。调用方先检查退出码为0，再完整解析JSON；输出失败可能留下前缀，不能仅凭文件存在或JSON可解析就消费。接口与契约见[0030](docs/specs/0030-retrieval-context-json.md)及[host/semantic.h](host/semantic.h)。

真实模型效果采用操作员显式运行的独立验收脚本，不注册默认CTest，也不进入普通CI；需要手动提供已准备的模型、runner和`/usr/bin/time`：

```sh
python3 -B tests/semantic_model_check.py --tool build/semantic/yan_semantic \
  --model model.gguf --runner /path/to/llama-server --work build/semantic-real-check --json-context
```

脚本实际调用生产工具，保存身份、原文出处、预标签Top5结果和耗时/RSS报告。只有显式添加`--json-context`才同时检查完整JSON上下文包的身份、排名和原文范围；不添加时保留原文本效果验收。工具缺失硬失败；外部依赖缺失明确返回77，表示效果待验，不能当通过或静默跳过。默认fake provider/oracle通过不能替代这项效果验证。

### Agent/harness MCP接入

同一可选语义工具配置可构建`yan_mcp`；生产目标不要求测试依赖：

```sh
cmake --build build/semantic --target yan_mcp --parallel
build/semantic/yan_mcp --disk-image /absolute/path/notes.img \
  --index /absolute/path/notes.index --model /absolute/path/embedding.gguf \
  --runner /absolute/path/llama-server
```

stdio server的stdout只含协议，不是交互式命令行。调用方以子进程连接，发现yanos_search与yanos_index_status；只固定启动路径，工具参数不能指定文件或执行命令。来源镜像须停止Guest并在调用期间保持稳定，索引需预先用yan_semantic显式rebuild；服务不自动重建、写笔记或生成回答。取消与EOF会停止当前任务并回收本服务拥有的后代，kernel I/O阻塞不承诺固定总回收时限。兼容范围与结果schema见[0031](docs/specs/0031-mcp-retrieval-tools.md)。

通用stdio启动参数示例，字段对应SDK的启动配置；不同harness的注册位置由调用方适配，不自动修改全局配置：

```json
{
  "command": "/absolute/path/yan_mcp",
  "args": ["--disk-image", "/absolute/path/notes.img", "--index", "/absolute/path/notes.index", "--model", "/absolute/path/embedding.gguf", "--runner", "/absolute/path/llama-server"]
}
```

官方Python SDK本阶段固定`mcp==2.3.0`，可在调用方自己的环境使用以下接口。`mode="2026-07-28"`明确选现代协议；定向验收另覆盖`legacy`与`auto`并检查实际版本，不把自动回退当现代兼容通过：

```python
import asyncio
import json
from mcp import Client
from mcp.client.stdio import StdioServerParameters

async def main():
    target = StdioServerParameters(
        command="/absolute/path/yan_mcp",
        args=["--disk-image", "/absolute/path/notes.img",
              "--index", "/absolute/path/notes.index",
              "--model", "/absolute/path/embedding.gguf",
              "--runner", "/absolute/path/llama-server"])
    async with Client(target, mode="2026-07-28",
                      read_timeout_seconds=150) as client:
        assert client.protocol_version == "2026-07-28"
        tools = await client.list_tools()
        result = await client.call_tool(
            "yanos_search", {"query": "设备如何叫醒处理器"})
        value = result.model_dump(by_alias=True, exclude_none=True)
        if value["isError"]:
            raise RuntimeError(value["content"][0]["text"])
        context = value["structuredContent"]
        assert json.loads(value["content"][0]["text"]) == context
        print(json.dumps(context, ensure_ascii=False))

asyncio.run(main())
```

Agent/harness取得完整原文、出处和四身份后自行回答；引用存在不证明答案内容真实。官方SDK是显式集成验收依赖，不是生产C工具或默认核心构建依赖。当前本机全量及真实模型验收进度见[STATUS](docs/STATUS.md)，不由上述用法示例推导已完成覆盖。

变异检查默认关闭。启用工具与所需依赖后，添加 `-DYAN_ENABLE_MUTATION_TESTS=ON` 在Linux工具/provider开启配置可注册十六组：`mcp_mutation`、`semantic_mutation`、`note_relations_mutation`、`note_metadata_mutation`、`search_terms_mutation`、`search_mutation`、`editor_mutation`、`terminal_mutation`、`yanfs_mutation`、`persistent_block_mutation`、`persistent_combined_mutation`、`guest_console_mutation`、`device_mutation`、`guest_runtime_mutation`、`guest_block_mutation`、`guest_m2a_combined_mutation`。默认套件中的 `mcp_mutation_gate`、`semantic_mutation_gate`、`note_relations_mutation_gate`、`note_metadata_mutation_gate`、`search_terms_mutation_gate`、`search_mutation_gate`、`editor_mutation_gate`、`yanfs_mutation_gate` 和 `terminal_mutation_gate` 检查分类器判据，随默认回归运行；用 `-E '_mutation$'` 运行默认套件，`-R '_mutation$'` 选择实际变异。NEMU 差分变异组按外部参考模型依赖另行注册。运行要求见 [开发准则](CONTRIBUTING.md)，当前计数与测量日期见 [STATUS](docs/STATUS.md)。

`yan_run` 的默认执行路径由 `guest_golden` 用基准工件逐字节守住：改动默认行为会让它失败，先读差异再决定是否有意为之。生成与重生成见 [`tests/guest/golden/README.md`](tests/guest/golden/README.md)。

外部验证层是可选依赖，缺失时对应的 CTest 不注册或报 SKIP，从不显示为通过：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DYAN_BUILD_TOOLS=ON \
  -DYAN_NEMU_REF_SO=/path/to/riscv32-nemu-interpreter-so \
  -DYAN_NEMU_REF_DIR=/path/to/nemu-source \
  -DYAN_RISCV_TESTS_DIR=/path/to/riscv-tests \
  -DYAN_RISCV_ARCH_TEST_DIR=/path/to/riscv-arch-test \
  -DYAN_SAIL_BIN=/path/to/sail_riscv_sim
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

分层、覆盖边界与退出码见 [CPU 验证规格](docs/specs/0011-cpu-validation.md)。

## 设计

YanCPU 使用 C17 编写，在本机命令行运行。初始目标为 RV32IM_Zicsr，采用 32 位、小端、单核和 M-mode 配置。模拟器关注指令执行后的状态变化，电路时序和微架构留待后续探索。

Guest 程序使用 C 和少量 RISC-V 汇编。CPU 经 Bus 访问 RAM 与虚拟设备，设备由 Host 提供终端和存储适配。Host 与 Guest 分别编译，通过虚拟硬件接口交互。

| 组件 | 职责 |
| --- | --- |
| YanCPU | 指令执行、寄存器、CSR、异常和中断 |
| Yan Machine Platform | Bus、RAM、终端、计时器与块设备 |
| YanOS | 启动、驱动、内存管理和系统接口 |
| YanFS | 单根目录文件系统，格式与失败边界见 [0021](docs/specs/0021-yanfs.md) |
| Yan Knowledge System | 笔记、问题、关系和探索路径的终端交互 |

代码按职责分区：Host侧`src/`与`include/`负责CPU、Bus与设备，`host/`负责检索核心、索引与只读来源适配，`tools/`负责Host CLI和MCP服务；Guest侧`os/`负责驱动、运行时与YanFS，`apps/`为Guest生产应用，`tests/`负责验证。依赖方向为`apps/` → `os/`，生产应用与`os/`不引用`tests/`；测试可以引用生产层。旧控制台分层约束见[0017](docs/specs/0017-console-and-os-layout.md)，独立应用入口见[0022](docs/specs/0022-terminal-file-operations.md)。

各组件的完整能力将分阶段实现。浏览器支持计划通过 WebAssembly 和界面适配接入；新增能力的时机由实际使用决定。

## 开发

参阅 [开发准则](CONTRIBUTING.md)、[阶段 0 规格](docs/specs/0003-machine-bus-ram.md)、[CPU 状态与取指规格](docs/specs/0004-cpu-state-fetch.md)、[ADDI 单步执行规格](docs/specs/0005-addi-step.md)、[整数计算规格](docs/specs/0006-integer-alu.md)、[控制转移规格](docs/specs/0007-control-flow.md)、[Load / Store 规格](docs/specs/0008-load-store.md)、[Guest 异常规格](docs/specs/0009-guest-traps.md)、[M 扩展规格](docs/specs/0010-m-extension.md)、[CPU 验证规格](docs/specs/0011-cpu-validation.md)、[机器模式中断规格](docs/specs/0012-machine-interrupts.md)、[Guest 启动与统一 trap 环境规格](docs/specs/0013-guest-trap-environment.md)、[Host 传输通道规格](docs/specs/0014-host-transport-channel.md)、[UART 字符设备规格](docs/specs/0015-uart-device.md)、[PLIC 网关与设备中断线规格](docs/specs/0016-plic-gateway-and-irq-lines.md)、[控制台与 `os/` 层规格](docs/specs/0017-console-and-os-layout.md)、[块请求协议规格](docs/specs/0018-block-protocol.md)、[协作式运行时规格](docs/specs/0019-cooperative-runtime.md)、[持久化块镜像规格](docs/specs/0020-persistent-block-image.md)、[YanFS 规格](docs/specs/0021-yanfs.md) 、[终端文件操作规格](docs/specs/0022-terminal-file-operations.md) 、[多行文本编辑规格](docs/specs/0023-multiline-text-editor.md) 、[重命名与复制规格](docs/specs/0024-file-rename-copy.md) 、[知识检索规格](docs/specs/0025-knowledge-search.md) 、[词项搜索与索引规格](docs/specs/0026-term-search-index.md) 、[笔记元数据规格](docs/specs/0027-note-metadata.md) 、[知识关系导航规格](docs/specs/0028-knowledge-relations.md) 、[Host语义检索规格](docs/specs/0029-host-semantic-search.md) 、[检索上下文包规格](docs/specs/0030-retrieval-context-json.md) 和 [MCP检索工具规格](docs/specs/0031-mcp-retrieval-tools.md)。

## 许可证

待选定。
