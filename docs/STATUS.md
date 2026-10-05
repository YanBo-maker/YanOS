# 项目状态

当前阶段：多行文本编辑整体实现与验证完成，[0023](specs/0023-multiline-text-editor.md) 于2026-10-05 09:22 +08:00经项目所有者批准并锁定；交付与理解程度待用户审查。当前分支为 `codex/multiline-editor`，基线为read状态优先修复 `1daae9f`。

- 规格批准：edit NAME、a/r/d/p/w/q、16KiB静态草稿、合法UTF-8文本与已有TAB、保留LF/CRLF/无末LF、一次save和取消不写盘；普通保存错误留稿，FS或输出fatal沿0022。项目所有者已认可形成规格并继续实现。
- 实现：公共编辑核心、应用模式路由、真实Guest与故障验收已完成；原八命令与文件系统格式保持。0023记录公开接口、确认token与新增应用原因码18–21。
- 验证：最终Release默认47/47（28.88 s）、ASan/UBSan默认47/47（48.86 s），十组实际变异10/10（130.76 s，均-j3），0失败、0SKIP。编辑核心64例、真实Guest15项与故障33项通过；11个编辑器变异均命中指定owner及具体断言。细节与限制见本页「验证」。
- 用户审查：具体行为已批准，实现交付与理解程度待项目所有者审查。

### 已交付终端与状态优先修复

终端八命令在 `a8e1026` 交付；2026-10-05的 `1daae9f` 修复read先状态后参数，符合锁定0021，没有修改规格语义。修复后Release默认43/43（17.16 s）、ASan/UBSan默认43/43（33.15 s）、九组实际变异9/9（87.45 s，均-j3），0失败、0SKIP。旧交付、修前及修后测量保留在「验证」，交付审查与学习仍由项目所有者判断。

### YanFS 交付基线

YanFS 初版整体实现与验证完成，交付待用户审查。[0021](specs/0021-yanfs.md) 于2026-10-03经项目所有者批准并锁定；格式化、挂载、列目录、读取、创建、整文件覆盖、删除和空间回收按同一格式交付。该阶段分支为 `codex/yanfs`，提交 `24b80bb`；持久化后端此前交付在 `d2efd72`。本轮批准终端方案不改变YanFS交付审查状态。

- 规格批准：单根目录最多 63 文件，连续 extent 分配，覆盖先写新数据再发布目录；唯一元数据块写失败可能导致拒挂，不承诺断电一致性。
- 实现：FS 核心、Linux mkfs、Guest 块适配与整体用例已完成；名称与输出边界、失败后的缓存发布、完整响应消费等审查项已修复。
- 验证：最终 Release 默认 37/37（34.67 s）、ASan/UBSan 默认 37/37（56.59 s）；八组实际变异分别通过，详细命令与证据见本页「验证」。下文 0020 和早期阶段的结果保留为历史，不混作本轮文件系统证据。
- 用户审查：整体方案已认可，实现交付与理解程度待项目所有者审查。

## 已完成

- RAM 生命周期、小端读写、宽度与范围检查。
- Bus 的单 RAM 映射、对齐检查及错误信息。
- Machine 初始化、整机复位和内存镜像装载。
- CPU 的 32 个通用寄存器、x0 语义、PC 和独立复位。
- CPU 经 Bus 读取 32 位机器码；取指保留 PC、寄存器和内存。
- 21 条 RV32I 整数计算指令的单步执行，支持清单见 [整数计算规格](specs/0006-integer-alu.md)。
- 六条条件分支与 JAL、JALR，支持目标对齐检查和返回地址写回，见 [控制转移规格](specs/0007-control-flow.md)。
- LB、LBU、LH、LHU、LW、SB、SH、SW，经 Bus 访问 RAM，见 [Load / Store 规格](specs/0008-load-store.md)。
- 符号扩展、32 位回绕、移位量屏蔽及编码拒绝后的状态保持。
- 同步异常进入、mepc / mcause / mtval、MIE / MPIE 状态保存与 MRET 返回。
- 六种 CSR 指令与机器模式 CSR 集合；ECALL、EBREAK，以及 FENCE 与 FENCE.I（单 hart、每次取指重读 RAM，两者都只推进 PC）。
- 机器模式中断：`mie` / `mip` 的 MSIP、MTIP、MEIP 三位，全局 `mstatus.MIE`，中断进入（mcause 最高位置 1、mepc 指向未执行指令、mtval 为 0）与 MRET 返回，原因码优先级 MEIP > MTIP > MSIP，见 [机器模式中断规格](specs/0012-machine-interrupts.md)。
- 单 hart CLINT：标准基址 `0x02000000`，msip、mtimecmp、mtime 的 32 位半字读写；mtime 只由 Host 的离散 tick 推进，不读宿主墙上时钟。
- 单 context M-mode PLIC：标准基址 `0x0c000000`，priority、pending、enable、threshold、claim / complete，source 0 保留，只有 pending、enabled 且 priority 大于 threshold 的源拉高 MEIP。网关按电平语义工作：设备线 asserted 且该源未被 claim 时置 pending，源被 claim 后在 complete 之前不再重新 pending，complete 时线仍 asserted 才重新 pending，线 deassert 则撤销尚未 claim 的请求；平台用 `yan_plic_set_level(plic, source, asserted)` 双向驱动设备线，旧的 `yan_plic_raise` 已移除，见 [PLIC 网关与设备中断线规格](specs/0016-plic-gateway-and-irq-lines.md)。网关另经独立验证：对抗用例加 40000 步随机差分参考模型，唯一存活的变异由新增的边界用例闭合。
- UART 字符设备（[0015](specs/0015-uart-device.md) v3）：标准基址 `0x10000000`，`TXDATA`、`RXDATA`、`STATUS`、`CONTROL`、`IRQ_STATUS` 五个寄存器。`STATUS` 的三位是 `TX_READY`、`RX_READY`、`CONNECTED`，`TX_READY` 的语义是"此刻写 `TXDATA` 是否会被接受"，与写入路径用的是同一个判据，因此不会出现"报告就绪却拒绝写入"；没有 backend 时三位全 0，两个方向都以 `YAN_UNAVAILABLE` 拒绝（`include/yan/status.h` 为此新增该状态码），驱动据此可以 headless 运行而不是一直空转。单字节接收缓冲，`IRQ_STATUS` 的 `RX_IRQ_PENDING` 写 1 清除，接收线经平台映射到 PLIC source 2。Host 侧 backend 是 `YanUartTerminal`（`tx_ready` / `tx_write` 两个回调），`yan_uart_push_rx` 注入接收字节；Guest 侧旧控制台驱动保留在 `os/console.c`，中断读行与末字节输出检查由0022新增层承担。早期分支交付记录保留在下方历史段。
- Host 传输通道（[0014](specs/0014-host-transport-channel.md)）：标准基址 `0x10001000`，标识寄存器、两个共享 `RING_BASE` 的字节环、门铃与中断寄存器。`yan_transport_configure` 校验环的大小与落位，`G2H_TAIL` / `H2G_HEAD` 由 Host 驱动；Host 侧 `yan_transport_host_consume` / `host_publish` 在未配置时返回 `YAN_UNAVAILABLE`。越界不由 `DOORBELL` 判定，而由 Host 记账 API 在搬运时判定并置 `STATUS.OVERFLOW_DETECTED`，`DOORBELL` 只做通知。Host→Guest 中断线经平台映射到 PLIC source 1。
- `os/` 层与控制台：`os/platform.h` 重述 Guest 侧 UART 与 PLIC 源号常量，`os/console.h` 冻结控制台接口（`connected` / `putc` / `puts` / `getline`，没有阻塞入口）。0022新增 `line`、`terminal` 与 `shell`，生产 `apps/` 依赖 `os/`；两者均不引用 `tests/`，测试可以引用生产层。2026-10-05当前include扫描与生产ELF链接输入核对无测试依赖；旧控制台约束见 [0017](specs/0017-console-and-os-layout.md)，新增层见 [0022](specs/0022-terminal-file-operations.md)。
- 最小 M-mode Guest 环境：`tests/guest/mtrap_entry.S` 提供入口与 Direct 陷阱向量（固定 20 字帧、保存调用者保存寄存器与被中断的 `sp`），`mtrap.c` 提供统一调度与 CSR 访问，`guest_devices.h` 提供 Guest 可见的 CLINT / PLIC 地址与访问封装，见 [Guest 启动与统一 trap 环境规格](specs/0013-guest-trap-environment.md)。
- `yan_run` 已经改用 `yan_machine_init_with` 组装 `YanMachine` 并逐条调用 `yan_machine_step`：四个设备窗口（CLINT、PLIC、UART、transport）都解码，设备中断线由平台每步采样进 PLIC，`mtime` 每条指令推进一次。UART 接收中断因此在该执行器下可达（`guest_terminal` 的接收中断场景由红转绿），transport 窗口的 `MAGIC` 也能读到；`-h` / `--help` 打印用法到标准输出并返回 0；这两种拼写、未知选项与无参数的退出码由 `guest_terminal` 的四条 CLI 断言固定，它们在缺交叉工具链时也先于 SKIP 门槛执行（`tests/guest/run_terminal.sh`）。`yan_difftest` 保持不连接设备。
- RV32M 的 MUL、MULH、MULHSU、MULHU、DIV、DIVU、REM、REMU，包含除零和有符号溢出规则。
- 面向外部验证模型的只读 CPU 架构状态快照接口。
- RV32 ELF32 镜像加载与符号表解析；`tohost` 与签名区都由符号决定，不依赖固定地址。
- 只读的差分比较模块，输出出错指令、首个不同寄存器与两侧完整寄存器表。
- `yan_run` Guest 执行器：最大步数、`tohost` 退出、签名区导出、JSONL 架构状态记录，以及区分「通过 / Guest 报错 / 未终止 / Host 错误 / 已导出签名」的退出码。
- `yan_difftest` 差分执行器：加载外部参考模型，逐条比较架构状态，结束时比较整段 RAM。
- `yan_gen` 随机 RV32IM 指令流生成器（给定种子确定输出），以及 5 个手写 Guest 用例。
- C17 / CMake 构建、Unity 单元测试、CTest 及 GitHub Actions。

详细分层、覆盖边界与退出码见 [CPU 验证规格](specs/0011-cpu-validation.md)；该规格的「CTest 汇总」记的是 M1a 阶段的 21 / 22 / 28 / 29 组（已在规格里标注为当时快照）。当前0023配置默认47组、开实际变异后57组；0022的43/52组与0020于2026-10-03的32/39组保留在本页历史测量。该规格另记下「外部验证层的测试只在 `YAN_BUILD_TOOLS=ON` 时注册」这条前提；各组的通过情况以本页「验证」一节为准。

## 验证

### 多行文本编辑（2026-10-05）

最终配置使用Linux/WSL工具链，Release开启实际变异，Debug开启ASan/UBSan且关闭实际变异。Release注册57组，其中默认47组、实际变异10组；默认三个 `*_mutation_gate` 随回归运行，不被 `_mutation$` 过滤。构建日志 `build/editor-final-release-build.log` 与 `build/editor-final-asan-build.log` 均完成，没有warning/error匹配。

- Release默认：`ctest --test-dir build/editor-release -E '_mutation$' -j3 --output-on-failure`，47/47，28.88 s。
- ASan/UBSan默认：`ctest --test-dir build/editor-asan -j3 --output-on-failure`，47/47，48.86 s。
- 十组实际变异：`ctest --test-dir build/editor-release -R '_mutation$' -j3 --output-on-failure`，10/10，130.76 s。

上述均0失败、0SKIP；保存日志分别为 `build/editor-final-release-tests.log`、`build/editor-final-asan-tests.log`、`build/editor-final-mutations.log`，实际变异逐项证据保存在 `build/editor-final-mutations-detail.log`。11个编辑器变异均命中唯一指定owner和具体断言，0存活、0harness error，涵盖LF、CRLF、完整载入、容量拒绝、q无写、修改不立即写、固定create/replace、提交后输出失败以及名称内嵌NUL。真实no-effect与wrong-owner控制先运行；puregate拒绝同owner无关断言、marker只出现在stderr/PASS、缺metadata、重复记录及异常footer。

全量结束后补强editor puregate的PASS-marker构造：先以永久检查复现fixture丢掉marker，再显式放入PASS记录；FS已有此记录，新增同类构造检查。两组puregate后验2/2（1.14 s），日志 `build/editor-final-pure-pass-marker.log`，总审核另行运行确认。只修改控制输入，不改生产、实际变异或指定断言；以上全量测量保留为本轮真实执行，不伪称重跑耗时。

核心64例覆盖文本/EOL、16384容量原子性、输入/地址边界、重入、保存与输出故障。真实生产Guest15项用独立struct/zlib和完整镜像oracle核新建、修改、取消、空间不足、跨块中文及同进程连续会话；新进程cat读回保存字节。33项故障含两个健康baseline，分别核首/中/末输出字节、加载/保存IO以及两块加载响应错tag；应用原因19/17/9精确匹配，输出前缀与镜像分别断言。保存已发布后的输出失败保留新文件，不声称回滚。

新增原生门禁复核发现共享分类器此前只要求owner；本轮补具体assertion marker及永久same-owner无关断言、marker非FAIL、duplicate和harness优先控制。YanFS10项、终端14个native项和6个runtime项现均通过对应判据；旧真实检出与测量保留，不把这次门禁缺口改写成历史回归失败。另以tests-first修复editor显式长度NAME中的NUL被截断为合法前缀；64例中包含完整拒绝和合法非NUL终止短名，总审核原探针重新严格ASan编译确认拒绝。

生产 `BUILD_TESTING=OFF` 独立ext4配置构建三个目标并运行同15项Guest验收，日志 `build/editor-off-final-build.log` 与 `build/editor-off-final-guest.log`，不依赖Unity或测试Guest库。当前ELF text 29920、data 40、BSS 80108字节；editor对象静态持有。11个生产C单元的90个静态帧合计2480字节、单帧最大528，editor start为144、app为96，无动态帧；这是编译器帧报告，不构成完整调用、递归或ISR嵌套栈上界证明。

故障driver只注入一次下一块请求IO失败，未单独注入保存的第二次metadata写失败；该边界仍沿0021。单根目录、连续extent碎片与覆盖峰值空间、单metadata写失败可能拒挂、无断电恢复、无deadline以及UTF-8按byte退格等限制保持。实现与验证完成，用户交付审查和理解程度仍待项目所有者判断。

### 规格与实现匹配复核（2026-10-05）

已批准的0021双输出重叠限制已落入规格、实现和永久测试；read有效字数清零、list两输出保持、无I/O及非重叠/零长度路径均核实。STATUS尾段仍称“实现修正与执行验证仍待完成”，属于未收口的补充时记录，本轮保留原文并标明后续落地。

独立审查另发现read先拒绝NULL/context别名字数指针、后检查状态，违反0021既定 `initialized → BUSY → FAULTED → MOUNTED → 其他参数` 顺序。真实探针在BUSY、FAULTED、未挂载配合NULL时均返回INVALID；补永久用例后先得到73例1失败，再把guard移到字数检查前。状态错误时仅有效且非context别名字数清零，无效指针不写；健康状态下无效参数仍为INVALID。真实callback中的BUSY组合检查整个FS缓存、输出哨兵和I/O增量不变。此项修实现以符合锁定SPEC，不新增接口例外或修改语义。

- 修复后Release：`ctest --test-dir build/terminal-release -E '_mutation$' -j3 --output-on-failure`，43/43，17.16 s。
- 修复后ASan/UBSan：`ctest --test-dir build/terminal-asan -j3 --output-on-failure`，43/43，33.15 s。
- 九组实际变异：`ctest --test-dir build/terminal-release -R '_mutation$' -j3 --output-on-failure`，9/9，87.45 s；终端20项与YanFS10项均0存活、0harness error。

均0失败、0SKIP；默认43组、Release开实际变异52组。日志为 `build/spec-imple-audit-20261005-fixed-release-default.log`、`-fixed-asan-default.log`、`-fixed-mutations.log`，前置构建日志分别为 `-fixed-release-build.log`、`-fixed-asan-build.log`；独立生产OFF配置三目标重建完成，记录在 `-fixed-production.log`。总审核在自有目录另行严格C17、ASan/UBSan编译运行核心73例全通过，并重编独立探针确认BUSY/FAULTED/NOT_MOUNTED精确返回，未复用旧二进制。

本轮修前默认43/43的19.22 s/34.85 s属于修前树，旧核心72例全绿也未覆盖新组合，不代替修后结果。只读审查报告不代替这些执行证据；read/list批准条款无遗漏，返回优先级错配已关闭。README的生产apps分区、STATUS过期依赖统计和历史导航已机械纠正，旧规格与阶段结论未改。

### 历史测量：Guest 终端文件操作（2026-10-04）

WSL Linux、GCC 11.4、C17 严格警告，启用工具并具备 Python、Unity 与 RISC-V 交叉编译器，未配置外部参考模型。默认注册43组；Release开启实际变异后共52组，包含九组实际变异。默认的两个 `*_mutation_gate` 是分类器负控，必须保留在默认套件中。

- Release默认：`ctest --test-dir build/terminal-release -E '_mutation$' --output-on-failure`，43/43，50.40 s。
- Debug ASan/UBSan默认：`ctest --test-dir build/terminal-asan --output-on-failure`，43/43，77.88 s。
- 九组实际变异：`ctest --test-dir build/terminal-release -R '_mutation$' -j3 --output-on-failure`，9/9，86.57 s；新终端组14个native与6个runtime非等价缺陷均命中具名具体断言，20检出、0存活、0harness error。

默认与变异结果均0失败、0 SKIP。默认保存为对应目录 `final-default.log`，变异汇总和完整输出为Release的 `final-all-mutations-fixed.log`、`final-all-mutations-fixed-detail.log`；具名终端变异证据保存于 `build/terminal-final-mutation-evidence/`。两个配置完成 clean-first verbose 构建（`final-build.log`）；Host核心、native shell/line及真实runtime driver启用ASan/UBSan，生产Guest仍是严格 freestanding RV32IM，未受Host sanitizer插桩。独立 `BUILD_TESTING=OFF` 配置实际构建生产ELF、yan_run与yan_mkfs，不依赖tests或Unity。

九组变异运行前后149个source/test/CMake输入的SHA-256相同。之后仅澄清shell.h中“FS从未初始化的普通INVALID”与“已初始化但未挂载的致命NOT_MOUNTED”注释，声明和实现未改；附加Release/ASan native shell/line各2/2通过（0.04 s/0.10 s）。最终149项清单总审核复算与当前文件一致。新的独立生产构建再次以实际ELF、yan_run与yan_mkfs执行files七场景，7通过、0失败、0harness error。

shell 66个Unity用例、line 40个用例、launcher 15个PTY/生命周期用例、生产Guest files 7个场景和真实UART/runtime 27个场景随默认回归通过。真实窗口核对RXDATA保留、source2 mask/ack/wake/complete、消费后rearm、predicate只读与BLOCKED期间另一任务推进；六个输出阶段分别注入首/中/末字节失败，绑定精确应用故障码及输出前缀。独立whole-image oracle核对成功写、新进程读回、拒绝行不改盘和提交后输出失败仍保留新文件。cat显示以CPython严格UTF-8 decoder与16项literal自检作为独立判据。

初期shell/FS别名探针触发invalid bool读取，修正为地址检测先于bool读取；TTY信号窗口探针曾出现属性未恢复，修正为raw前安装handler、restore后释放handler。PTY强制回收测试补SIG_IGN后READY屏障和实际-SIGKILL断言；receipt末byte注入曾命中CR，改为按接受byte计数命中末LF。内部oracle自检异常归HARNESS2。首次九组变异为8通过、1失败（124.34 s）：FS fatal变异预期误写unmount reason15，实际FAULTED可卸载并错误健康exit；门禁正确拒绝wrong-owner。改为绑定实际tohost1与预期SHELL_FATAL12后重跑，得到上述最终9/9。此前原八组8/8、185.04 s及默认43/43的45.75 s/75.97 s是独立阶段快照，不代替最终结果。早期局部green不代替这些失败与修复证据。

限制保留：单行文本、按byte退格、不提供完整Unicode编辑；EOF不可见Guest，断开不产生新的IRQ，不能保证BLOCKED立即发现断开。没有输入deadline或WFI，空闲仍耗CPU；SIGKILL不保证TTY恢复。FS连续extent、覆盖峰值空间、唯一metadata块、无断电一致性和自动修复沿0021。实现交付与理解程度由项目所有者判断。

### 历史测量：YanFS 初版（2026-10-03）

WSL Linux、GCC 11.4、C17 严格警告，启用 `YAN_BUILD_TOOLS`，具备 Python、Unity 与 RISC-V 交叉工具链，未配置外部参考模型。该阶段默认注册 37 组；Release 开启变异后共 45 组，包含八组实际变异。默认的 `yanfs_mutation_gate` 是分类器负控，不能被宽泛的 `-E mutation` 排除。

- Release 默认：`ctest --test-dir build/yanfs-release -E '_mutation$' --output-on-failure`，37/37，34.67 s。
- Debug ASan/UBSan 默认（变异关闭）：`ctest --test-dir build/yanfs-asan --output-on-failure`，37/37，56.59 s。
- 原有七组变异：`ctest --test-dir build/yanfs-release -R mutation -E '^yanfs' --output-on-failure`，7/7，165.16 s。
- YanFS 变异：`ctest --test-dir build/yanfs-release -R '^yanfs_mutation$' --output-on-failure`，1/1，7.18 s；十个非等价缺陷全部命中指定 owner 断言，0 存活、0 错误归因、0 harness error。

以上均 0 失败、0 SKIP。八组变异分两次运行，不合成为一次八组的耗时。最终默认汇总与完整输出分别保存为两个目录的 `final-default.log`、`final-default-detail.log`；变异为 Release 的 `existing-mutations.log` 和 `final-yanfs-mutation.log`、`final-yanfs-mutation-detail.log`。两个配置均完成 clean-first verbose 构建；构建与验证前后 127 个源码文件的 SHA-256 相同，总审核再算当前文件也无差异。

FS 核心 72 个 Unity 用例、adapter 25 个用例均通过。核心覆盖 CRC、规范名称与填充、保留字节、extent 越界/重叠、碎片与覆盖峰值空间、五个名称接口读前边界检查、输出别名、读失败前缀、BUSY、FAULTED 与缓存发布。adapter 覆盖 16 字节设备错误即时退出、半帧与分段载荷、非法头不等待、完整 wrong-op/tag 响应消费、提交 AGAIN 停用及标签回绕。mkfs 独立 Python oracle 与数据短写、metadata 短写、seek、flush、close 五类 Host 故障均通过。

Guest 整体用例在真实块层与协作式运行时执行创建 5 字节、覆盖 5000 字节、删除和再分配，最终 writer 3175487 步；两个新 reader 进程各 577282 步，镜像逐字节不变。Host 独立构造并核对整个 4096 字节元数据及全部 65536 字节镜像，decoder 自测拒绝 CRC 重封后的名称填充变化、尾零与邻块变化；CRC 损坏、有效 CRC 的 extent 重叠、版本错误分别拒挂。reader-before-writer 负控绑定精确 NOT_FOUND 结果、断言码和阶段；Guest 32 位地址范围越界输入返回 INVALID、不执行 I/O。

分类器的 26 项负控与跨套件异常优先级控制通过；完整 Unity footer、逐项 PASS/FAIL/IGNORE 计数及退出码必须一致。真实 no-effect 对照存活，wrong-owner 不计检出，崩溃、挂起、sanitizer、构建错误归 harness。九条源码/依赖探针通过：仓内源、测试或执行器缺失硬失败 1，只有缺外部编译器或 Unity 才返回 77；检查仅改临时副本。

独立只读源码审查与复审提出的名称扫描、合法响应消费、状态输出与检测力缺口已修复并验收；只读审查不代替执行验证。初版仍限制单根 63 文件与连续 extent，碎片和覆盖峰值空间可能导致 NOSPACE；唯一元数据块失败写可能拒挂，无数据 CRC、断电一致性、自动修复或响应 deadline。生产故障恢复需停止当前 `yan_run`，由新进程重开镜像并挂载；Guest 卸载/重挂不清除 Host 故障。实现与验证完成，交付审查与理解程度由项目所有者判断。

### YanFS 阶段验证与修复记录

task02 首次核心执行为 64 个用例、3 个失败、0 个忽略（`task02.log`）；task03 核心 68 个用例通过，核心与 mkfs 两组 CTest 2/2、0.21 s。task04 核心与 mkfs 通过，adapter 因严格编译后无可执行文件记为 Not Run；task05 核心 Release/ASan 专项通过，adapter 的 empty-ring 夹具未复位导致 24 例中 1 例失败，修正后通过。

task06 Guest 专项 1/1、12.62 s，首次八项变异有 owner 失败记录，但分类器仍错误接受空成功输出及不自洽 footer；task07 完整 footer/计数与异常优先级修复后复验通过。task08 补充合法 CAPACITY 帧回答 WRITE 的完整消费、未挂载 read 字数清零，以及越界/覆盖借旧 extent 两项变异，再运行上述最终默认与专项。修复前默认 Release 37/37、32.89 s，ASan 37/37、55.87 s 保存为 `before-task08-default.log`，属于修复前树，不代替最终结果。

### 历史测量：0020 持久化块镜像（2026-10-03）

WSL Linux、GCC 11.4、`YAN_BUILD_TOOLS=ON`，具备 Python 与 RISC-V 交叉工具链，未配置外部参考模型。默认注册 32 组：22 组 Host/Unity、9 组 Guest 验证及 1 组 Python 门禁负控；Release 开启 `YAN_ENABLE_MUTATION_TESTS=ON` 后注册 39 组。总审核独立核对配置、注册表与本轮保存的命令日志。

- Release：`ctest --test-dir build/closeout-persistent-release -E mutation --output-on-failure`，32/32 passed，21.10 s。
- Debug ASan/UBSan：`ctest --test-dir build/closeout-persistent-asan --output-on-failure`，32/32 passed，22.99 s。
- 七组变异：`ctest --test-dir build/closeout-persistent-release -R mutation --output-on-failure`，7/7 passed，157.64 s。

以上均 0 失败、0 SKIP。日志分别保留为各目录的 `final-default.log` 与 Release 的 `final-mutations.log`，不依赖会被后续 CTest 命令覆盖的 `LastTest.log`。`host_disk` 当前 10 个用例，`persistent_block_mutation` 检出 9 个非等价缺陷；baseline 与 no-effect 对照均存活。新增用例确认失败后读写不再调用文件 I/O，以及短写的 4095 字节在重开后仍存在、邻块保持原样，随后完整写可成功。

Host 变异逐项绑定表中指定的测试断言，无关断言失败负控不能计检出。Linux 测试专用执行器在真实关闭镜像后注入失败，CLI 验证 Guest 成功后最终退出 5、打印关闭失败诊断且镜像不变；忽略关闭失败返回值的隔离变异被测试拒绝。简单 reader 初始负控固定要求字节比较失败码 `0x80000008`。

25 个组合门禁负控通过；fake-wait、yield-once、omit-write 各命中声明的 Guest 断言码与阶段，wrong-block 命中 Host 字节比对。no-effect 存活，pure-hang 不计检出，编译失败负控由门禁检查拒绝。缺源与依赖共六个探针通过：缺执行器或被测 `host_disk.c` 返回 1，缺外部编译器返回 77，未移动工作树源码；记录在 Release 的 `source-probes.log`。

本轮验证正常运行与跨进程可见性，不验证断电持久性；文件后端仍是单写者、同步 Host I/O。外部参考层与 MSVC 工具目标本轮未运行。独立源代码审查及针对性复审未发现阻塞项，提出的断言归因与 CLI 关闭边界缺口已补强；只读审查不代替测试执行。用户已批准契约，交付审查仍待进行。

### 历史测量：2026-09-24 持久化组合门禁

2026-09-24 总审核复验：Linux 构建启用 `YAN_BUILD_TOOLS=ON`，具备 Python 与 RISC-V 交叉工具链，未配置外部参考模型。默认注册 **32 组**：22 组 Host/Unity、9 组 Guest 验证及 1 组 Python 门禁负控；开启 `YAN_ENABLE_MUTATION_TESTS` 后共 **39 组**，新增 7 组变异检查。

本轮 Release 默认套件 **32/32 passed，21.46 s**；`persistent_combined_mutation` **1/1 passed，6.21 s**。25 个门禁负控全部通过，原始的两个总审核复现负控原样通过。四个变异分别命中预定证据：fake-wait 为 writer `0x50000014 / phase 10`，yield-once 为 writer `0x50000013 / phase 10`，omit-write 为 writer `0x50000011 / phase 20`，wrong-block 为 Host 镜像字节不一致。无行为变化对照存活，纯挂起与编译失败不计检出。

修复前同日总审核实测 Release **31/31，22.84 s**、Debug ASan/UBSan **31/31，24.54 s**、全部变异组 **7/7，154.95 s**。本轮修改 Python 判据并新增负控，未重跑 ASan 全量与其余六组变异；这些历史结果不记为当前 39 组全量通过。外部参考层本轮未运行。

组合用例覆盖已完成响应的快速路径、背压下的阻塞与中断唤醒，以及写入后重新启动读回。Host 在两个模拟器进程之间独立核对全部 32768 字节，reader 逐字节核对目标块、快速路径块与邻块，再确认镜像未被改动。成功写响应要求完整写入及 `fflush` 成功；当前实现不保证断电持久性或失败写回滚，I/O 失败后拒绝后续读写，关闭并重新打开才能恢复使用。该次测量时后端语义尚待规格裁定；2026-10-03 的批准与锁定见本页开头及 0020。

### 历史测量：M2a

下段保留 2026-09-20 的原始测量与当时统计说明，当前配置以本节开头为准。

GCC 11.4、CMake 3.22.1 下的 Debug 与 Release 构建注册 **21 组 Unity 套件**（共 **172 个用例**：`uart` 19、`plic` 15、`transport` 14，其余为既有分组；`boot` 由 4 增至 6，新增两条是 0017 的 A1 / A2）。启用 Guest 工具链与 `yan_run` 时默认另有 6 组 Guest 侧测试：`guest_trap_env`、`guest_terminal`、`guest_console`、`guest_golden`、`guest_runtime` 与 `guest_block`，合计 **28 组 CTest**；加 `-DYAN_ENABLE_MUTATION_TESTS=ON` 时为 **33 组**（再注册 `guest_console_mutation`、`device_mutation`、`guest_runtime_mutation`、`guest_block_mutation`、`guest_m2a_combined_mutation`）。**以下数字以本段为准**，本页其它段落若残留旧口径以本段覆盖。**两次独立测量**：总审核角色在全新目录实测默认 **28/28 passed，25.21 s**、`ctest -R mutation` **5/5 passed，281.19 s**；主 Agent 在开关下实测整套 **33/33 passed，317.39 s**（`guest_console_mutation` 45.4 s、`device_mutation` 40.5 s、`guest_runtime_mutation` 48.2 s、`guest_block_mutation` 151.0 s、`guest_m2a_combined_mutation` 3.7 s）——两次测量量级一致，差异来自机器负载。变异检查默认不注册，只为不让慢证据主导默认套件；合并前必须显式跑一次，见 [CONTRIBUTING](../CONTRIBUTING.md) 的「合并前检查清单」与「提交前总审核检查清单」。

变异检查的耗时有一半花在文件系统上：它的 `--work` 一度放在构建目录（WSL 的 Windows 挂载盘 drvfs），同一套变异体实测 366.7 s；把 `--work` 移到原生文件系统（`/tmp/yan-mutation`）后降到约 35～45 s。这是 drvfs 逐文件操作的代价，不是被测代码的差异。

立即数测试覆盖全部 4096 种编码；移位覆盖 0～31 及寄存器移位量高位屏蔽。十条寄存器运算各覆盖 32768 种 rd / rs1 / rs2 组合。固定向量和边界矩阵检查符号比较、算术回绕、逻辑运算与 AUIPC 当前 PC 语义。

控制转移测试覆盖 B 型全部 4096 种偏移、J 型各偏移位与边界、JALR 全部 4096 种立即数及目标低两位。检查条件真值、寄存器字段、目标对齐、地址回绕、返回地址和延迟到下一次取指的映射错误。

访存测试覆盖八条指令的全部 4096 种偏移、寄存器字段组合、符号边界、小端读写与截断、地址和 PC 回绕、RAM 首尾及错误状态保持。取指失败时无数据访问，加载到 x0 仍检查错误；Store 可覆盖已经取出的当前指令。

异常测试覆盖各原因码、mtval、入口状态、Host 错误隔离、嵌套异常、无效入口、六种 CSR 指令的寄存器组合和读写抑制、MRET 恢复及 Machine 复位。Guest 集成测试通过 CSR 指令配置入口，在 ECALL 后调整 mepc 并返回继续执行。

M 扩展测试覆盖四种乘法结果、四种除法和余数结果、零除数、`INT_MIN / -1`、全部寄存器字段及非法 `funct7` 编码。

镜像测试用手工构造的 ELF32 覆盖段映射、`.bss` 清零、符号查找、越界段与各种畸形头；差分比较测试覆盖相等、仅 PC 不同、首个不同寄存器定位、x0 归一化与报告格式。

中断测试分三层。CLINT 层覆盖复位值（mtimecmp 复位为 `UINT64_MAX`，避免复位即触发）、msip 位宽、mtimecmp 与 mtime 的高低半字独立写、离散 tick、`UINT64_MAX` 不触发、`mtime == mtimecmp` 恰好触发、deadline 为 `mtime + 1` 不触发、64 位加法回绕、区域边界与未对齐 / 未映射错误。PLIC 层覆盖 priority、pending（只读）、enable、threshold 的 MMIO 往返，source 0 保留，`priority > threshold` 的严格比较，priority 为 0 永不触发，claim 清除 pending 并置 in-service、claim 仲裁（最高优先级、同优先级取最小号）、claim 后 MEIP 撤销、complete 清除 in-service。CPU 层覆盖三种源写入 `mip` 的采样、`mie` 掩码截断、`mstatus.MIE` 与 `mie` 的联合屏蔽、原因码优先级、mcause 最高位、mepc 指向未执行的下一条指令、mtval 为 0、入口 mstatus 与 MRET 按 MPIE 恢复 MIE、中断在取指前判定因而优先于同一条指令的同步异常，并用固定机器码跑完整的设备 → 中断 → 处理程序 → MRET → 继续执行回路。PLIC 层另有电平网关的用例：线在 claim 之后保持 asserted 也不会重新 pending（in-flight 抑制），complete 时线仍 asserted 才重新 pending，线 deassert 则撤销尚未 claim 的请求。Machine 层覆盖设备连接 / 清理、未连接设备时拒绝服务、整机复位同时复位设备、两台机器设备互不影响、每步只推进一 tick；另有一例检查 MMIO 窗口只是数据映射，指令取指仍只认 RAM。

中断测试另做了一轮变异检查：在副本里注入 7 个缺陷——中断原因码优先级改成软件优先、中断的 mtval 填 PC、MTIP 的判据改成严格大于、PLIC 阈值比较改成不小于、claim 不清 pending、source 0 可被驱动电平、`mie` 写入不做掩码截断——`clint` / `plic` / `interrupt` 三组测试对每一个都报告失败，没有存活者。这一轮的 7 个缺陷针对的是旧网关；电平网关那一轮的变异检查（[0016](specs/0016-plic-gateway-and-irq-lines.md) E 节）由独立验证完成：对抗用例加 40000 步随机差分参考模型，唯一存活的变异由新增的边界用例闭合（该用例已进入 `plic` 组，见 [0016](specs/0016-plic-gateway-and-irq-lines.md) 测试计划的 A7）。这轮验证在仓库之外执行，仓库里没有对应脚本。工作区新增的 `tests/guest/run_device_mutation.sh`（注册为 `device_mutation`）对 `plic` 注入 5 个缺陷，全部被检出、没有存活者。

UART 组覆盖 [0015](specs/0015-uart-device.md) v3 的寄存器表与语义：复位值、保留位读 0、只读寄存器忽略写入、未连接时设备完全惰性（三位全 0、两个方向都返回 `YAN_UNAVAILABLE`）、已连接时正常收发、`TX_READY` 与写入接受判据一致（含"读到就绪之后 backend 撤销"这条竞态用例）、backend 缺任一回调即被拒、断开连接清接收状态、整机复位保留 backend、单字节接收缓冲（读走清 `RX_READY`、读空不改变状态、缓冲满时不覆盖旧值）、`IRQ_STATUS` 写 1 清除、窗口边界与未映射偏移、空指针与参数错误、两台 Machine 互不影响。另有两条走平台接线：`connected_uart_reports_and_transmits` 覆盖逐字节发送，`receive_interrupt_reaches_plic_source_two` 覆盖 `push_rx` → 采样 → PLIC source 2 → MEIP。该组 19 例在 Debug ASan/UBSan 与 Release 下全部通过。工作区另落了设备侧变异脚本 `tests/guest/run_device_mutation.sh`（注册为 `device_mutation`），对本组注入 10 个缺陷，全部被检出、没有存活者。

传输通道组覆盖 [0014](specs/0014-host-transport-channel.md)：标识寄存器恒定、`HOST_READY` 由配置推导、只读寄存器忽略写入、配置落位校验（2 的幂、下限、越出 RAM）、空环往返、满 / 空边界、环回绕与字节序、越界在 Host 记账处锁存首个错误、doorbell 通知已注册的 Host、`IRQ_STATUS` 写 1 清除、窗口与 Host 参数错误、中断线驱动 PLIC source 1、整机复位保留 Host 配置、两台 Machine 的通道互不影响。该组 14 例在 Debug ASan/UBSan 与 Release 下全部通过（上一轮 ASan 下两条用例未释放整机 RAM 的泄漏已随用例修复消失）。同一个 `device_mutation` 脚本对本组注入 12 个缺陷，全部被检出、没有存活者。

控制台与 `os/` 层：`os/console.c` 已实现并提交在 `feat/m1-device-channel`（尚未进 `main`），[0017](specs/0017-console-and-os-layout.md) 的 B 组自检（`tests/guest/console_check.c` 与 `run_console.sh`：11 个 scripted 场景加 6 个 `yan-run` 场景）与 C 组变异检查（10 个植入缺陷，规格要求至少 6 个）都已落地，Host terminal backend 一并完成，三组分别注册为 `guest_console` / `guest_console_mutation` / `guest_terminal`，都通过；`guest_console` 的 17 个场景全绿，`guest_console_mutation` 报 10 detected / 0 survived。0017 的 **A1 / A2 也已落地**：`tests/test_boot.c` 的 `guest_uart_view_matches_the_host` 与 `guest_plic_sources_match_the_host` 逐条比对 `os/platform.h` 与 `include/yan/uart.h` / `include/yan/machine.h` 的 UART 基址、五个偏移、三个状态位、两个中断位与两个 PLIC 源号（该文件现在 6 个用例）。

**默认路径的等价性与 golden 基准**。`yan_run` 迁到 `yan_machine_step` 后，默认路径（无 `--terminal`、transport 未配置）与迁移前逐步等价，理由不是一次性的字节比对，而是设备线的取值：`yan_uart_pending` 在未接 backend 时返回 false、`yan_transport_pending` 在未配置时返回 false，采样把两个源驱动为 deasserted，而 deasserted 只清 pending、不产生新中断（[0016](specs/0016-plic-gateway-and-irq-lines.md) 的网关表）。这条性质现在由 `guest_golden` 长期守住：它用固定探针跑默认路径，把 `--trace` 与 `--signature` 与基准工件逐字节比较（本机实测 trace 656 行、signature 16420 字节），默认行为一旦变化就必须被看见并复核，而不是事后发现。基准工件的生成方式与重生成条件见 [`tests/guest/golden/README.md`](../tests/guest/golden/README.md)。

**基准差点进不了提交**。`.gitignore` 里"验证产物不提交"的 `*.hex` / `*.jsonl` 与基准工件的扩展名撞车，两个基准文件一度被静默忽略，只剩 `README.md` 可提交；那样的后果是基准进不了仓库，`run_golden.sh` 在别的机器上因缺基准直接 `SKIP(77)`——一个看起来在守着、实际什么都没守的检查。`.gitignore` 现已用取反规则显式放行这两个文件。判断"某文件能否提交"要看 `git add --dry-run` 是否列出它，**不能只看 `git check-ignore` 的返回码**：命中取反规则时它也返回 0。

Guest 环境分两层。Host 层（`Boot`，始终运行、不需要交叉工具链）逐条比对 Guest 与 Host 的 CLINT / PLIC 设备常量、CSR 编号与 `mie` / `mstatus` 位（UART 常量比对属于 [0017](specs/0017-console-and-os-layout.md) 的 A1 / A2，尚未加入），检查陷阱帧的 17 个槽位互不重叠且帧长为 16 字节倍数，并让 CPU 执行 Guest 的 `sw` / `lw` 真正读写 CLINT 与 PLIC；另有启动契约：复位后无向量、`csrw mtvec` 只保留 31:2 位、`mstatus` 掩码行为与 Guest 启动假设一致。Guest 层（`guest_trap_env`）编译并运行一段自检程序，覆盖启动状态、向量安装、ECALL / EBREAK / 非法指令 / 非对齐访存四种同步异常走同一向量、MSIP 与 MTIP 的中断进入与返回、调用者保存寄存器跨陷阱不变、被中断指令按处理程序的决定重放或跳过，以及 CLINT / PLIC 的 Guest 访问。

Guest 层同样做了变异检查：在副本里注入 10 个缺陷（不清 MSIP、不 disarm MTIP、MTIP 未使能、不改 `mepc`、不装 `mtvec`、帧只压 4 字节、帧槽位与 `a4` 重叠、错误 CLINT 基址、错误 PLIC enable 偏移、`mtimecmp` 只写低半字），Guest 自检或 Host 测试对每一个都报告失败，没有存活者。第一轮还暴露出自检自身的漏洞：失败码 1 与「通过」的 `tohost = 1` 撞车，使 `no-mtvec` 变异体存活，修正为失败码最高位置 1 后才被检出。

### 外部验证层

以下结果来自外部工具链与参考模型，依赖缺失时对应的 CTest **不注册或报 SKIP**，从不显示为通过。

- **逐指令差分测试**：18 个 Guest 镜像（5 个手写源 × -O0 / -O2，加 8 个随机生成程序）与 NEMU riscv32 参考模型逐条比较全部一致，结束时整段 RAM 一致。
- **变异测试（DUT 侧）**：向副本注入 4 个已知缺陷——SLTI 有符号比较改无符号、JALR 不清 bit 0、DIV 除零结果改 0、LB 去符号扩展——差分测试对每一个都报告了不一致，并给出出错指令的 pc 与机器码。测试本身若放过任何一个即判失败。
- **变异测试（参考模型侧）**：把 NEMU 参考模型的 `slt` 改成无符号比较、重新构建共享对象，差分测试仍然报告不一致，而未变异时同一镜像通过。只改 DUT 只能证明工具有检测力；改参考模型才能证明比较真的读取了参考状态。
- **官方 riscv-tests**：`rv32ui` 与 `rv32um` 共 50 例，49 例通过，1 例显式 SKIP（`rv32ui-p-ma_data` 要求非对齐访存成功，而 Yan 平台按设计拒绝）。
- **官方 ACT4 测试语料**：`riscv-arch-test` 的 `rv32i/I` 与 `rv32i/M` 共 47 例，其中 40 例的 DUT 签名与 Sail 参考模型的签名逐字节相同，0 例不同，7 例 SKIP。7 例全部是 `I-beq/bge/bgeu/blt/bltu/bne/jal` 的分支与跳转用例，它们故意构造未对齐目标并期望陷阱处理器接管；框架把陷阱处理器的实例化放在 `STANDARD_SM_SUPPORTED` 之内，而该开关的前提是 `medeleg` / `mideleg` 委托、PMP 与 S-mode，YanOS 都没有，所以两侧都落到地址 0 并报「possible trap loop」，脚本据此报 SKIP 而不报通过。`mie` / `mip` 与 CLINT / PLIC 已实现，但语料里**没有任何用例调用中断宏**（`RVMODEL_SET/CLR_MEXT_INT`、`RVMODEL_SET/CLR_MSW_INT`、`RVMODEL_MSIP_ADDRESS`、`RVMODEL_MTIME_ADDRESS` 在 `tests/rv32i/I` 与 `tests/rv32i/M` 中出现 0 次），因此本阶段交付的是接口而不是 ACT4 中断通过数。脚本另有两项配置检查：DUT 头声明的 CLINT 地址必须与框架 `sail_macros.h` 的有效地址一致（探针汇编失败即 FAIL），以及 DUT 中断宏守卫确实被框架覆盖。这一层使用官方测试源与官方参考模型，但不使用 ACT4 自带的构建系统。
- **已知限制（既有设计，非本轮引入）：ACT4 脚本把"两侧都没出签名"记成 SKIP，因此部分跳过仍可能整体通过**。`tests/official/run_act4.sh:205-211` 在 Sail 与 DUT **都**没有产生签名时按"用例需要陷阱处理器"计入 `skipped`；而整体判定是 `[ "$differed" -eq 0 ] && [ "$matched" -gt 0 ]`（`:239`），只有在**全部**跳过（`matched = 0`）时才 fail-closed。**事实**：单看一条用例无法区分"DUT 行为不对"与"该用例需要陷阱处理器"——两侧都拒绝同一镜像时证据是一样的。**代价**：在真有 ACT4 检出的机器上，它可能掩盖"某几个用例的 DUT 行为不对"，只表现为跳过数偏多。**触发条件**：配置了 `YAN_RISCV_ARCH_TEST_DIR` 且部分用例未出签名（本机没有 ACT4 检出，该脚本根本不会注册，所以本机遇不到）。**候选动作**：下一阶段真正跑官方语料时，把它改成"跳过比例超过阈值即 FAIL"，并**在能验证的机器上**落笔——按项目纪律，未验证的改动不进仓库，所以本轮不修。
- **签名比对**：8 个镜像的签名与 Sail（第三方参考模型）逐字节相同，签名区大小 256～17184 字节。
- **Guest trap 环境**：编译 `tests/guest/mtrap_entry.S`、`mtrap.c`、`trap_env.c` 并由 `yan_run` 执行，Guest 自检的每一项都通过（退出码 0）。该层只用交叉工具链与 DUT，不需要参考模型，因为陷阱与中断语义不在参考模型范围内。

### 覆盖边界

- 差分测试比较 PC、32 个通用寄存器与结束时的整段 RAM；NEMU riscv32 参考状态里没有 CSR 字段，因此 `mstatus`、`mtvec`、`mscratch`、`mepc`、`mcause`、`mtval`、`mie`、`mip` 与 trap / MRET / 中断语义**不在差分测试覆盖范围内**，含这些指令的 Guest 不适用于该层。差分执行器不连接 CLINT / PLIC，中断线恒为低。
- 参考模型由本仓库补全（上游对应文件是留白桩），与 DUT 同作者，证明的是两份实现一致，不等同第三方模型提供的证据；第三方证据来自官方 `riscv-tests` 与 Sail 签名比对，二者都只覆盖用户态 RV32IM。
- Guest trap 环境由自检程序验证，证据来自本仓库：它证明陷阱路径在**这台**实现上按规格工作，不构成与外部模型的一致性证据。外部中断（MEIP）无法从 Guest 侧驱动——PLIC 没有 Guest 可写的源寄存器，源线只能由平台按设备电平用 `yan_plic_set_level` 驱动，而两条设备线的置位都由 Host 侧动作触发（`yan_uart_push_rx`、Host 写入传输环）——因此 Guest 自检只覆盖 MSIP 与 MTIP 的进入 / 返回。Host 触发的 MEIP 投递由 `Interrupt` 那组 Host 测试覆盖；UART 接收线的整条路径（`push_rx` → 采样 → PLIC source 2 → MEIP）另由 UART 组的 `receive_interrupt_reaches_plic_source_two` 覆盖。
- `yan_run` 现在按平台语义映射四个设备窗口并每条指令推进一次 `mtime`，`yan_difftest` 不映射设备、不推进时间。同一镜像在两个执行器下若读取 `mtime` 会得到不同结果，因此含设备访问的 Guest 只适用于 `yan_run`，这一条已在工具注释与规格里写明。

Debug 启用 AddressSanitizer 和 UndefinedBehaviorSanitizer。内存分配失败分支尚未通过故障注入验证。

CI 覆盖 Linux Debug 检测构建、Linux Release 和 Windows Debug。外部验证层需要 NEMU 参考模型与 RISC-V 工具链，暂不在 CI 中运行，因此不对外部层做持续集成结论。各任务的运行结果与审查状态见 [Pull Requests](https://github.com/YanBo-maker/YanOS/pulls)。

## 下一步

审阅已实现的0023编辑流程、保存与取消的实际字节变化，再由项目所有者选择下一项能力。编辑器、终端与文件系统的交付审查和学习程度仍由项目所有者判断；尚无新能力规格锁定。

### 历史推进记录（M1a / M2a）

以下保留当时的分支、计数与待办，后续记录中的修复和裁定取代早期状态；它们不描述当前工作树。

完成 M1a 的收口：`feat/m1-device-channel` 上的 11 条提交（`d612bec..8a8c8c1`）已经推送，但它们**不在任何 PR 里**——PR #13 在 2026-09-20 19:06 以 `fa7af56`（`os/` 骨架那一提交）为头被合入 `main`（merge commit `03f5b80`），GitHub 因此不再推进 `refs/pull/13/head`。下一步是**为这 11 条提交开一个新的 PR** 交项目所有者审阅；`main` 目前只含到 `fa7af56`，本文记录的设备、控制台与 golden 都还只在任务分支上。

**M2a 的实现已在分支 `feat/m2a-block-and-runtime` 上收口**（自 `e5ae493` 切出，9 条提交 `71769c9..65d22bb`，已推送，远端 ref `65d22bb`；工作区干净）。**PR 尚未创建**——需要项目所有者在 GitHub 上新建，堆叠目标为 `feat/m1-device-channel`；在此之前 M2a 的实现只存在于该分支上。提交后复核（总审核角色独立执行，不采信执行方数字）：只有 `CMakeLists.txt`（4 条）与 `tools/yan_run.c`（2 条）出现在多条提交里，正是设计的 hunk 级拆分；`1e83dc5` 的 `yan_run.c` 里 `yan_transport_host_readable` 出现 0 次（边沿版），`2dffc6b` 的 diff 恰好只有门铃那一处；注册名逐条递进（`1e83dc5` 无新注册 → `d18aacb` `guest_block` → `6600c61` `guest_runtime` → `4347e8e` `guest_m2a_combined`）；`git diff --stat e5ae493..HEAD -- 0005/0006/0007/0008/0012` 为空；文档只出现在第 1 条（规格）与第 9 条（README/STATUS/CONTRIBUTING/0011）；并用 `git archive` 取出两个快照独立复跑：**第 6 条树上 28 组里恰好只红 `guest_m2a_combined`**，**第 7 条树上 28/28 全绿**——"预期失败 → 修复转绿"的教学结构成立。**记账**：第 3 条（`feat(host)`）的提交信息里"当门铃响起时服务一次"描述的是**该提交当时的边沿行为**，不是最终设计；这一决定的缺陷与修复见第 7 条（`fix(host)`），两者在历史里成对存在。主 Agent 决定**不改写提交信息**（reword 会重写其后全部哈希、让本页记录的提交号与远端 ref 失效），因此把导航记在这里：读者从本页即可知道"边沿 → 电平"是一个被记录下来的演进，而不是漏掉的旧说法。

开始 M2a：[0018](specs/0018-block-protocol.md) 与 [0019](specs/0019-cooperative-runtime.md) 的规格已经锁定，实现按它们的 IMPLE PLAN 推进——先扩展 `os/platform.h`（通道寄存器与 PLIC 访问器）与 `tests/test_boot.c` 的防漂移比对，再做块协议的帧层与协作式运行时。0019 里记录了一笔已知债务：树里会同时存在 `tests/guest/mtrap_entry.S` 与 `os/trap_entry.S` 两份陷阱入口，合并成一份的条件写在那一节。

架构前置第一块（`os/platform.h` 的通道与 PLIC 视图 + `test_boot.c` 的比对）已经落地并验证：常量与 `include/yan/transport.h` / `include/yan/interrupt.h` 一一对应（只差 Host 侧的 `YAN_TRANSPORT_MIN_RING_SIZE`，那是配置校验用的常量，Guest 侧不需要）；只有 `G2H_HEAD`（Guest 生产）与 `H2G_TAIL`（Guest 消费）带 setter，Host 驱动的 `G2H_TAIL` / `H2G_HEAD` **故意没有 setter**，让"写了被忽略"这件事在接口上就不存在；流控封装在两个方向都是全有全无。实测 `test_boot` 8 个用例、全量 27/27。

同期登记两处**规格空档**（不改规格，按 IMPLE 级记录，供实现与验证遵循）：
- **`mie.MEIE` 由谁置位**：0019 把 IRQ route 列为调用方责任，但只枚举了设备 `IRQ_ENABLE` 与 PLIC 的 enable / priority / threshold 两级，漏了 CPU 一级。主 Agent 的裁定是：`yan_os_sched_run()` 入口置 `mie |= MEIE` 并使能 `mstatus.MIE`——依据是那一节的枚举**没有**把 CPU 一级交给调用方。这是填补空档、不动接口与不变量。**实现已经依赖这条裁定**：`os/task.c:80` 定义 `YAN_OS_MIE_MEIE`（`0x800`）、`:212` 在调度器入口 `csrs mie`、`tests/guest/runtime_check.c:66` 还有 `RT_FAIL_MEIE_OFF` 断言。**若项目所有者否决，运行时代码与测试都要改**，不是只改文档。
- **错误响应的集成不变量**：0018 要求 `status != 0` 时 `count = 0`，Guest 帧层又按 `count` 计算期望字节数；两者只有 Host 真的写 `count = 0` 时才自洽。**不变量本身成立且已被实现、也被两个 drive 的断言覆盖**：Host 不得发出 `status != 0` 且 `count != 0` 的响应。**但它的后果不是 Guest 挂起**：`os/block.c:229-238` 对非 OK 状态把期望载荷置 0，因此 Guest 会正常交付那个失败头、由调用方自己的检查抓到（实测 `:FAIL: fault: a failed read completes no blocks: expected 0, got 1`）。原文的"挂起"推断有误（主 Agent 用推断代替了实测，已认领并更正）；**验证方仍必须断言 Host 从不发出 `status != 0` 且 `count != 0` 的响应**，这不是规格改动，是验证要求。

协作式运行时的实现已落地并自测通过，本机独立复跑（我另建工作目录）结果一致：自检 **11/11 场景通过**，变异检查 **6 detected / 0 survived**，且 `os/task.c` 的 md5 在变异前后相同（脚本自己复原）。随后一轮**全新上下文、证伪导向**的独立验证在实现侧**未能推翻任何结论**：等价变异体（私下植入两支、11 场景全 PASS，且 `objcopy -O binary --only-section=.text` 后变异体与原件的 `.text` md5 相同——编译器已折叠不可达分支）、陷阱帧（自写探针逐寄存器确认 16 个 caller-saved 全保住；逐个删除 `sw` 的负控 16/16 被抓）、注入时点（反汇编确认落点并做 2×2 对照：窗口内变异体死、窗口外存活）。该轮同时**打回了验证门禁**，见下面那一条。块协议侧：门禁经发现者复验通过（六条打回项 CONFIRMED FIXED、14/14 变异体带 `:FAIL:` 证据），但**随后新挖出的 5 条未修完**，且 `SKIP_RETURN_CODE 77` 的洞正在全仓统一修；因此**本文不记为已验证，也不引用任何"存活 0"**。运行时侧门禁修复已交付并复验，组合面另发现并修复了 F1 死锁。**计数**：`guest_m2a_combined` 与 `guest_m2a_combined_mutation` 已注册（前者在第二个任务腾空响应环、全程不响铃，因此它同时是 `yan_run` 门铃处理的回归；后者的 `fake-wait` 变异能通过全部功能检查，只有阻塞窗口那条断言能把它分开，见 `CMakeLists.txt` 的两处注释）。实测：默认套件 **28 组**（`guest_m2a_combined` = #28，本机 1.86 s 绿）、开 `-DYAN_ENABLE_MUTATION_TESTS=ON` **33 组**；我在自己的全新构建目录里复跑 `ctest -N` 得到同样的 28 / 33，**先前写的 29 是把两个新用例都算进了默认套件，已更正**。**口径可复现**：本机 `build/` 是各构建目录的容器（`build/arch-m2a` 是主 Agent 的隔离目录，其余为各 Agent 的目录），上面的数字在隔离目录与我这边的全新目录中各得一次，结论一致。以下条目是需要留档的裁定与实测：

- **等价变异体（不可杀，必须证明而不是声称）**：0019 的 C 组第 4 条把"`switch_to` 之后不恢复中断状态"写成"无条件置 1 **或** 无条件清 0"，但**"无条件置 1"是等价变异体**：`os/task.c:396` 的 `previous` 是 `const`（`csrrci` 读并清之后**紧接** `if (previous == 0) panic`），因此此后它恒为 `YAN_OS_MSTATUS_MIE`（`0x8`）；两处 `yan_os_irq_restore(previous)`（`:417` 提前返回、`:439` 正常路径）只有置位分支可达，于是"恢复保存值"与"无条件置 1"生成完全相同的指令序列。**不存在**能区分二者的合规测试，除非删掉规格强制的前置条件检查——而 0019 自己已经写明"有了这条，save/restore 的两端就是同一个值"。**裁定**：只构造可杀的那一支（脚本里的 `restore-after-switch-clears`），不构造违背后置条件的测试去凑杀掉数、不削弱前置条件检查；存活者仍为 **0**，但该条**单列**为"等价变异体 + 证明"。口径：**每个非等价变异体都必须被杀死；"等价"必须被证明。** 规格措辞的修改（C 组第 4 条只列可杀的那一支）属语义性修改，**待项目所有者裁定后**由总审核角色落笔，裁定前不改 0019。
- **运行时并不依赖 `os/console.c`**（更正实现方报告里"必须追加 `console.c` 否则无法链接"的说法）：`os/task.c:131-133` 对 `tohost` 与 `yan_os_console_puts` / `putc` 都是 weak 声明（`:5` 先包含 `console.h` 再重声明，**顺序必需**——重声明若挪到首次调用之后，引用会退回强引用、静默失去该性质）。本机独立复现：镜像**不含** `console.c` 时链接 rc=0，运行得到 `the Guest reported failure code 2147483650`（= `0x80000000|2`，`INTERRUPTS_DISABLED`）after 36 instructions。`console.c` 的唯一作用是让 panic 的诊断行可观测，与 0019 panic 一节的措辞一致。
- **验证门禁的判据（项目级教训，本轮由独立验证打回）**：变异检查**只认断言失败**。脚本层的 timeout、信号死亡、sanitizer 报告、构建失败一律归 harness error / INCONCLUSIVE，不能计成"检出"——否则一个把测试挂死或打崩的变异体会被记成被杀掉，门禁就成了空转。同仓 `tests/guest/run_device_mutation.sh:1174` 的 `failure_kind()` 是既有正确形态（`:1254` 只把 `assertion` 记为检出）；`run_runtime.sh` 一度在 `case "$status"` 之前先比对注入计数，于是"纯挂起/纯崩溃"也能落进检出——已打回修正，并要补 baseline 守卫。**注意：当前 6 个运行时变异体的裁定仍成立**（验证方逐个核过每个至少有一条真断言型检出），被推翻的是门禁本身。**在门禁修好并重跑之前，不要引用任何"存活 0"作为证据。**
- **附带事实（影响 SKIP 的可达性）**：`CMakeLists.txt:101-102` 先 `unset(YAN_RISCV_GCC CACHE)` 再 `find_program`，所有 Guest 侧测试的注册都挂在 `YAN_RISCV_GCC` 上，因此**缺工具链时测试根本不注册**；`SKIP_RETURN_CODE 77` 只在"配置时有工具链、运行时缺依赖"时才可达。
**规格锁定后被修正（三项，均经项目所有者批准）**：

1. **0018 §整帧原子性 规则 2 的取模基准**：原文 `(H2G_HEAD + i) % RING_SIZE`，**改为 `(H2G_TAIL + i) % RING_SIZE`**，并补上理由（消费侧的读位置是消费者的读指针）。证据：三方独立确认（实现方提出、总监督复核 `os/block.c:45` 与 `tools/host_block.c:285` 两侧都按消费位置取模、独立验证 Agent 复现）；反例是 `RING = 8192`、`TAIL = 0`、发布完整 16 字节后 `HEAD = 16`，字面公式会读 16..31 而不是 0..15，半帧（`HEAD = 8`）时更会读到 8 个未发布字节；实测按字面实现后 **12/18 行变红**（capacity 行 `:FAIL: … expected 0, got 4294967293`）。**归属：主 Agent 在锁定前的漏检**（他逐行读过 0018 却没发现这一处）。
2. **0018 §发送前流控 补读侧上界**：读的响应帧是 `16 + count × 4096`，因此 `RING_SIZE = 8192` 时读与写的 `count` 上界都是 1；不补写会让读者以为读没有上界。实现取显式失败（Guest 层 `max_count` 拒绝且不写一字节；Host 对"响应放不下"的读回 status 1）。
3. **0019 VERIFY C 组第 4 条只列可杀的那一支**：清单里保留"无条件清 0"，"无条件置 1"移出并指向本页的等价性证明。依据是**双人独立确认**——实现方植入两支跑 11 场景全 PASS；独立验证 Agent 用更强证据：`objcopy -O binary --only-section=.text` 后**变异体与原件的 `.text` md5 完全相同**（GCC 已证明 `previous == 8` 并折叠掉不可达分支），8 个攻击面全部失败；规格 0019 自身在 save/restore 一节末句已自认"两端就是同一个值"。

- **门禁纪律一：检出必须指向至少一条 `:FAIL:`**（块协议门禁被 4 个元变异体证伪）。最严重的一条是**零行为变化**：只在 pump 里加一行 `fputs(..., stderr)`，就被判成 `detected by unit (wrote to standard error)`，而 unit 日志是 `266 check(s), 0 failed`——**一个什么都不改的变异体会被记成被杀掉，而 sanitizer 报告走的正是这条路径**。另三条：纯崩溃（exit 139）、fixture abort（exit 2）、Guest 陷阱晚于全部检查通过（`tohost != 1` 被一律映射成"Guest check failed"）。**纪律**：非零退出码、stderr 非空、信号死亡、sanitizer 报告、fixture abort **都不得**产出检出；Guest 侧必须把"陷阱"与"检查失败"分开。附带事实：Host unit 这条路径**没有内建超时**，会挂死，唯一上界是 CTest 的 900 s。
- **门禁纪律二：`SKIP_RETURN_CODE 77` 不得由"被测实现缺失"可达**。实测：把 `os/block.c` 移走 → 两个块脚本都退 **77**，CTest 记 `***Skipped` 并报 `100% tests passed, 0 tests failed`——**删掉被测实现反而是全绿**。对照 `run_runtime.sh` 在移走 `os/task.c` 时退 **1**（正确）。**纪律**：77 只允许表示"缺工具链或宿主依赖"；被测实现缺失必须硬失败。全仓 Guest 脚本的 77 语义审计正在进行，**结论出来之前不得声称"全部套件的 77 语义已核对"**。
- **实现缺陷（修复中，不要写成已修复）**：`tools/host_block.c:326-330` 在"回复放得下"检查与 publish **之前**就清了 `fail_next`，于是"环里放不下回复"时**注入的 status 2 被吞、下一次请求反而成功**（探针：`call 1: answered=0 served=0 fail_next=0` → `call 2: … status=0`），违反 `tools/host_block.h` 的文档语义。已打回，要求把清除时机移到 publish 成功之后并加用例钉住。
- **两处低优先项**：`os/block.h` 关于 `__ashldi3` 的注释把"依赖优化档"写成了无条件事实（实测本项目的 Guest 口径 `-O2` 下 GCC 开放编码 64 位变量位移，`-Os` 才引 `__ashldi3`）；以及一处**接口缺口（非违规）**——`os/platform.h` 只有 `push` / `pop`、没有非消费式的 peek，因此环内取模与基址算术在 `os/block.c:45` 与 `tools/host_block.c:285` **各写了一份**，不违反 0014 的"位置推进只走 push/pop"，但两份算术是重复的，登记为已知缺口。
- **组合面才发现的一个真缺陷（F1 死锁，已修）**：门铃是**边沿**，而"我有一个答不了的请求"是**电平**。泵在回复放不下时 `break` 且不消费请求，但边沿已被消费；此时 Guest 已阻塞在 `wait` 里、不会再次响铃 ⇒ **已 accept 的请求被永久搁置**。修法是把服务条件由 `doorbell_rung` 改成 `doorbell_rung || yan_transport_host_readable(&machine.transport) > 0`（请求环里还有未消费字节就重试），让门铃回到"通知"的本义。**关键点：规格里没有任何一条禁止它，两个组件各自测也都测不出来**——只有组合用例能发现，这是"集成必须真的做"的实证。
- **修好之后组合用例反而红了（反直觉但正确）**：它原来的"阻塞窗口"建立在那个 bug 之上（"A 取走第一个响应腾出空间 → 立刻 wait"，旧边沿行为下泵不会自己重试，predicate 必假）。修复后泵会自己补答，A 走到 `wait` 时 predicate 已真，走规格第 2 步的快速路径。**合法的窗口**是"A 在回复放不下时就阻塞、空间由**另一个任务**在 A 已阻塞之后腾出"；用例已按此重构，并做反向验证：把泵改回边沿触发 ⇒ 新用例 `INCONCLUSIVE combined case: no verdict within 3000000 instructions` ⇒ **证明它确实是该修复的回归测试**。重构后的 Guest 镜像里 `yan_os_transport_doorbell` 出现 **0** 次（窗口释放全程不靠响铃），`polls == 1` 判据保留，`fake-wait` 变异仍被杀死（`polls = 34`，且**全部功能检查都通过**——只有这条判据能区分）。
- **组合面的两条覆盖事实（供将来别高估覆盖）**：严格交替的驱动在生产泵下**永远不会真正阻塞**（响应在门铃后一条指令内 publish，打印 `sequential-waits idle_ticks=0`，另一个任务一次都没被调度），阻塞路径只能靠流控背压或更慢的 host 覆盖；**残余（非 API 可达）**——Guest 若绕过 framing 层直接用 `yan_os_transport_h2g_pop` 弹出非帧对齐的字节数，可把响应环永久顶在 ≥ 4080 已用，使排队的读永远答不出（`take` 不走这条路：要么整帧成功、要么完全不消费，畸形帧按帧长丢弃）。因此登记一条约束：**framing 层是唯一受支持的消费者**。另记一条成本项（非缺陷）：Guest 永不排空时，泵每个指令边界做 O(16) 的无效重试直到步数上限（实测 3M 步约 0.7 s），不饿死终端轮询或 Guest。
- **0018 门禁：发现者复验通过，随后又挖出 5 条（已派单，未修完）**。复验结论：六条打回项全部 CONFIRMED FIXED；14/14 变异体各自带 `:FAIL:` 证据；6 条永久负控（含"零行为变化"那条）全部 behaved；`fail_next` 已修（探针 `call 1: fail_next=1` → `call 2: status=2`）并有用例加变异体双重钉住。新 5 条：门禁 mutant→drive 映射缺口（**一个真的跨回绕缺陷因此 SURVIVED，而同一缺陷用 guest drive 跑就能被杀**——是门禁表格接不到）、`run_block.sh` 在执行器不支持 `--disk` 时仍 exit 0 报 PASS、陷阱级联需要新增 `DETECTED (trap cascade)` 类（陷阱 + Guest 自身 `:FAIL:` 计检出并标注；陷阱 + 0 条 `:FAIL:` 仍 INCONCLUSIVE）、`available < 16` 这条规格正文条款不该有"删掉还全绿"的用例集、`os/platform.h` 加变异接缝。**修复完成前不写成已修**。
- **`SKIP_RETURN_CODE 77` 的洞正在全仓统一修**：口径是——移走被测实现 / 测试程序 / 基线 → **非 0 非 77** 并打印 `FAIL the source under test is missing: <路径>`；移走仓库外依赖 → 仍 77；改完在完好树上必须仍全绿。**正当 77 只允许**：交叉工具链、`timeout(1)` / `nm` / `cmake` / `python3` 等宿主工具、Unity 检出、外部参考模型（NEMU 源与 `.so`）；数据类基线（如 `tests/guest/golden/probe.trace.jsonl`）默认缺失即硬失败。**这条洞自 M1 起就存在**，意味着此前"全绿"的说法对这 12 个用例是不成立的——不是某次改动的回归。
- **77 修复第二轮（清单外的 4 个洞）已交付，主 Agent 已批准其判定顺序**：46 用例的审计结果为 **BEFORE 10×rc0 / 2×rc1 / 34×rc77 → AFTER 11×rc0 / 22×rc1 / 13×rc77**，22 处变化**全部是 77→1**，13 条正当 77 保留。它把判定顺序改成"**外部依赖先判（77）→ 仓内产物后判（1）**"，并明确声明这是**语义性决定**、两个方向都测过；主 Agent 批准的理由是：本机没有 Sail / ACT4 / riscv-tests 时仍必须是 77，而"外部在、仓内产物缺"从 77 变 1——正是要封的洞。
- **一条未被触发的静默通过路径（已修）**：`run_runtime.sh` 的 `unrunnable` 计数器在 PENDING 之后仍打印 PASS 并 exit 0；已改为 FAIL + exit 1。它此前**从未被触发过**，所以既没有掩盖过失败，也没有在日志里留下痕迹——登记它是为了说明"打印 PASS 且 exit 0"本身也需要判据，不能只看输出。
- **规格列举之外的 panic 条件（加法式行为）**：运行时另加了 `UNEXPECTED_TRAP`（同步异常、或非 MEIP 的 cause）、`UNSERVICED_SOURCE`、`NO_TASK`、`EVENT_RANGE`、`PREDICATE`、`EXIT_RETURNED`。0019 那一节的措辞是"下列情形是编程错误"（举例非穷举），因此这些加法式条件站得住，其中 `UNSERVICED_SOURCE` 与 0016 的电平语义一致。**未来影响**：`UNEXPECTED_TRAP` 意味着将来把 `ecall` 用作系统调用入口时必须改这一处。

接入 ACT4 自带的构建系统（`testplans`、UDB 配置与框架的 RVMODEL 生成），让参考模型的期望结果在构建期编译进自检 ELF，从而把当前依赖签名比对与 SKIP 的 7 个异常相关测试也纳入；这需要先补齐 `medeleg` / `mideleg` 与异常委托，或为框架提供一条不经过它们的 M-mode 启动路径。CLINT / PLIC 的寄存器与 Guest 侧陷阱环境已经存在，地址映射也已与框架核对过；剩下的是框架启动路径依赖的委托与 PMP 状态，以及语料中尚不存在的中断用例。

已实现 40 条 RV32I 基础指令的功能路径、八条 RV32M 指令、CSR 指令和 MRET。单步返回 YAN_OK 表示正常完成，YAN_TRAP 表示已进入 Guest 异常或中断；Host 参数和对象状态错误仍直接返回。Bus、RAM 与独立取指接口保留原有错误语义。

中断与陷阱目前只覆盖单 hart、M-mode、直接入口与非抢占的固定优先级：没有 S/U 模式，没有 `medeleg` / `mideleg` 委托，没有 `mtvec` 向量模式，没有中断嵌套，PLIC 只有一个 M-mode context，CLINT 只有 hart 0 的 msip / mtimecmp。Guest 已有协作式任务调度；系统调用 ABI、进程、页表与抢占式调度尚未实现。也未完成完整 ISA / 特权架构符合性验收。陷阱与中断语义没有参考模型可比较，证据来自本仓库的模块测试与 Guest 自检，属于已知边界而非已验收能力。两个设备（UART、传输通道）同样没有外部参考模型可比，证据只来自本仓库的模块测试；控制台与 `os/` 层只有 Guest 侧自检与变异检查，同样没有外部参考模型。

## 规格锁定后的修正

2026-10-03：0018 原文“v1 的失败是全有全无”保留；补充响应语义与介质状态的边界及 0020 链接。依据为项目所有者在本会话对“成功前刷新、失败后停用、失败写不承诺回滚”方案的“可以”批准；由总审核角色落笔。0018 未改变帧格式或 status 值，文件后端的恢复规则在 0020 定义。

2026-10-03：0021 锁定后补充 read/list 双输出不得彼此重叠，属于经批准的接口语义修正。原文“所有输入、输出缓冲与输出参数不得与 YanFs context（包括 metadata/scratch）重叠”保留，但它没有约束两个输出彼此重叠；原有 read 前缀计数与 list 文件信息、下一槽要求也保留。实际冲突输入是 read_bytes 指向 out：读取四字节 `ABCD` 时，同一地址无法同时保存 `ABCD` 和数值 4；list 的 cursor 指向 info.size_bytes 时，大小 5 的文件位于槽 0，同一字段无法同时保存大小 5 和下一槽 1。总审核发现并报告这两项规格遗漏，主 Agent 向项目所有者提出补充裁定。

新条款禁止 read 的 length 字节内容输出与四字节字数输出重叠，以及整个 YanFsInfo 与四字节 cursor 输出重叠；可检测重叠返回 INVALID，不进行 I/O。read 的有效且非 context 别名字数输出清零，list 两输出保持不变；既定状态检查优先级不变。批准依据为项目所有者本轮明确回复：“read 的内容与字节数输出、list 的文件信息与下一项位置输出，均不得彼此重叠；可检测重叠返回 INVALID，不进行 I/O。read 的有效字节数输出清零，list 的输出保持不变。”由总审核角色落笔，VERIFY 已增加完全/部分重叠和不重叠边界验收计划。

补充规格时的状态原文保留：“实现修正与执行验证仍待完成，此次规格修正不表示 YanFS 已通过。”此句描述2026-10-03补充条款当时的状态，已由同日YanFS交付验证取代。实现与永久测试已随 `24b80bb` 落地；2026-10-05总审核用当前源码严格C17、ASan/UBSan独立编译运行核心72例，0失败、0忽略。read/list重叠拒绝、字数清零、list输出保持、无I/O及不重叠/零长度用例均通过；这是本次补充的复核证据，不代替全量回归或用户审查。

## 待定设计

- 后续 RISC-V 指令、特权机制与设备的分阶段支持范围。
- ACT4 的 DUT 配置与 RVMODEL 宏的落地方式。
- YanFS 后续格式演进、事务恢复与层次目录；初版语义已由 0021 锁定。
- 项目许可证。
