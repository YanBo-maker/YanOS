# 0034：CI 运行 Guest 默认测试与 STATUS 归档

- **意图**：让 CI 绿灯真实覆盖 Guest 测试，并把当前状态从历史记录里剥离出来，降低每次交付的验证和文档成本，为 B（写入类 MCP）扫清工程负担。见 [决策 0001](../decisions/0001-roadmap-A-D-B.md)。
- **状态**：Approved（2026-10-19，闫博）
- **supersedes**：无
- **成功标准**：闫博看到 PR 上的 Linux CI 绿灯，就能确信 Guest 默认测试真的编译并跑过了，工具链缺失或测试被跳过时一定是红灯；打开 STATUS 一屏内就能看清当前阶段和已交付能力，历史一条不丢；之后每次交付只需改 STATUS 三处，不再复写验证计数。

> 状态：**Approved**（批准日期 2026-10-19，项目所有者闫博批准；开放问题 1–4 均按默认方案裁定，R9 流程变更一并批准）。本规格只改 CI 配置和项目文档，不改功能代码和测试代码。前置条件：A（0033 的提交与 PR）已合入 main。

## INTENTION

远端 CI 绿灯目前只说明 Host 侧测试通过。原因是 `build.yml` 没有安装 RISC-V 交叉编译器，也没有开 `YAN_BUILD_TOOLS`，Guest 测试在 CI 里**根本不注册**（`CMakeLists.txt:190/988/1100` 等处的条件是 `YAN_BUILD_TOOLS AND UNIX AND YAN_RISCV_GCC`），而 Guest 验证全靠本机。另外，`docs/STATUS.md`（约 157KB）和 `MONITOR_REPORT.md`（约 301KB）把当前状态埋在历史记录里，每次交付都要同步几十处计数。

做完之后：PR 的 Linux CI 会真实编译 Guest 并运行默认 Guest 测试，工具链缺失或测试被跳过时 CI 失败。STATUS 只描述当前状态，篇幅有上限；历史记录原样搬到归档；交付流程不再要求把验证明细复写进 STATUS。

## 现状事实（实施前已核对）

- 本机 Guest 工具链：WSL Ubuntu 22.04.5，`/usr/bin/riscv64-linux-gnu-gcc`，版本 `riscv64-linux-gnu-gcc (Ubuntu 11.4.0-1ubuntu1~22.04) 11.4.0`，apt 包为 `gcc-riscv64-linux-gnu 4:11.2.0--1ubuntu1` / `gcc-11-riscv64-linux-gnu 11.4.0-1ubuntu1~22.04cross1`；Python 3.10.12，CMake 3.22.1。
- CMake 查找逻辑（`CMakeLists.txt:37-53`）：候选名依次为 `riscv64-unknown-elf-gcc`、`riscv32-unknown-elf-gcc`、`riscv64-linux-gnu-gcc`。设置了 `YAN_RISCV_PREFIX` 时，`<prefix>gcc` 插到最前；先 `unset(YAN_RISCV_GCC CACHE)` 再 `find_program`。找不到时**只是不注册**，不报错。
- Guest 编译使用 `-march=rv32im -mabi=ilp32 -nostdlib -nostartfiles -ffreestanding`，不依赖 libc/newlib，所以 linux-gnu 交叉 gcc 够用（本机即如此）。
- CI 当前矩阵：ubuntu-22.04 Debug+ASan、ubuntu-22.04 Release、windows-2022 Debug，三者都没有 `YAN_BUILD_TOOLS`。
- `yan_mcp`、`mcp_sdk`、`semantic_*` 等只在 `YAN_BUILD_SEMANTIC=ON` 时注册（`CMakeLists.txt:240-266, 310-399`），依赖 libcurl/yyjson/模型；`guest_note_blocks_host` 需要 `TARGET yan_mcp`。
- `MONITOR_REPORT.md` 被 `.gitignore`（`/MONITOR_REPORT.md`）忽略，不在 Git 中；外部监控 Agent 会周期性写入。AGENTS.md 规定它不得删除。
- 仓库内 30 多个文件按文件级链接引用 `STATUS.md`，没有 `STATUS.md#锚点` 形式的链接；但有文字形式的章节指向，例如 0011 的「见 STATUS 的验证一节」。

## SPEC

### R1：Linux CI 安装与本机同源的 RISC-V 工具链并启用 Guest 构建
当 PR 或 main 推送触发 CI 时，两个 ubuntu-22.04 任务先 `apt-get update`，再用 apt 安装 `gcc-riscv64-linux-gnu`（来源与本机 WSL 相同：Ubuntu 22.04 官方仓库），并以 `-DYAN_BUILD_TOOLS=ON -DYAN_RISCV_PREFIX=riscv64-linux-gnu-` 配置。这两个选项是新增的，原有的 `CMAKE_BUILD_TYPE`、`YAN_ENABLE_SANITIZERS` 保持不变。
- AC1.1：Given 一次 PR CI 运行，When 查看两个 Linux 任务日志，Then 都有 `riscv64-linux-gnu-gcc --version` 的输出，主版本为 11，并打印实际安装的包版本（`dpkg-query -W gcc-11-riscv64-linux-gnu`）。小版本和本机不一致时只记录，不判失败。
- AC1.2：Given 同一次运行，When 查看配置后 `build/CMakeCache.txt`，Then 含 `YAN_RISCV_GCC:FILEPATH=/usr/bin/riscv64-linux-gnu-gcc` 和 `YAN_BUILD_TOOLS:BOOL=ON`。

### R2：CI 运行的集合等于本机同配置的默认集合
当 Linux 任务执行 `ctest`（不加 `-R`/`-E`）时，注册并运行的测试集合与本机 WSL 用相同参数全新配置得到的 `ctest -N` 集合**逐名相同**。必须包含 `guest_*` 默认测试（例如 `guest_yanfs`、`guest_terminal`、`guest_runtime`、`guest_search`、`guest_note_blocks`、`guest_trap_env`、`guest_persistent_combined`）和既有默认的 `*_mutation_gate` 纯门禁。
- AC2.1：Given 本机 WSL 执行 `cmake -S . -B /tmp/ci-mirror-<cfg> -DCMAKE_BUILD_TYPE=<cfg> -DYAN_ENABLE_SANITIZERS=<s> -DYAN_BUILD_TOOLS=ON -DYAN_RISCV_PREFIX=riscv64-linux-gnu-`（Debug/ON 与 Release/OFF 各一次），When 在**同一提交**上把 `ctest -N` 的测试名排序后（外部路径选项保持默认空值），与对应 CI 任务日志中的测试名比较，Then 两份列表相同，且都含 `^guest_` 前缀的测试 ≥1 个。
- AC2.2：Given 同一次 CI 运行，When 看 ctest 汇总，Then 两个 Linux 任务均 `0 tests failed`，Skipped 为 0（见 R4）。
- AC2.3：交付记录写明两个 Linux 任务的实际注册数和 CI 耗时（只是记录，不构成性能保证）。

### R3：工具链缺失或 Guest 测试未注册时 CI 失败
当 Linux 任务配置后 `YAN_RISCV_GCC` 不是有效路径，或 `ctest -N -R '^guest_'` 列出的测试数为 0 时，该任务在运行测试之前失败，日志中打印一行写明原因（工具链未找到 / Guest 测试未注册）。检查只写在 `build.yml` 的 Linux 步骤里，不改 CMake。
- AC3.1：Given 一个临时负控分支（不合入），去掉 apt 安装步骤，并在日志中打印 `command -v` 对三个候选名的结果，确认全部不存在。When CI 运行，Then 两个 Linux 任务为失败状态，日志含上述原因行，且没有进入测试步骤。如果 runner 本身已预装某个候选，就在负控分支里把它从 PATH 中移除后再测。
- AC3.2：Given 正常分支，When CI 运行，Then 检查步骤通过。

### R4：Linux CI 中任何 SKIP 都判为失败
CTest 会把 `SKIP_RETURN_CODE 77` 记为通过。当 Linux 任务的 ctest 输出中出现任何 `Skipped` 测试时，任务失败并列出被跳过的测试名。实现方式：ctest 输出用 `tee` 写入日志，在 `set -o pipefail` 下保留 ctest 自身的退出码；然后 grep 日志中 CTest 汇总的 `(Skipped)` 行，有命中就打印并 `exit 1`；日志不存在也算失败。本规格**不设跳过白名单**：在 R2 的配置下，正当的 77（外部参考模型、riscv-tests、ACT4、Sail）都不会注册。
- AC4.1：Given 一个临时负控分支（不合入），在 `CMakeLists.txt` 末尾临时加 `add_test(NAME ci_skip_probe COMMAND ${CMAKE_COMMAND} -E false)` 和 `set_tests_properties(ci_skip_probe PROPERTIES SKIP_RETURN_CODE 1)`，When CI 运行，Then 两个 Linux 任务失败，日志列出 `ci_skip_probe`。该改动只存在于负控分支。
- AC4.2：Given 正常分支，Then 两个 Linux 任务的 Skipped 均为 0。

### R5：Windows 任务不变
- AC5.1：Given `build.yml` 的 diff，When 查看 windows-2022 矩阵项和它走的步骤，Then 配置参数、构建和测试命令与改动前相同；Windows CI 的注册数和改动前的 main 一致（交付记录写明数字）。

### R6：STATUS 拆为当前状态和历史归档
`docs/STATUS.md` 改写为只描述当前状态，**≤ 16 KiB（UTF-8 字节）**，只保留以下章节：
1. 当前阶段（最近一个阶段的状态，≤ 15 行）；
2. 已交付能力（每项一行：编号、能力、规格链接、合入 PR/commit；从现有「已完成」清单压缩而来）；
3. 覆盖边界与已知限制（现有「外部验证层」「覆盖边界」中**当前仍成立**的结论，以及 CI 覆盖范围，按本规格更新后的事实书写）；
4. 待定设计；
5. 下一步；
6. 指向 `docs/STATUS-history.md` 的一行说明。

历史归档采用**整份快照**：把改动前的 `docs/STATUS.md` 原样重命名为 `docs/STATUS-history.md`（字节不变，和 STATUS 在同一目录，所以相对链接 `specs/...` 不用改写），然后新写一份精简的 `docs/STATUS.md`。新 STATUS 和归档之间允许有内容重复；归档里不加文件头，说明写在新 STATUS 的第 6 节。之后每次替换「当前阶段」时，旧段落原样追加到 `STATUS-history.md` 末尾（R9）。
- AC6.1：`wc -c docs/STATUS.md` ≤ 16384。
- AC6.2（不丢内容）：设 `OLD` 为实施时 main 上的 `docs/STATUS.md`。在提交②的重命名提交上，`sha256sum docs/STATUS-history.md` 等于 `OLD` 的 SHA-256；`git log --follow docs/STATUS-history.md` 能追到原 STATUS 的历史。
- AC6.3：新 STATUS 只含 R6 列出的六个章节（检查二级标题列表）；新 STATUS 中写成当前事实的每一条，都能在归档或当前代码/CI 中找到依据。dsh 审核时逐条核对。

### R7：MONITOR_REPORT 归档，保留通道
把 `MONITOR_REPORT.md` 原样复制到归档文件（文件名带日期时间，例如 `MONITOR_REPORT-20261020-1530.md`，不覆盖已有归档），重置前再算一次原文件的哈希，确认和归档一致；不一致说明监控 Agent 中途写入了，重新复制。然后把原文件改为只含通道说明头（来源、用途、指向归档的一行），文件保留，外部监控 Agent 继续写入同一路径。
- AC7.1：Given 归档完成，When 计算归档文件和归档前 `MONITOR_REPORT.md` 的 SHA-256，Then 两者相同。
- AC7.2：`MONITOR_REPORT.md` 仍存在，≤ 2 KiB，含指向归档的路径。
- AC7.3：归档位置按「开放问题 1」的裁定执行；默认方案下 `git status` 不因归档出现新的跟踪文件。

### R8：引用修正
新 STATUS 不再包含的章节，如果在其它文件里被按名称引用（例如「见 STATUS 的验证一节」「见 STATUS『0032 当前集成默认回归与Guest范围』」），引用要改为指向 `STATUS-history.md`（或指向新 STATUS 中保留的对应章节）。这只是机械修正，只允许改链接目标和章节指向这几个字，不改其它文字。
- AC8.1：在 `README.md`、`AGENTS.md`、`CONTRIBUTING.md`、`docs/specs/*.md` 中 grep `STATUS`，把文件级链接以外的所有命中（文字章节指向、简称，例如「STATUS 的验证一节」）逐条列表。每条结论只能是两种之一：所指内容仍在新 STATUS 中；或已改为指向 `STATUS-history.md`。
- AC8.2：本规格改动或新建的文件中，所有 Markdown 相对链接都能解析到存在的文件。用一次性只读脚本检查，交付时附输出；未改动文件里的旧失效链接不在范围内。
- AC8.3：被改的历史规格文件，`git diff` 中只出现链接目标/章节指向的变化。

### R9：交付流程同步，降低文档同步成本
修改 `AGENTS.md` 和 `CONTRIBUTING.md` 里与 STATUS/MONITOR 相关的流程条款，使每次阶段交付的文档动作固定为以下四件：
1. **阶段验证明细**（注册数、通过数、耗时、审核轮次、证据日志路径）只写一处：该阶段规格的「VERIFY 执行记录」节（既有做法，例见 0025），并在 PR 描述里摘要。STATUS、README、CONTRIBUTING **不再复写**这些计数。
2. STATUS 只做三处更新：替换「当前阶段」段；在「已交付能力」加一行；覆盖边界有变化时改对应条目。
3. 开始新阶段、替换「当前阶段」时，旧段落原样追加到 `STATUS-history.md` 末尾。
4. 阶段合入 main 后，总审核按 R7 的方式归档 MONITOR_REPORT 并重置为通道头。

AGENTS.md 中「只有总审核可改文档」「dsh 独立对齐审核」「监控结论须核实」三项原则**保留**，只把它们的检查范围相应缩到上述位置。

同一流程提交中一并落地以下四项（内容依据团队规范 `yanos-team-playbook.md` v2 的 §1 与附录，闫博已同意并入 0034）：
5. **AGENTS.md 作为唯一入口**：在现有 AGENTS.md 上增补团队规范 §1 的通用规则摘要（唯一事实来源、spec 头四字段、两条通道、PR 与日志、审查闭环、对外操作权限、完成定义），以及 lean-code、intent-to-spec、spec-driven-impl、spec-alignment-review 的关键规则各数行。不变原则写在这里，spec 只引用。保持精简：逐行自问「删掉它，机器人会不会犯错？」，能链接的不重抄。
6. **`.github/pull_request_template.md`**：新建，内容照团队规范 §1「PR 模板」原文。
7. **`docs/specs/README.md`**：新建索引表，列为「编号 | 标题 | 状态 | supersedes | 被谁取代」，覆盖 0001–0034 全部现存 spec（0002 不存在，注明「未入库，见 PR #2，已取代」）。旧 spec 没有状态字段的，按事实填 Implemented；不回补旧 spec 文件本身的头字段。
8. **`docs/decisions/0001-roadmap-A-D-B.md`**：新建，内容照团队规范附录原文。
- AC9.4：Given 修改后的 AGENTS.md，When 逐项对照团队规范 §1 的七个小节，Then 每个小节在 AGENTS.md 中都有对应条目或指向权威文件的链接；AGENTS.md 中不出现与本规格 R9 第 1–4 件相矛盾的条款。
- AC9.5：`.github/pull_request_template.md` 与团队规范 §1 PR 模板逐字一致（`diff` 为空，忽略行尾空白）。
- AC9.6：`docs/specs/README.md` 的表格行数等于 `docs/specs/` 下 spec 文件数加 1（0002 注释行）；本规格（0034）一行状态为 Approved，合入后随 PR 改为 Implemented。
- AC9.7：`docs/decisions/0001-roadmap-A-D-B.md` 与团队规范附录逐字一致；本规格头部的决策链接能解析到该文件（并入 AC8.2 的链接检查）。
- AC9.1：Given 修改后的 AGENTS.md 和 CONTRIBUTING.md，When grep「STATUS」「MONITOR_REPORT」，Then 每处命中都与上述四件一致，不再出现要求在 STATUS 或 CONTRIBUTING 记录各阶段计数的条款。
- AC9.2：CONTRIBUTING.md 提交前清单第 3、4 项（现第 106–107 行）按上述规则改写；其余清单项不变（以 diff 为准）。
- AC9.3：README.md 中关于 CI 覆盖的描述（如有）、新 STATUS 的 CI 条目，与 R1–R4 的实际行为一致。

## Non-goals

- 不改任何功能代码（`src/`、`os/`、`apps/`、`host/`、`tools/`）和测试代码（`tests/`），也不改 `CMakeLists.txt`。
- 不重构测试框架，不改测试名、超时、SKIP 语义和注册条件。
- 不把外部验证层搬进 CI：NEMU 差分（`difftest_*`）、官方 riscv-tests、Sail 签名、ACT4 都不进。
- 不在 CI 开启 `YAN_ENABLE_MUTATION_TESTS`（实际变异 17 组本机约 455 s，加上构建成本，不算低）；既有默认的 `*_mutation_gate` 纯门禁照常运行。
- 不在 CI 开启 `YAN_BUILD_SEMANTIC`、不装 libcurl/yyjson/模型/官方 MCP SDK，所以 `yan_mcp`、`mcp_*`、`semantic_*`、`guest_note_blocks_host` 不进 CI。
- Windows 任务不加 Guest 测试，也不在 Windows 上装 RISC-V 工具链。
- 不加 apt/构建缓存、不加并行矩阵、不加重试、不固定 apt 包小版本（ubuntu-22.04 镜像源与本机同源）。只有当实测 Linux 任务超过 30 分钟时，才回报项目所有者另行决定，不在本规格内自行优化。
- 不重写、不精简、不改写历史文档内容；归档是搬移。不修正历史段落里的旧结论或旧计数（包括 STATUS 中已经过时的 `CMakeLists.txt:101-102` 这类行号引用）。
- CONTRIBUTING.md 里既有的各阶段验证计数段落（现第 70–98 行）本规格不处理，见开放问题 3。
- 不新增文档生成脚本、不加 Markdown lint 到 CI、不加 pre-commit 钩子。AC 里的检查脚本是一次性的验证手段，不入库。
- 不改 `.github/workflows` 以外的 CI 相关文件，不新增 workflow 文件。（`.github/pull_request_template.md` 不属于 CI 文件，按 R9 第 6 件新建。）

## 约束

- 文档修改按 AGENTS.md 由**总审核**角色执行，dsh 独立做只读对齐审核。R9 修改流程本身属于语义性变更，本规格获项目所有者批准即视为该变更已批准。
- 在 A 合入 main 之后，从最新 main 切短期分支实施。A 会更新 STATUS，AC6.2 的 `OLD` 取实施时 main 上的版本。
- PR 与提交拆分（闫博 2026-10-19 裁定拆成两个 PR）：**PR-1 CI**，含 R1–R5，一个提交，两次负控运行在不合入的临时分支上做；**PR-2 文档**，含 R6–R9，在 PR-1 合入后从最新 main 切分支，提交依次为：②a 把 STATUS 重命名为 STATUS-history，②b 新建精简 STATUS（R6），③ 引用修正（R8），④ 流程条款（R9，含 AGENTS.md 唯一入口、PR 模板、spec 索引、决策记录 0001）。②和③分开提交，便于用 diff 核对「只搬不改」。PR-2 新 STATUS 的 CI 条目按 PR-1 合入后的实际行为书写（AC9.3）。MONITOR 归档（R7）默认不产生提交。
- 两天内完成：CI 部分含两次负控运行，约 0.5–1 天；文档部分约 1 天。超时就回报，不扩大范围。

## 假设

- [假设] GitHub 的 ubuntu-22.04 runner 能通过 apt 装到与本机相同的 `gcc-riscv64-linux-gnu`（同为 jammy 仓库），bash、`timeout`、`nm`、Python 3 已预装，Unity 继续由 FetchContent 拉取。
- [假设] 两个 Linux 任务都跑 Guest（ASan 和 Release 各一遍）。本机 ASan 默认约 248 s、Release 约 333 s（含 SEMANTIC 部分），CI 不含 SEMANTIC，预计在 30 分钟内。
- [假设] 16 KiB 上限足够放下六节内容；已交付能力按每项一行计约 33 行。
- [假设] AC6.2 用整份快照加哈希证明「不丢、不改」，所以不需要逐行搬移证明。代价是归档里会包含当时的「当前状态」段，和新 STATUS 有重复，这是可以接受的。

## 开放问题（已裁定，2026-10-19）

裁定：四项均按默认方案执行。1 归档到仓库外 `E:\YanOS-archive\monitor\`，不入 Git；2 ASan 与 Release 两个 Linux 任务都跑 Guest；3 CONTRIBUTING 既有计数不搬，只停止新增；4 STATUS 上限 16 KiB。以下保留原问题供追溯。

1. **MONITOR_REPORT 归档放哪里？** 它被 gitignore，是监控通道的工作状态。默认方案：放仓库外的本机目录 `E:\YanOS-archive\monitor\MONITOR_REPORT-<日期>.md`，不入 Git。备选：入库到 `docs/history/`，这样会把约 300KB 的监控与回应记录变成公开仓库内容（仓库是 public）。
2. **两个 Linux 任务都跑 Guest，还是只在 Release 跑？** 默认两个都跑：ASan 能抓 Host 侧执行器问题，代价是 CI 时长大约翻倍。
3. **CONTRIBUTING.md 第 70–98 行的各阶段验证计数**要不要一并原样搬到 `STATUS-history.md`？默认不搬，只停止新增；搬的话原样追加到归档末尾，AC 为：CONTRIBUTING 删除的行按顺序逐字出现在归档的追加段里。
4. **STATUS 上限 16 KiB 可以吗？** 当前约 157KB。
