# 用 dsh 连接 YanOS 笔记库

YanOS 通过 stdio MCP 向 dsh 提供检索、原文块和笔记写入工具。普通笔记库与学习记录各用一个镜像、一个服务进程；下面的普通实例只读，学习实例开启写入。协议和工具参数沿用 [0033](specs/0033-block-mcp-navigation.md) 与 [0035](specs/0035-agent-writable-knowledge.md)。

YanOS 使用现有本地 embedding 模型检索，回答由 dsh 当前使用的模型生成。工具返回的原文会进入 dsh 的模型上下文。本例的接入方式按 dsh `0.2.0-rc.2` 核对，实际验收范围见 [0035 的 VERIFY 记录](specs/0035-agent-writable-knowledge.md)。

## 准备镜像和启动配置

先按 [README](../README.md#构建与测试) 构建 Linux／WSL 工具。普通库需要已有镜像，以及按 [Host 语义检索规格](specs/0029-host-semantic-search.md)建立的有效索引、embedding 模型和 runner；调用期间 Guest 应已退出、镜像保持稳定。

学习库可以从空镜像开始。新路径上的以下命令创建 64 个块、63 个文件槽的 v1 镜像；空库仍留有数据空间：

```sh
build/semantic/yan_mkfs --image /absolute/path/learning.img --blocks 64 --files 63
```

在自己的配置目录创建 `yanos-mcp.patch.yml`，将绝对路径换成实际路径。学习实例的后三个依赖路径可指向不存在的文件：块读取和写入不打开它们，调用学习实例的语义搜索则会报 DEPENDENCY。

```yaml
- insert:
    - id: mcp-yanos-notes
      name: '@deepseek-ai/dsh-mcp-client'
      config:
        serverName: yanos_notes
        transport: stdio
        command: /absolute/path/yan_mcp
        args:
          - --disk-image
          - /absolute/path/notes.img
          - --index
          - /absolute/path/notes.index
          - --model
          - /absolute/path/embedding.gguf
          - --runner
          - /absolute/path/llama-server
        toolCallTimeoutMs: 180000
        reconnect:
          enabled: false
    - id: mcp-yanos-learning
      name: '@deepseek-ai/dsh-mcp-client'
      config:
        serverName: yanos_learning
        transport: stdio
        command: /absolute/path/yan_mcp
        args:
          - --disk-image
          - /absolute/path/learning.img
          - --index
          - /absolute/path/unused-learning.index
          - --model
          - /absolute/path/unused-learning.gguf
          - --runner
          - /absolute/path/unused-learning-runner
          - --allow-write
        toolCallTimeoutMs: 180000
        reconnect:
          enabled: false
```

使用尚未存在的 profile 名，从 dsh 自带 headless 模板初始化一次：

```sh
dsh --profile yanos-notes --from-default-profile headless --dump-config >/dev/null
```

之后启动时省略 `--from-default-profile`，通过任务自己的 overlay 接入两个实例：

```sh
dsh --profile yanos-notes --patch /absolute/path/yanos-mcp.patch.yml --json -
```

`-` 表示从 stdin 读取任务，`--json` 输出 dsh 的执行记录。认证和模型服务沿用 dsh 自身配置。专用 profile 使这次启动生成的配置与日常 profile 分开；不需要修改全局配置或重启 Web 服务。

## 读原文，再预览学习记录

dsh 中的工具名带实例前缀，例如 `mcp__yanos_notes__yanos_search` 和 `mcp__yanos_learning__yanos_note_create`。普通实例有六个只读工具，学习实例另有新建与整文件覆盖。一次调用一个工具，等待结果后再调用下一个；同一实例的合法并发任务会返回 BUSY。

使用仓库的普通笔记夹具时，可以让 dsh 按以下顺序完成一次预览：

1. 在普通实例检查索引为 READY，搜索“设备如何叫醒处理器”。
2. 用检索返回的来源身份列出 `irq` 的块，再读取 `irq#entry`，保留原文与物理出处。
3. 整理含 `criteria`、`known`、`unknown`、`untouched`、`log` 五块的学习记录，`known` 保持空白，不编造回答或理解状态。
4. 在学习实例调用新建工具，明确带 `dry_run:true`，展示返回的完整 `text` 与 `diff`，等待确认。

提交前，把预览的 `text` 和 `diff` 展示给闫博，在同一会话里得到明确确认后，才用同一 name、同一 text、预览返回的身份提交。把条目移入 `known` 另需闫博确认理解；仅保存记录不表示已经学会。

来源变化时报 STALE，应重新预览并确认。预览身份只绑定库状态，不能证明人工确认或绑定提交内容；这些约束由 Agent 遵守。具体参数、错误与重试顺序见[写工具说明](mcp-write-tools.md)。

学习库里的 `[[irq#entry]]` 按当前库解析，会显示 DANGLING_FILE；dsh 应选择普通实例读原文。两个实例不自动跨库解析引用。首次试用使用独立样本镜像，完整的确认提交与 Guest 读回验收另见规格，不由配置加载成功推导。
