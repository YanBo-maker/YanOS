# 写入类 MCP 工具

本文说明 `yan_mcp` 的 `--allow-write`、两个写工具、镜像旁的锁文件，以及 `notes.img` 和 `learning.img` 怎么各起一个进程。写工具的 `inputSchema`、`outputSchema` 和 `annotations` 从 [`tools/mcp_note.c`](../tools/mcp_note.c) 的工具 JSON 解析得到，与源码逐字段相同；该文件自 `2921fed98a55358bb97282b0181072a2769b4e76`（[PR #55](https://github.com/YanBo-maker/YanOS/pull/55) 的合并提交）起未改动，所以本页写工具字段的逐字段说明仍以它为据。其余段落只说明这些字段怎么调用。

`mcp_note` 在实际 `--allow-write` 服务上读取 `tools/list`，逐项比较本页两个工具的名称、description、annotations、inputSchema 和 outputSchema；字段或类型不同、文档缺失或 JSON 损坏都会让测试失败。检查器在 [`tests/mcp_write_docs.py`](../tests/mcp_write_docs.py)，文档中的 create 输入结构仍通过明确引用与 replace 共享。这次工具列表查询不打开镜像或模型。

在 Linux semantic/MCP 与 Guest 测试目标齐备时，运行 `ctest --test-dir <构建目录> -R '^mcp_note$' --output-on-failure` 可核对真实接口。纯 Python 的 `mcp_write_docs` 测试在配置找到 Python 时注册，用篡改文档和返回值核对检查器；它也在 Windows 运行。官方 SDK 的工具发现验收仍是 `mcp_sdk`。本检查的执行记录见 0035 VERIFY，README、状态和用法说明继续由独立文档审核核对。

规格见 [0035](specs/0035-agent-writable-knowledge.md)，并发裁定见 [0002](decisions/0002-B-write-concurrency.md)。始终可见的六个只读工具协议见 [0031](specs/0031-mcp-retrieval-tools.md) 和 [0033](specs/0033-block-mcp-navigation.md)。

另有两个只读工具（`yanos_note_meta` 单文件元数据、`yanos_note_list` 全库名单与 tag AND 筛选）由**独立开关 `--allow-meta`** 门控，与本文的 `--allow-write` 正交，见 [0041](specs/0041-note-meta-mcp-tool.md)。因此 `tools/list` 有四种形态：不带开关 6 个、仅 `--allow-meta` 8 个、仅 `--allow-write` 8 个、两者同开 10 个（顺序为六只读 → meta → list → create → replace）。`--allow-meta` 只影响只读工具是否出现，不影响本文任何写工具行为。

## `--allow-write`

`yan_mcp` 的四个路径参数仍然必填：`--disk-image`、`--index`、`--model`、`--runner`。`--allow-write` 是不带值的开关，写两次会以退出码 2 打印 `ERROR USAGE`（[`tools/yan_mcp.c`](../tools/yan_mcp.c) 的 `options`）。默认关闭。

关闭时，`tools/list` 只有始终可见的六个只读工具（带 `--allow-meta` 时再加上那两个 meta 工具，见上节）。调用 `yanos_note_create` 或 `yanos_note_replace` 走和未知工具名同一条路径：JSON-RPC 错误 `-32602`（Invalid params），不进入写工具（[`tools/mcp_protocol.c`](../tools/mcp_protocol.c) 里 `allow_write` 为假时不识别这两个名字）。

打开时，这两个工具接在只读工具之后（只读工具为六个，或带 `--allow-meta` 时为八个），顺序是 `yanos_note_create`、`yanos_note_replace`。

`initialize` 的 `serverInfo` 和现代结果 `_meta` 里的 `io.modelcontextprotocol/serverInfo` 都是 `{"name":"YanOS","version":"0035"}`。版本与开关无关（[`tools/mcp_protocol.c`](../tools/mcp_protocol.c) 的 `SERVER_INFO`）。

服务不重建索引，也不生成回答。写工具只改启动时那一个镜像里的一个文件：新建或整文件覆盖，不删除、不改名、不追加。

## 两个镜像各起一个进程

每个 `yan_mcp` 只接受一个 `--disk-image`。普通笔记和学习笔记分成两个进程，在客户端里配成两个 server。锁文件按镜像的 `realpath` 区分；整库身份 `source_sha256` 按目录槽号、名称、大小和全部文件内容计算，不包含镜像路径。保留这些信息的副本可以有相同身份。两个进程的 `--disk-image` 指向同一个文件时，`realpath` 相同，共用一把锁，调用按单镜像处理。

```sh
build/semantic/yan_mcp --allow-write \
  --disk-image /absolute/path/notes.img \
  --index /absolute/path/notes.index \
  --model /absolute/path/embedding.gguf \
  --runner /absolute/path/llama-server

build/semantic/yan_mcp --allow-write \
  --disk-image /absolute/path/learning.img \
  --index /absolute/path/learning.index \
  --model /absolute/path/embedding.gguf \
  --runner /absolute/path/llama-server
```

学习实例也要给出 index、model、runner 三个路径，这三个文件可以还不存在，进程仍能启动。块工具和写工具用 `yan_semantic_posix_open_image_ex` 只打开镜像。`yanos_search` 走 `yan_semantic_posix_open`：model 或 runner 打不开时返回 `YAN_SEMANTIC_DEPENDENCY`（[`host/semantic_posix.c`](../host/semantic_posix.c)）。[`tests/mcp_note_checks.py`](../tests/mcp_note_checks.py) 里学习实例这三项都不存在时，`yanos_search` 的工具错误是 `{"code":"DEPENDENCY"}`。

构建需要 Linux/WSL 上的 `-DYAN_BUILD_SEMANTIC=ON`。非 Linux 打开这个开关会在配置阶段失败（`CMakeLists.txt`）。

## `<image>.lock`

锁文件路径是 `realpath(镜像) + ".lock"`。`yan_image_lock` 用 `open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644)` 打开它，再 `flock`，标志带 `LOCK_NB`（[`host/note_write.c`](../host/note_write.c)）。文件可以是空的；这个函数不往里面写字节。

谁会创建并持有它：

- `yan_run --disk-image` 在 Linux 上于打开镜像之前拿 `LOCK_EX`，一直持有到进程退出。拿不到时向 stderr 打印 `yan_run: disk image '<路径>' is locked by another writer`，退出码 5（[`tools/yan_run.c`](../tools/yan_run.c)，`EXIT_HOST_ERROR`）。这段只包在 `__linux__` 里。
- 写工具提交拿 `LOCK_EX`，`dry_run: true` 的预览拿 `LOCK_SH`。调用返回前关闭描述符，锁随之放开（[`tools/mcp_note.c`](../tools/mcp_note.c) 的 `open_note` / `close_note`）。

文件在第一次成功走到 `open` 时出现。镜像路径还不存在时，`realpath` 失败，函数返回 `YAN_FS_IO`，不会创建锁文件。参数不合法的写调用在加锁之前就返回，也不会为了这一次去创建它。预览不写镜像，但同样经过 `yan_image_lock`，所以目录里还没有锁文件时，第一次 `dry_run` 就会把它创建出来。

`yan_image_lock`、`yan_note_write`、`yan_run`、`yan_mcp` 都不删除这个文件。留在镜像旁边。持锁进程退出或被杀死后，内核随文件描述符关闭放开 `flock`，文件还在，下一次可以再拿锁。

写工具提交时先把新内容写进同目录的 `<image>.XXXXXX` 临时文件（`mkstemp`），`rename` 成功后才释放路径。正常返回和所有失败路径都会清理它；只有进程被 `SIGKILL` 终止（不可捕获）才会在镜像目录留下这样一个文件。这类残留不带锁、不参与加锁判断，也不影响后续写入，可以安全删除；写入路径不会读取、复用或覆盖已存在的临时文件，下一次提交总是新建一个。实现里没有任何启动时扫描目录清理历史残留的代码。

处理：

- 写工具返回 `{"code":"BUSY"}`：有进程正持有排他锁（正在跑的 `yan_run --disk-image`，或一次还没返回的提交）。预览与提交都是 `LOCK_NB`，不会等。等对方退出后再调。服务里若已有一个工具调用未结束，父进程也会直接返回 `BUSY`，这时还没有去拿文件锁（[`tools/yan_mcp.c`](../tools/yan_mcp.c) 的 `handle`）。
- 所有只读工具都不拿这把锁：始终可见的六个，以及由 `--allow-meta` 启用的 `yanos_note_meta`、`yanos_note_list` 同样只以只读方式打开镜像。
- 不要删 `<image>.lock` 来“解锁”。已经持有的 `flock` 挂在打开的描述符上；删掉路径再新建的是另一个文件。

共享锁可以并存。排他锁在已有共享锁或另一把排他锁时返回 `YAN_FS_BUSY`，写工具把它映射成 `BUSY`。

## 预览和提交

两个工具的参数相同。`name` 和 `text` 必填。`dry_run` 缺省为 false。提交（`dry_run` 缺省或 false）必须带 `expected_source`；预览可以不带。多出来的字段会被拒绝。

先预览：`dry_run` 设为 true，传入 `name` 和 `text`。成功时 `isError` 为 false，`structuredContent` 与 `content[0].text` 是同一份 JSON，其中 `source_sha256` 是当前整库身份，`text` 是将写入的全文，`diff` 是行差异。

Agent 可以出题、核对、写 `log` 和 `unknown`，把条目移入 `known` 必须在同一会话里得到闫博明确确认。提交前，把预览的 `text` 和 `diff` 展示给闫博，在同一会话里得到明确确认后，才用同一 name、同一 text、预览返回的身份提交。上面两句与两个工具 description 里的原文一致。学习与理解程度仍由项目所有者判断。

确认之后再调同一个工具：`dry_run` 为 false 或不传，`expected_source` 填预览返回的 `source_sha256`，`name` 和 `text` 与预览时相同。成功响应里 `source_sha256` 是写后的整库身份；`previous_source_sha256` 在新建时为 null，在覆盖时为写前身份。只有目标文件已经存在且与 `text` 逐字节相同时，响应才多一个 `"unchanged": true`，并且不写盘（[`tools/mcp_note.c`](../tools/mcp_note.c) 的 `encode_commit`）。

`diff` 去掉新旧内容相同的前缀行和后缀行。每行带上结尾的 LF；末尾没有 LF 的一段也算一行。完全相同时 `start_line` 为 0，`removed` 和 `added` 都是空数组。新建时旧内容是零行；新内容非空时 `start_line` 为 1，新内容也为空时为 0，两个行数组均为空。[`tests/mcp_note_checks.py`](../tests/mcp_note_checks.py) 的预览用例固定了四组结果，例如旧内容 `x\ny\nz\n`、新内容 `x\nY\nz\n` 时，`diff` 为 `{"start_line":2,"removed":["y\n"],"added":["Y\n"]}`。

名称在 schema 里是 1–31 个字符。真正写入前还要过 YanFS 名称规则：只含 `A-Za-z0-9._-`，拒绝 `.` 和 `..`（[`os/note_metadata_format.c`](../os/note_metadata_format.c) 的 `yan_note_name_valid`）。正文长度不超过 16384 字节，并且要通过 0032 的文本检查（[`host/note_write.c`](../host/note_write.c) 的 `yan_note_write_check`）。不合格时工具错误为 `{"code":"INVALID"}`，不打开镜像。

### 调用方遇到 STALE

`STALE` 是工具错误，不是 JSON-RPC 错误。`isError` 为 true，没有 `structuredContent`，`content[0].text` 是 `{"code":"STALE"}`。镜像字节不变。

提交时的判定顺序在通过参数检查并拿到锁之后是（[`tools/mcp_note.c`](../tools/mcp_note.c) 的 `yan_mcp_note_execute`）：

1. 文件已存在，且内容和 `text` 逐字节相同：成功，带 `unchanged: true`，不写盘。这一步不比较 `expected_source`。
2. 带了 `expected_source`，且不等于当前整库身份：`STALE`。
3. 新建时名字已存在：`EXISTS`。覆盖时名字不存在：`NOT_FOUND`。
4. 否则写入。

预览没有第 1 步。带了过期的 `expected_source` 时先返回 `STALE`，然后才是 `EXISTS`、`NOT_FOUND`，覆盖预览还可能因旧文件不是合法文本而返回 `INVALID_TEXT`。

调用方这样处理 `STALE`：

1. 不要用同一个 `expected_source` 再提交另一份 `text`。
2. 再调一次同一工具，`dry_run` 为 true，这次不要带旧的 `expected_source`。
3. 把新预览的 `text` 和 `diff` 再给闫博看，在同一会话里得到明确确认。
4. 用新预览的 `source_sha256` 作为 `expected_source`，连同同一 `name`、同一 `text` 提交。
5. 如果只是重试一份已经写进去的相同内容，第 1 步会返回 `unchanged: true`。收到它可以停止，不必再预览。

`EXISTS` 表示新建时名字已被别的内容占用，应改用 `yanos_note_replace` 并重新预览。`NOT_FOUND` 表示覆盖时文件不在，应改用 `yanos_note_create` 并重新预览。

## 学习记录的完整示例

[`tests/learning-note/`](../tests/learning-note/) 提供初始学习记录、更新后的记录和普通笔记原文。初始记录的 `known` 为空；更新记录把 `unknown` 的一项移入 `known`，并在 `log` 追加一条明确标为模拟确认的核对记录。示例不表示项目所有者已经学会。

使用时，调用方先在普通实例读取 `[[irq#entry]]` 等原文，再在学习实例预览完整记录，把预览内容与差异展示给用户。获得明确确认后，用同一名称、同一正文和预览返回的身份提交。修改已有记录也走相同流程。学习实例的引用仍显示 `DANGLING_FILE`：这表示目标文件不在当前镜像，调用方应选择普通实例读取依据。

[`guest_learning_note`](../tests/guest/run_learning_note.py) 验收在临时双镜像上执行这条流程，核对重启 MCP 后的原文和生产 Guest 的读回结果。它只模拟确认，不证明真实用户理解，也不证明任意 Agent 都会遵守确认规则。注册条件和运行入口见 [README](../README.md#写入笔记)。

## 工具参考

失败时 `content[0].text` 是一个 JSON 对象，至少有 `code`。镜像已经通过 rename 替换、随后目录 fsync 或重新计算身份失败时，`code` 为 `IO`，并带 `"committed": true`（[`tools/mcp_protocol.c`](../tools/mcp_protocol.c) 的 `yan_mcp_reply_ex`）。没有 `committed` 时按未写入处理。

`code` 与 [`yan_mcp_note_result_name`](../tools/mcp_note.c) 的返回值一致：`INVALID`、`BUSY`、`STALE`、`EXISTS`、`NOT_FOUND`、`INVALID_TEXT`、`CORRUPT`、`UNSUPPORTED`、`DIRECTORY_FULL`、`NOSPACE`、`IO`。

### `yanos_note_create`

description：

> 新建一个 YanFS 笔记文件；不删除、不改名、不追加，整文件写入，单次最多 16384 字节。提交（dry_run 缺省或 false）必须带预览返回的 expected_source；写入成功响应的 source_sha256 是写后身份，previous_source_sha256 是写前身份（create 时为 null）。学习笔记只放在学习镜像：正文用固定 ID 的五个具名块 criteria（学会标准，每条一行、可检查）、known（闫博确认已懂）、unknown（已知不懂）、untouched（还没碰过）、log（每次核对的日期、题目、回答要点、被指出的问题和 [[NAME#ID]] 原文出处）。Agent 可以出题、核对、写 `log` 和 `unknown`，把条目移入 `known` 必须在同一会话里得到闫博明确确认。提交前，把预览的 `text` 和 `diff` 展示给闫博，在同一会话里得到明确确认后，才用同一 name、同一 text、预览返回的身份提交。

annotations：

```json
{
  "readOnlyHint": false,
  "destructiveHint": false,
  "openWorldHint": false
}
```

`inputSchema` 与 `yanos_note_replace` 相同，见下一节。

outputSchema：

```json
{
  "type": "object",
  "oneOf": [
    {
      "additionalProperties": false,
      "required": [
        "schema_version",
        "operation",
        "name",
        "bytes",
        "source_sha256",
        "previous_source_sha256"
      ],
      "properties": {
        "schema_version": {
          "const": 1
        },
        "operation": {
          "const": "note_create"
        },
        "name": {
          "type": "string",
          "minLength": 1,
          "maxLength": 31
        },
        "bytes": {
          "type": "integer",
          "minimum": 0,
          "maximum": 16384
        },
        "source_sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "previous_source_sha256": {
          "type": "null"
        },
        "unchanged": {
          "type": "boolean"
        }
      }
    },
    {
      "additionalProperties": false,
      "required": [
        "schema_version",
        "operation",
        "name",
        "source_sha256",
        "text",
        "diff"
      ],
      "properties": {
        "schema_version": {
          "const": 1
        },
        "operation": {
          "const": "note_create_preview"
        },
        "name": {
          "type": "string",
          "minLength": 1,
          "maxLength": 31
        },
        "source_sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "text": {
          "type": "string"
        },
        "diff": {
          "type": "object",
          "additionalProperties": false,
          "required": [
            "start_line",
            "removed",
            "added"
          ],
          "properties": {
            "start_line": {
              "type": "integer",
              "minimum": 0
            },
            "removed": {
              "type": "array",
              "items": {
                "type": "string"
              }
            },
            "added": {
              "type": "array",
              "items": {
                "type": "string"
              }
            }
          }
        }
      }
    }
  ]
}
```

### `yanos_note_replace`

description：

> 整文件覆盖一个 YanFS 笔记文件；不删除、不改名、不追加，单次最多 16384 字节。提交（dry_run 缺省或 false）必须带预览返回的 expected_source；写入成功响应的 source_sha256 是写后身份，previous_source_sha256 是写前身份。学习笔记只放在学习镜像：正文用固定 ID 的五个具名块 criteria（学会标准，每条一行、可检查）、known（闫博确认已懂）、unknown（已知不懂）、untouched（还没碰过）、log（每次核对的日期、题目、回答要点、被指出的问题和 [[NAME#ID]] 原文出处）。Agent 可以出题、核对、写 `log` 和 `unknown`，把条目移入 `known` 必须在同一会话里得到闫博明确确认。提交前，把预览的 `text` 和 `diff` 展示给闫博，在同一会话里得到明确确认后，才用同一 name、同一 text、预览返回的身份提交。

annotations：

```json
{
  "readOnlyHint": false,
  "destructiveHint": true,
  "idempotentHint": true,
  "openWorldHint": false
}
```

inputSchema（两个写工具相同）：

```json
{
  "type": "object",
  "additionalProperties": false,
  "required": [
    "name",
    "text"
  ],
  "properties": {
    "name": {
      "type": "string",
      "minLength": 1,
      "maxLength": 31
    },
    "text": {
      "type": "string"
    },
    "expected_source": {
      "type": "string",
      "pattern": "^[0-9a-f]{64}$"
    },
    "dry_run": {
      "type": "boolean"
    }
  }
}
```

outputSchema：

```json
{
  "type": "object",
  "oneOf": [
    {
      "additionalProperties": false,
      "required": [
        "schema_version",
        "operation",
        "name",
        "bytes",
        "source_sha256",
        "previous_source_sha256"
      ],
      "properties": {
        "schema_version": {
          "const": 1
        },
        "operation": {
          "const": "note_replace"
        },
        "name": {
          "type": "string",
          "minLength": 1,
          "maxLength": 31
        },
        "bytes": {
          "type": "integer",
          "minimum": 0,
          "maximum": 16384
        },
        "source_sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "previous_source_sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "unchanged": {
          "type": "boolean"
        }
      }
    },
    {
      "additionalProperties": false,
      "required": [
        "schema_version",
        "operation",
        "name",
        "source_sha256",
        "text",
        "diff"
      ],
      "properties": {
        "schema_version": {
          "const": 1
        },
        "operation": {
          "const": "note_replace_preview"
        },
        "name": {
          "type": "string",
          "minLength": 1,
          "maxLength": 31
        },
        "source_sha256": {
          "type": "string",
          "pattern": "^[0-9a-f]{64}$"
        },
        "text": {
          "type": "string"
        },
        "diff": {
          "type": "object",
          "additionalProperties": false,
          "required": [
            "start_line",
            "removed",
            "added"
          ],
          "properties": {
            "start_line": {
              "type": "integer",
              "minimum": 0
            },
            "removed": {
              "type": "array",
              "items": {
                "type": "string"
              }
            },
            "added": {
              "type": "array",
              "items": {
                "type": "string"
              }
            }
          }
        }
      }
    }
  ]
}
```

## 学习笔记的五个块

学习笔记只放在学习镜像里。文件格式与普通笔记相同。description 约定正文使用五个固定 ID：`criteria`、`known`、`unknown`、`untouched`、`log`。写工具不检查这些块在不在，两个镜像都按普通文本写入。`log` 里的 `[[NAME#ID]]` 指向普通笔记库中的文件；学习实例不会跨镜像解析引用。
