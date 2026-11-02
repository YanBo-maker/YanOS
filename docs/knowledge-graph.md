# 概念与命题图

图把笔记中显式记录的概念、关系和原文出处组织起来，供Agent从已有知识解释新内容。笔记内容按项目所有者声明视为已理解知识；图不评估掌握程度。普通正文仍然是知识来源，没有图声明不表示用户不懂。

Host命令和C接口由[0044](specs/0044-evidence-knowledge-graph.md)定义，实施与验收状态见[STATUS](STATUS.md)。图形前端、新增MCP图工具及自动关系抽取不在本阶段范围。

## 在笔记中记录关系

把声明放在已有具名块的正文中。例如：

```text
@block square-kind
@kg concept square 正方形
@kg concept rectangle 矩形
@kg alias square 等边矩形
@kg relation is_a square rectangle
这里讨论平面四边形。正方形满足矩形定义，并且四条边等长。
```

`square`和`rectangle`是库内概念ID，后面是显示名称。概念只需声明一次，关系也可先引用、后声明；另一个文件用相同ID和名称补充说明时，会合并概念并保留两处来源。同ID不同名称报CONFLICT；同名概念可以保留不同ID，按名称查询时报告歧义。

`is_a`是这条关系的类型，程序保留它的原字节，不自动做传递推理。每条关系出现都是独立命题，同一对概念的不同类型、重复关系和自环都保留。条件与解释放在所属块的原文中，读取时应一起查看。

声明之间使用一个ASCII空格。ID与关系类型的字符、标签的字节上限及错误边界见[0044 R1](specs/0044-evidence-knowledge-graph.md#r1正文声明与图核心)。`@kg`保留行不解释Markdown代码围栏或转义；要保留字面示例，可以在行首加普通字符。已有元数据头、`@block`和`[[NAME#ID]]`语法保持。

## 构建与查询

Linux/WSL可构建独立工具；这个目标不要求模型、语义索引、CURL、yyjson或RISC-V交叉编译器：

```sh
cmake -S . -B build/graph -DBUILD_TESTING=OFF -DYAN_BUILD_TOOLS=ON
cmake --build build/graph --target yan_graph --parallel
build/graph/yan_graph --disk-image notes.img list
build/graph/yan_graph --disk-image notes.img query square
build/graph/yan_graph --disk-image notes.img query 正方形
```

调用期间停止使用该镜像的Guest，并保持镜像稳定。命令只读镜像，每次调用重新构建内存图，退出后释放；不会修改笔记或保存另一份持久化图数据库。新增、修改、删除或改名会在下一次查询中反映出来。

`list`按概念ID列前20项并报告总数；`query`先匹配精确ID，再匹配名称或别名，返回概念、完整别名、前20处概念声明及前20条直接关系，并分别报告全部数量。关系按文件名及声明字节位置排列；自环只出现一次。当前CLI没有翻页参数，C接口可遍历完整图供后续适配使用。

复制笔记保留相同概念ID，因此同名定义会合并，复制的关系出现另行保留。改名只改变出处地址，不改变正文里的概念ID；如果删除后仍有其它定义，概念继续存在。如果关系所需的概念已无任何显式定义，构建返回UNRESOLVED，不自动补出定义。

## 给Agent使用

成功时stdout只有一个版本号为1的JSON对象。`source_sha256`绑定同次验证的逻辑笔记内容，`skipped`统计非文本文件；它是内容身份，不是作者认证。query结果中的`declarations`和`relations`分别含total、shown、items；每条关系含类型、两端概念、方向及where出处。完整字段见[0044接口字段](specs/0044-evidence-knowledge-graph.md#r2接口字段锁定)。

调用方先检查退出码，再解析完整JSON。例如：

```python
import json
import subprocess

result = subprocess.run(
    ["/absolute/path/yan_graph", "--disk-image", "/absolute/path/notes.img",
     "query", "square"],
    check=True, capture_output=True, text=True, encoding="utf-8")
graph = json.loads(result.stdout)
```

取得关系的where后，可用现有`yanos_block_read`读取完整原文：name取where.file，block_id取where.block_id，expected_source取本次graph.source_sha256。长块按[0033分页规则](specs/0033-block-mcp-navigation.md)继续读取；来源变化后重新查询，不把旧出处和新正文拼在一起。现有MCP服务的启动与工具列表保持，参见[接入说明](dsh-mcp-quickstart.md)。

Agent应根据具体命题和原文条件解释，不能因为两个概念都出现，或图里存在一条间接路径，就声称相应关系已经记录。NOT_FOUND只表示没有匹配的显式概念，可继续使用已有全文/语义检索寻找普通正文依据，不能将其翻译成“用户不懂”。

## 结果与限制

退出0才表示完整成功；参数错误退出2，stderr首行为ERROR USAGE并可附用法；其它普通错误退出1并给出单行错误token。发布前失败不输出JSON，真实输出故障可能留下前缀，因此不能只根据JSON文件存在或可解析就消费结果。

非文本文件整文件跳过；合法文本的坏元数据头、坏块或坏图声明会使本次构建失败，不返回半份图。超出图容量明确报LIMIT。前后来源身份观察用于发现变化，不提供快照；输入稳定仍由调用方负责。具体上限、错误及验证范围见0044，不从示例推导整个知识库已标注完整或Agent已经遵守解释规则。
