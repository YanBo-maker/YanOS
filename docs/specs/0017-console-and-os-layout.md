# 0017：控制台与 `os/` 层

> 状态：**Spec Lock**（2026-09-20 项目所有者审核通过）。实现中如需变更接口、数据结构或行为，走 INTENTION / SPEC / IMPLE 审核，不得直接修改本文件；`os/console.h` 与 `os/platform.h` 是随本规格冻结的接口。

## INTENTION

[0015](0015-uart-device.md) 让 Guest 有了字符设备，[0014](0014-host-transport-channel.md) 让 Guest 有了通用通道，但两者都只是设备：没有任何代码把"一行输入"翻译成设备操作，也没有地方安放 YanOS 自己的系统代码——目前所有 Guest 侧代码都住在 `tests/guest/`，那是**验证程序**的家。

本阶段建立两件事：`os/` 这一层，以及它的第一个成员——控制台。控制台是 M1 验收"Guest 能说话"的直接载体，也是第一个真正消费设备契约的 YanOS 组件：0015 规定驱动在 `CONNECTED = 0` 时不得无限轮询，这条契约只有存在一个驱动时才可被违反，也只有在存在一个驱动时才可被验证。

## SPEC

### `os/` 目录与分区

| 路径 | 内容 | 说明 |
| --- | --- | --- |
| `os/` | YanOS 内核代码 | 与 `src/`、`include/`（Host 侧）平行 |
| `os/platform.h` | Guest 侧平台 MMIO 视图 | 裸机代码不能包含 `include/yan/*.h`，故重述常量 |
| `os/console.h` / `os/console.c` | 控制台驱动 | 本规格的主体 |
| `tests/guest/` | 验证程序 | **不变**；它引用 `os/`，而不是被 `os/` 引用 |

依赖方向是单向的：`os/` 不得包含 `tests/` 下的任何头；`tests/` 可以包含 `os/`。

### Guest 侧平台视图与防漂移

`os/platform.h` 重述 UART 的地址、偏移与位定义，并给出四个访问器：`yan_os_uart_status`、`yan_os_uart_connected`、`yan_os_uart_tx_ready`、`yan_os_uart_put`、`yan_os_uart_get`。

重述会漂移，因此沿用仓库既有的办法：`tests/test_boot.c` 已经逐条比对 Guest 与 Host 的 CLINT / PLIC / CSR 常量，本规格要求把 UART 的一组也纳入同一比对。**比对失败即测试失败**，而不是靠人去记得同步。

`os/platform.h` 中的 `yan_os_uart_put` 与 `yan_os_uart_tx_ready` 读同一个状态位：这样"报告就绪"与"写入被接受"在一次观察内不可能不一致，与 0015 的不变式同源。

### 控制台接口

```c
typedef enum { YAN_OS_OK = 0, YAN_OS_UNAVAILABLE, YAN_OS_INVALID_ARGUMENT } YanOsResult;

int          yan_os_console_connected(void);
YanOsResult  yan_os_console_putc(char c);
YanOsResult  yan_os_console_puts(const char *text);
YanOsResult  yan_os_console_getline(char *buffer, unsigned capacity, unsigned *length);
```

**没有任何阻塞调用，这是刻意的。** 0015 规定 `CONNECTED = 0` 时驱动不得进入等待；如果接口里存在一个"会等"的函数，这条契约就只能在实现里靠自觉遵守。把它编码进签名——每个入口在无终端时立即返回 `YAN_OS_UNAVAILABLE`——契约就变成可测试的。

### 行为

**输出**。`putc` 先读 `TX_READY`：为 1 时写 `TXDATA` 并返回 `YAN_OS_OK`；为 0 时返回 `YAN_OS_UNAVAILABLE`，不重试、不缓冲。`puts` 逐字符调用 `putc`，**在第一个被拒的字符处停止并返回该结果**——已经写出的部分保留，因为"部分输出 + 明确错误"比"静默截断"或"无限重试"都好定位。传 `NULL` 返回 `YAN_OS_INVALID_ARGUMENT`。

**行编辑**（`getline`）：

| 输入 | 行为 |
| --- | --- |
| 可打印字符（`0x20`–`0x7e`） | 存入缓冲并回显 |
| `0x08` 或 `0x7f`（退格） | 缓冲非空时删除最后一个字符，并回显 `"\b \b"`；缓冲为空时什么都不做 |
| `0x0d`（CR）或 `0x0a`（LF） | 结束该行：回显 `"\r\n"`，写入 NUL 终止符，返回 `YAN_OS_OK` |
| 其它控制字符 | 忽略 |
| 缓冲已满（已存 `capacity - 1` 个） | 忽略后续可打印字符，**不溢出、不结束行**；换行到达时正常返回，`length == capacity - 1` |

CR 之后紧跟的 LF 视为同一行的结束并被吞掉（一位状态即可），这样 CRLF 与单独的 LF 都只产生一行。

**驱动契约（来自 0015，这里是它的兑现）**：

1. `getline` 与 `putc` 在 `CONNECTED = 0` 时**立即返回** `YAN_OS_UNAVAILABLE`，不进入轮询。
2. `getline` 在 `CONNECTED = 1` 时轮询 `RXDATA`。轮询循环必须在**每次迭代都重新检查 `CONNECTED`**：终端中途消失时返回 `YAN_OS_UNAVAILABLE`，而不是永远等一个不会到来的字符。
3. `CONNECTED = 1` 但长期没有输入时，`getline` 会一直轮询——这是 M1 可以接受的，因为还没有协作式运行时可以让出。**中断驱动或可让出的版本属于 M2**，见"明确不在范围内"。

**headless 行为**：调用方（测试程序或未来的 shell）在 `yan_os_console_connected() == 0` 时应当跳过读行、把输出当作被丢弃，并继续推进业务逻辑。控制台不提供"回退到某个默认输入"的行为。

### 明确不在范围内

不做 ANSI 转义序列与光标控制；不做历史记录与行内编辑（左右键、Home/End）；不做 Tab 补全；不做多控制台与终端热切换；不做中断驱动的输入（等 M2 的协作式运行时）；不做字符编码转换（只处理单字节 7 位 ASCII，非 ASCII 字节按可打印字符原样传递）。

### 规范依据

- 驱动契约与 `CONNECTED` / `TX_READY` / `RX_READY` 的语义来自 [0015：UART 字符设备](0015-uart-device.md)。
- 防漂移比对的既有做法见 [0013：Guest 启动与统一 trap 环境](0013-guest-trap-environment.md) 与 `tests/test_boot.c`。

## 测试计划

### A. Host 层（`tests/test_boot.c`，不需要交叉工具链）

| # | 用例 | 断言 |
| --- | --- | --- |
| A1 | 常量防漂移 | `os/platform.h` 的 UART 基址、五个偏移、三个状态位、两个中断位与 `include/yan/uart.h` 逐条相等 |
| A2 | 源号防漂移 | `YAN_OS_PLIC_SOURCE_TRANSPORT/UART` 与 `include/yan/machine.h` 的 `YAN_MACHINE_PLIC_SOURCE_*` 相等 |

### B. Guest 层（`tests/guest/run_console.sh`，需要交叉工具链）

新增一个 Guest 自检程序，直接对 `os/console.c` 的出口行为断言：

| # | 用例 | 断言 |
| --- | --- | --- |
| B1 | 无终端时立即返回 | 未接 backend 时 `connected()==0`，`putc` / `puts` / `getline` 都返回 `YAN_OS_UNAVAILABLE`，且**不挂起**（用步数上限证明它没有循环等待） |
| B2 | 输出逐字节 | 接入 backend 后 `puts("hi")` 返回 `YAN_OS_OK`，Host 捕获到恰好 `h`、`i` |
| B3 | 输出被拒 | `TX_READY = 0` 时 `putc` 返回 `YAN_OS_UNAVAILABLE` 且没有字节被交付；`puts` 在第一个被拒字符处停止 |
| B4 | 行编辑 | 输入 `ab` + 退格 + `c` + LF → `getline` 返回 `YAN_OS_OK`、`buffer == "ac"`、`length == 2`，回显包含退格序列 |
| B5 | 缓冲边界 | 输入长度大于 `capacity` 时返回 `length == capacity - 1`、以 NUL 结尾、**不越界写**（哨兵字节不变） |
| B6 | CRLF | `a` CR LF `b` LF 产生两行 `"a"` 与 `"b"` |
| B7 | 终端中途消失 | `getline` 轮询中 Host 断开 backend，函数返回 `YAN_OS_UNAVAILABLE` 而不是继续等待 |
| B8 | 无效参数 | `getline(NULL, ...)`、`capacity == 0` 返回 `YAN_OS_INVALID_ARGUMENT` |

### C. 变异检查

对 `os/console.c` 注入至少 6 个缺陷：无终端时进入轮询、`putc` 不检查 `TX_READY`、`puts` 忽略被拒字符继续写、退格不检查空缓冲、缓冲满时溢出写、CR 之后不吞 LF。确认 B 组逐一报错、无存活者，且变异不留在仓库。

## IMPLE PLAN

1. 本规格交项目所有者审核并锁定。
2. `os/platform.h`、`os/console.h`、`os/console.c` 三个文件的接口已随本规格落地；锁定后由实现任务填充 `console.c`，**不得修改两个头文件**（如需变更走 INTENTION / SPEC / IMPLE）。
3. `tests/guest/run_trap_env.sh` 一类的脚本编译 Guest 程序时增加 `-I <repo>/os` 并把 `os/console.c` 加入源列表；新增 `tests/guest/run_console.sh` 与对应的 CTest 注册（依赖交叉工具链时 `SKIP_RETURN_CODE 77`）。
4. `tests/test_boot.c` 增加 A1 / A2 两组比对。
5. 先写 B 组用例确认失败，再实现，然后跑变异与全量回归。

## VERIFY

见"测试计划"。实现、验证与审查状态见 [STATUS](../STATUS.md)。
