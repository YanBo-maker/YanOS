# 开发准则

每次修改解决一个具体问题。优先保持代码清楚、行为可验证，让读者能够沿着提交历史理解系统的演进。

## 开发流程

任务按 INTENTION → SPEC → IMPLE PLAN → IMPLE → VERIFY 推进。小任务可以简写在同一份记录中。文档与规格的修改分工见 `AGENTS.md` 的「文档与规格的修改权」：`README.md`、`docs/STATUS.md` 与 `docs/specs/*` 只由总审核角色落笔，其他人提交变更请求。

### INTENTION

说明当前问题、使用需求、为何现在需要，以及本次范围。学习需求应落实为可通过实验回答的问题，例如观察一次异常如何改变执行流。未来设想记录在待办中，开发顺序依据实际依赖调整。

### SPEC

定义外部行为、接口、状态、数据格式、错误处理和不变量。涉及 MMIO、CSR 或 ABI 时，给出地址与布局，并附具体例子和验证用例。

RISC-V 行为以官方规范为依据。首次实现前记录规范版本、章节和链接，支持范围随实现进度更新。

### IMPLE PLAN

先列出修改文件、受影响模块、最小实现和测试计划，再开始编码。

### IMPLE

ISA 行为遵循 Spec → Test → Implementation：先写用例并确认预期失败，再实现功能，让测试通过。配套代码与测试组成完整提交。

### VERIFY

对照规格检查行为，运行相关测试和回归检查，记录实际结果、尚未验证的部分及已知限制。审查 CPU / Bus / Device、Host / Guest 和 OS / Hardware 的边界。

用一个实际输入说明执行过程，并给出阅读入口。若验证发现规格存在问题，应记录证据、修订设计后继续实现。

## 实现约定

- Host 核心使用 C17；Guest 使用 C 和必要的 RISC-V 汇编。
- 明确定宽整数、字节序、溢出和对齐行为，避免依赖 C 的未定义行为。
- CPU 通过 Bus 访问内存与 MMIO。终端和文件操作由设备或 Host 适配层处理。
- 先验证功能，再依据工作负载和性能测量优化。
- 注释说明设计原因、规范依据和容易误读的边界条件。
- 工具链、构建工具与规范版本在对应任务中确定。

## 分支与审查

采用 [GitHub Flow](https://docs.github.com/en/get-started/using-github/github-flow)：在短期任务分支上开发，通过 Pull Request（PR，合并请求）审查，再合入 `main`。

分支名描述具体任务，例如 `feat/cpu-state`、`fix/jalr-alignment`。一个 PR 对应一个可审查能力，可以包含数条提交。PR 说明问题、最终行为、验证结果和剩余限制。

功能合并前由项目维护者审查。`main` 保持已验证状态，默认以 merge commit 合并，保留有助于理解实现过程的提交。

已发布历史保持稳定，后续修正使用新提交；撤销已共享的变化使用 `git revert`。未发布的个人提交可在审查前整理。处理冲突时保留已有工作。

### 合并前检查清单

1. 默认套件全绿：`cmake --build build --parallel && ctest --test-dir build --output-on-failure`。若配置已开启变异，用 `ctest --test-dir build -E '_mutation$' --output-on-failure` 运行默认套件，保留默认的 `search_mutation_gate`、`editor_mutation_gate`、`yanfs_mutation_gate` 和 `terminal_mutation_gate` 判据负控。这里的 `build` 指**你自己的构建目录**：本机上 `build/` 是各构建目录的容器（`build/agent-*` 等，没有统一的根配置），所以要先 `cmake -S . -B build/<你的目录>` 配置它，否则 `cmake --build build` 会直接报 `Error: could not load cache`（exit 1）。
2. 变异检查显式跑一次：`cmake -S . -B build -DYAN_BUILD_TOOLS=ON -DYAN_ENABLE_MUTATION_TESTS=ON …` 后运行 `ctest --test-dir build -R '_mutation$'`。开关下实际变异套件在 Linux、工具与依赖齐备时是**十一组**：`search_mutation`、`editor_mutation`、`terminal_mutation`、`yanfs_mutation`、`persistent_block_mutation`、`persistent_combined_mutation`、`guest_console_mutation`、`device_mutation`、`guest_runtime_mutation`、`guest_block_mutation`、`guest_m2a_combined_mutation`；默认四个 `*_mutation_gate` 检查分类器，不算实际变异组。另有两组 `difftest_mutation` / `difftest_ref_mutation` **只在检出 NEMU 参考模型时注册**，本地缺参考模型时不在名单里。每一组都必须 **0 存活**，而且**检出只认指定owner及具体断言失败**：无关case或同case的无关断言不计检出；完整footer、逐项PASS/FAIL/IGNORE计数与退出码必须一致。timeout、信号死亡（139 等）、sanitizer 报告、fixture abort、构建失败**一律归 harness error，不计检出**（这两条判据是 2026-09-20 两次门禁缺陷换来的：一次是只加一行 `fputs` 的零行为变化变异体被判成检出，一次是纯崩溃与 fixture abort 被计成检出）。**这一步不能省**：变异检查是"测试真的有检测力"的唯一证据，默认不注册只是因为它慢。
3. 文档自洽核对：由总审核角色核对状态行、规格索引、计数与链接。
4. **新增或改动被忽略扩展名的工件时，确认它真的能进提交**：判据是 `git add --dry-run <路径>` 是否列出该文件，**不是** `git check-ignore` 的返回码——命中取反规则时它也返回 0。仓库已经踩过一次：golden 基准的 `.jsonl` / `.hex` 被"验证产物不提交"的规则吃掉，基准进不了仓库，测试会在别的机器上静默 SKIP。

变异检查默认关闭，合并前显式运行。0025知识检索本机验收（2026-10-07）：Linux默认55组、开启实际变异后66组；Release55/55（32.33 s）、ASan/UBSan55/55（50.93 s）、Windows MSVC32/32（5.70 s），十一组实际变异11/11（142.77 s），均0失败、0SKIP。Search17个缺陷命中指定owner和具体断言；真实no-effect、wrong-owner与同owner wrong-assertion控制均已复验，0存活、0harness error。四个默认mutation puregate及Guest脚本门禁保留在默认套件中，本机通过不代替远端CI或维护者审查。证据与覆盖边界见 [STATUS](docs/STATUS.md)。

历史0024重命名与复制本机验收：Linux默认49组、开启实际变异后59组；Release49/49（19.29 s）、ASan/UBSan49/49（36.98 s），十组实际变异10/10（97.01 s，-j3），Windows MSVC默认29/29（2.27 s），均0失败、0SKIP。FS19个与终端24个实际缺陷均命中指定owner及具体断言，0存活、0harness error。新默认Guest文件管理及故障验收由Linux工具配置注册，Windows计数不能直接与其比较；边界与证据见 [STATUS](docs/STATUS.md)。`-E '_mutation$'`保留默认puregate，`-R '_mutation$'`只选十组实际变异。

历史Git整合测量：2026-10-05 Git整合与兼容复测：Windows MSVC Debug本机29/29（4.02 s）；Linux Release默认47/47（17.67 s）、ASan/UBSan默认47/47（31.65 s）、十组实际变异10/10（88.14 s，均-j3），0失败、0SKIP。Linux注册仍为默认47组、开实际变异57组，Windows本机配置不包含Linux工具与真实Guest验收；本机通过不代替远端CI或PR审核。命令与证据见 [STATUS](docs/STATUS.md)。

编辑器交付测量：2026-10-05多行编辑配置注册默认 **47组**、开启实际变异后 **57组**；Release默认 **47/47（28.88 s）**，Debug ASan/UBSan默认 **47/47（48.86 s）**。十组实际变异一次运行 **10/10（130.76 s，-j3）**；编辑器11个非等价缺陷均命中指定owner及具体断言，0存活、0harness error。三个默认puregate核分类纪律，原生同owner无关断言控制已补齐。以上均无失败、无SKIP，命令与证据见 [STATUS](docs/STATUS.md)。

历史测量：2026-10-04终端文件操作配置注册默认 **43组**、开启实际变异后 **52组**；Release默认 **43/43（50.40 s）**，Debug ASan/UBSan默认 **43/43（77.88 s）**。九组实际变异一次运行 **9/9（86.57 s，-j3）**；新终端组20个非等价缺陷均命中具名具体断言，0存活、0harness error。以上均无失败、无SKIP，命令与证据见 [STATUS](docs/STATUS.md)。

历史测量：YanFS 于 2026-10-03 的 Linux 工具配置注册默认 **37 组**、开启变异后 **45 组**；Release 默认 **37/37（34.67 s）**、Debug ASan/UBSan 默认 **37/37（56.59 s）**。八组实际变异分开执行：原七组 **7/7（165.16 s）**，YanFS 一组 **1/1（7.18 s）**，十个非等价缺陷全部命中 owner 断言；均无失败、无 SKIP，不拼接为一次八组的耗时。

同日 0020 的历史配置为默认 **32 组**、开启变异后 **39 组**；Release **32/32（21.10 s）**、ASan/UBSan **32/32（22.99 s）**、七组变异 **7/7（157.64 s）**。配置、证据与历史测量见 [STATUS](docs/STATUS.md)。每次提交前重新测量并记录配置、命令、通过与跳过数量，历史耗时仅作参考。

### 提交前总审核检查清单

由总审核角色在**每次提交前**逐条执行并留下结论；勾不动的条目要先修文档再提交。每一项都注明依据，避免"靠记性"。

- [ ] **1 文档自洽**：状态行、规格索引、计数（套件数 / 用例数 / 规格数）、链接、行号引用，以及"已完成 / 进行中 / 待裁定"三类口径，全部与**本轮实测**一致。（依据：`AGENTS.md`「文档与规格的修改权」第四条。）
- [ ] **2 历史结论未被误改**：历史阶段规格的结论仍在、被取代的段落仍保留原文并带取代标注。（依据：同上第六条；0005–0008 的标记与 0012 的取代标注。）
- [ ] **3 规格修改有授权**：锁定规格的每一处改动都能指向项目所有者的批准，并已在 `docs/STATUS.md` 留下「规格锁定后被修正」的说明（错误原文、证据、批准依据、归属）。（依据：`AGENTS.md` 第三条；0018 规则 2 / 0019 C 组第 4 条的先例。）
- [ ] **4 `MONITOR_REPORT.md` 已读并回应**：读取 → **逐条用命令核实** → 回应写回同一文件（署名、时间、证据）。未核实的结论不得进 `README.md` / `docs/STATUS.md` / 规格；`mut-*` 或仓库外副本下的失败日志是变异判据，不是回归结论。（依据：`AGENTS.md`「外部监控通道」四条。）
- [ ] **5 计数现测**：默认套件数与变异开关下的套件数、以及耗时，都要**当场跑一遍再写**，不得沿用旧数字。（依据：2026-09-20 的事故——`CONTRIBUTING` 里的 25 / 27 在 M2a 注册两个套件后就过期了。）
- [ ] **6 门禁判据纪律**：变异检查只认断言失败；timeout / 信号 / sanitizer / 构建失败归 harness error；**"存活 0"只有在门禁本身被复验过之后才可引用**。（依据：本轮两次门禁缺陷 + 门禁复验记录。）
- [ ] **7 `SKIP 77` 的语义**：`SKIP_RETURN_CODE 77` 不得由"被测实现 / 测试程序 / 基线缺失"可达，只能表示缺仓库外依赖（工具链、Unity、参考模型）。移动被测文件后必须硬失败。（依据：实测把 `os/block.c` 移走后两个脚本退 77、CTest 报"100% passed"；该洞自 M1 起存在。）
- [ ] **8 提交切分**：`git status` 每一项恰好归属一条提交、文档与其所述改动**同批**（不留"代码在第 5 条、文档在第 10 条"的中间态）、每条提交都不让树变红。（依据：`CONTRIBUTING.md`「提交粒度」与 `AGENTS.md`「在短期任务分支开发…配套实现与测试一起验证」。）
- [ ] **9 越权检查**：本轮是否有 Agent 改了不属于它的文件；未经审核的文档改动按「未记录的行为变化」出裁定（保留 / 回退 / 改写）。（依据：`AGENTS.md` 第五条；2026-09-20 批次 B 的先例。）

## 提交粒度

一条提交对应一个逻辑变化，能够独立审查和回退。新功能与配套测试一起提交，无关重构和格式调整另行提交。

例如，实现 ADDI 的提交应包含指令行为及负立即数、回绕等边界测试。头文件、实现和测试可以属于同一提交。

标题采用 `类型(范围): 中文说明`，范围可省略：

```text
docs(spec): 定义 CPU 初始状态与不变量
feat(cpu): 添加寄存器状态并验证 x0 不可修改
feat(memory): 添加 RAM 字节读写与边界检查
feat(cpu): 实现 ADDI 并覆盖负立即数和回绕
```

常用类型包括 `docs`、`feat`、`fix`、`test`、`refactor`、`build`、`ci` 和 `chore`。复杂修改在正文中说明原因、行为变化和验证结果。

提交前检查工作区与暂存差异，只纳入当前任务的修改。运行 `git diff --cached --check` 及相关验证，确认生成文件和运行数据已排除。

## 测试与版本

文档修改检查链接、内容一致性和差异。第一个可运行 C 模块落地时建立可复现的构建与测试，并接入 GitHub Actions。测试重点覆盖外部行为、边界条件和关键不变量。

达到明确的阶段验收目标后再创建版本标签。构建命令与验证方式随对应能力一同记录。
