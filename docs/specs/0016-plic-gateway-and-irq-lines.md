# 0016：PLIC 网关与设备中断线

> 状态：**待审核**（未 Spec Lock）。本规格修订 [0012](0012-machine-interrupts.md) 中 PLIC 网关相关的描述，并取代其中的"简化"约定。审核通过后重新锁定时，会同时在 0012 中加入指向本规格的更新说明。

## INTENTION

0012 为 PLIC 写下了两处简化：没有实现 gateway 的电平锁存，`yan_plic_raise` 无条件置 pending，因此 **in-service 期间的再次 raise 会立刻重新拉起 MEIP**，而 complete 不重新采样源电平。当时平台上没有设备驱动 PLIC 源，这两条简化只影响 Host 直接驱动的测试，代价可以接受。

M1 引入了两个真正每步驱动中断线的设备（UART 接收、传输通道 Host→Guest），简化不再成立，并暴露两个缺陷：

1. **in-service 失去意义**。claim 之后，下一步采样就会把 pending 重新置位，处理程序尚未 complete 时 MEIP 已经再次拉高。in-flight 排除在模型里不存在。
2. **line 的去断言完全没有被表达**。设备只能"raise"，无法表示"我不再请求服务"，于是已撤回的请求看起来仍然 pending。

本规格确定三件事：设备只表达 IRQ 线的电平；平台把线映射到 PLIC 源；**只有 PLIC 决定 MEIP**。

## SPEC

### 分层与职责

| 层 | 职责 | 禁止 |
| --- | --- | --- |
| Device | 以纯函数形式报告"我的 IRQ 线是否 asserted" | 不得持有 `YanPlic *`；不得出现任何 PLIC 概念（源号、pending、claim、MEIP）；不得有副作用 |
| Machine（平台） | 把每条设备线映射到 PLIC 源，并在每次采样时按当前电平驱动该源 | 不得绕过 PLIC 直接置 `mip.MEIP` |
| PLIC | 网关状态机（pending / in-service）与仲裁（enable、priority、threshold） | 不得感知设备语义 |
| CPU | 在指令边界采样 `mip`，据 `mstatus.MIE` 与 `mie` 决定是否进入中断 | — |

### 设备 IRQ 线接口

```c
bool yan_uart_pending(const YanUart *uart);             /* 接收线 */
bool yan_transport_pending(const YanTransport *transport); /* Host→Guest 线 */
```

- 返回值：`true` = 该线当前 asserted（设备正在请求服务），`false` = deasserted。
- **纯函数、无副作用**：调用它不得改变任何设备状态，因此可以每步调用。
- 含义由各设备规格定义：
  - UART：`RX_AVAILABLE && CONTROL.RX_IRQ_ENABLE`
  - Transport：`IRQ_STATUS.H2G_DATA && IRQ_ENABLE.H2G_DATA`
- 设备**不返回 mip 位**，也不接触 PLIC。这一点修订的是设备头文件与 Machine 骨架在 v1 时期的约定（`include/yan/uart.h` 曾要求返回 `YAN_INTERRUPT_MEIP`），不是 0012 的约定——0012 从未规定设备返回 mip 位。

### 平台映射

源号是平台接线，不是设备属性，因此常量位于 `include/yan/machine.h`：

| 源 | 设备线 | 常量 |
| --- | --- | --- |
| 0 | 保留，不可用 | — |
| 1 | 传输通道 Host→Guest | `YAN_MACHINE_PLIC_SOURCE_TRANSPORT` |
| 2 | UART 接收 | `YAN_MACHINE_PLIC_SOURCE_UART` |
| 3 起 | 未分配 | — |

**source 1 与 2 归平台所有。** 平台在每次采样时按设备线当前电平把它们驱动到位的，因此调用方（测试或 Host）对这两个源调用 `yan_plic_set_level` 只会被下一次采样覆盖。需要独立驱动一条源线的调用方应当使用未被接线的编号（现有测试用 5 与 7）。`yan_plic_set_level` 仍是唯一的外部源接口，只是 1/2 号的调用者是平台本身。

### PLIC 网关状态机

每个源 `i`（`1 ≤ i ≤ 31`）持有三个状态：`level[i]`（平台最近驱动的线电平）、`pending[i]`（已锁存的服务请求）、`in_service[i]`（已 claim、尚未 complete）。

| 事件 | 条件 | 动作 |
| --- | --- | --- |
| `set_level(i, true)` | `!in_service[i]` | `level[i]=1`；`pending[i]=1` |
| `set_level(i, true)` | `in_service[i]` | 只更新 `level[i]=1`；**`pending[i]` 保持 0** |
| `set_level(i, false)` | 任意 | `level[i]=0`；`pending[i]=0`（未 claim 的请求被撤回） |
| claim（读 `0x200004`） | 存在满足 `pending & enable & priority > threshold` 的源 | 取优先级最高者、同优先级取编号最小者 `i`；`pending[i]=0`；`in_service[i]=1`；返回 `i` |
| claim | 无合格源 | 返回 0，状态不变 |
| complete（写 `0x200004 = i`） | `in_service[i]` | `in_service[i]=0`；**若 `level[i]` 仍为 1，则 `pending[i]=1`** |
| complete | `!in_service[i]` | 忽略，状态不变 |
| complete | `i == 0` 或 `i > 31` | 忽略，状态不变 |

不变量：

- **I1**：`in_service[i] == 1` 的源必有 `pending[i] == 0`。这是"in-flight 期间不得因每步采样而重新 pending"的形式化表达。
- **I2**：source 0 恒为 `level=0`、`pending=0`、`priority=0`、enable 位为 0；claim 永不返回 0 作为源。
- **I3**：`pending` 只在四处改变——assert 置位、deassert 清零、claim 清零、complete 时 `level` 仍为 1 则置位。
- **I4**：`mip.MEIP` 只由 PLIC 产生。设备与 Bus 都不得直接置位它。
- **I5**：`set_level` 对同一源的重复驱动是幂等的，因此每步驱动与事件驱动等价。

### 完整时序（确认待审的核心行为）

以 UART 接收为例，`RX_IRQ_ENABLE=1`、PLIC 中 source 2 已 enable 且 `priority > threshold`：

1. 设备收到一个字节 → `RX_AVAILABLE=1`，线 asserted。
2. 采样 → `set_level(2, true)`；`in_service[2]=0` → `pending[2]=1`。
3. 仲裁命中 → `mip.MEIP=1` → CPU 在指令边界进入中断。
4. 处理程序读 claim → 返回 2；`pending[2]=0`、`in_service[2]=1` → **MEIP 撤销**。
5. **in-flight**：处理程序运行期间每一步都继续 `set_level(2, true)`（设备仍在请求），但 `in_service[2]=1` → `pending[2]` 保持 0，**不会重新 pending**（I1）。
6. 处理程序读走 `RXDATA` → `RX_AVAILABLE=0`；下一步采样 `set_level(2, false)` → `level[2]=0`。
7. 处理程序写 complete = 2 → `in_service[2]=0`；`level[2]==0` → **不重新 pending**。中断结束，无风暴。
8. **反例（正确行为）**：若处理程序 claim 之后**没有**读走数据，第 6 步不发生，`level[2]` 仍为 1；第 7 步 complete 之后 `level[2]==1` → `pending[2]=1` → MEIP 再次拉高，处理程序被再次进入。请求尚未被满足，这是电平语义的正确表现。
9. **另一个反例（刻意保留的后果）**：若处理程序 claim 之后**从不 complete**，`in_service[2]` 永远为 1，该源从此不再 pending，中断停止。这是刻意的：旧模型会用风暴掩盖"忘记 complete"，新模型让这个 bug 表现为该源静默停摆，而不是淹没系统。

### 采样契约

- `yan_machine_sample_devices(machine)` 对**每一条已接线的设备线**按当前电平驱动 PLIC 源，**两个方向都要驱动**（asserted 调 `set_level(i, true)`，deasserted 调 `set_level(i, false)`）。只驱动 assert 方向是不完整的实现。
- `yan_machine_step` 的顺序保持：`yan_clint_tick` → `yan_machine_sample_devices` → `yan_cpu_step`。设备线在 CPU 采样 `mip` 之前更新，因此一个字节在两步之间到达时，在本步边界就可被观察到。
- **电平的陈旧性有界且不影响正确性**：设备状态只在 `yan_cpu_step` 内部因指令而改变，而每条指令恰好一个 step，所以"改变设备状态"与"complete 该源"必然分处不同指令，complete 时用到的 `level` 最多比真实值旧一条指令——而那一条指令并未改变该设备状态。因此 complete 后的重新 pending 判定不会误判。

### 接口变更

```c
/* include/yan/interrupt.h */
typedef struct {
    uint32_t priority[YAN_PLIC_SOURCE_COUNT];
    uint32_t pending;
    uint32_t level;          /* 新增：各源最近被驱动的线电平 */
    uint32_t enable_m;
    uint32_t threshold_m;
    uint32_t in_service_m;
} YanPlic;

/* 取代 yan_plic_raise(plic, source)。电平语义，双向可驱动。 */
void yan_plic_set_level(YanPlic *plic, uint32_t source, bool asserted);
```

`yan_plic_raise` 被移除。迁移是机械的：`yan_plic_raise(p, s)` → `yan_plic_set_level(p, s, true)`。

`yan_plic_pending` 与 `yan_bus_pending_interrupts` **保持不变**：前者仍是"是否存在合格源"的汇总（因为 in-service 的源其 pending 位已为 0，它自然不参与），后者仍只汇总 CLINT 与 PLIC，外部设备不得绕过 PLIC 直接置 `mip.MEIP`。

### 与 0012 的关系

本规格取代 0012 的以下内容，其余部分继续有效：

| 0012 的表述 | 处理 |
| --- | --- |
| PLIC 一节的"简化（与完整 PLIC 的差异，必须在验证中声明）"整段 | **被取代**。电平锁存与 complete 重采样现已实现 |
| "Host 的 `yan_plic_raise(plic, source)` 是唯一的外部源接口" | **被取代**。唯一的外部源接口是 `yan_plic_set_level`，平台设备线由 Machine 驱动 |
| 仲裁规则中的三条件 `pending & enabled & priority > threshold` | 继续有效；但因 I1，in-service 的源不再满足 `pending` |
| 单 hart、单 M-mode context、source 0 保留、claim/complete 语义 | 继续有效 |

0012 记录的是一个**已经合并并验证过**的阶段，因此按"已发布历史保持稳定，后续修正使用新提交"用新文档取代其被修订的部分，而不是就地改写。这与 0014/0015 的就地升版不同——那两份规格对应的实现尚未落地，就地升版让读者只读一份文件即可。0012 未使用 0005–0008 那种"本文件保留对应阶段的指令规格"标记，是因为它描述的大部分 PLIC 行为（寄存器布局、仲裁规则、claim / complete 语义）**继续有效**，只有网关部分被本规格取代；本次修订已在 0012 的状态行加入指向本规格的更新说明，并在原"简化"段落处标注取代关系。

### 明确不在范围内

不做边沿触发源与 gateway 触发方式配置；不做 gateway 的独立 enable/disable（`enable_m` 已覆盖仲裁需求）；不做 S-mode context 与多 hart；不做中断优先级抢占与嵌套；不做 PLIC 的 `pending` 位软件写入；不做源电平的硬件去抖。

### 规范依据

- PLIC 的 gateway、claim/complete 与电平/边沿源区别：[RISC-V PLIC 规格](https://github.com/riscv/riscv-plic-spec)。
- 本规格采用的偏离与简化：gateway 不提供独立的触发方式配置，电平语义固定；`pending` 寄存器只读。

## 测试计划

### A. PLIC 网关（`tests/test_plic.c`）

| # | 用例 | 断言 |
| --- | --- | --- |
| A1 | assert 后未 claim | `pending` 置位；启用且优先级高于阈值时 MEIP 拉高 |
| A2 | assert 后 deassert，未 claim | `pending` 清零，MEIP 撤销（请求被撤回） |
| A3 | claim 仲裁 | 最高优先级胜出；同优先级取最小编号；`pending` 清零、`in_service` 置位；claim 后 MEIP 撤销 |
| A4 | **in-flight 抑制** | `in_service` 期间反复 `set_level(i, true)`：`pending` 恒为 0、MEIP 恒为 0、`in_service` 不变（I1） |
| A5 | **complete 后重新 pending** | `in_service` 期间 line 保持 asserted，complete 后 `pending` 重新置位且 MEIP 再次拉高 |
| A6 | complete 后不重新 pending | line 已 deassert 时 complete 后 `pending` 保持 0 |
| A7 | complete 幂等与边界 | 对未 in-service 的源 complete 被忽略；`complete(0)`、`complete(≥32)`、`complete(63)`、`complete(UINT32_MAX)` 被忽略且状态不变——**但断言必须在一个真实 in-service 的源存在时做**（`out_of_range_completion_cannot_disturb_a_live_source` 让 source 31 处于 in-service 且线 asserted）。在没有任何源 in-service 时写这些值观察不到任何差异，无法检出"用 `id & 31` 掩码代替范围检查"这类缺陷——独立验证已实测该变异在旧用例下存活 |
| A8 | source 0 保留 | `set_level(0, true)` 无效；`priority[0]` 与 enable 位 0 恒为 0；claim 不返回 0 |
| A9 | 仲裁条件 | **gateway 不看 priority**：线 asserted 且 !in-service 时 `pending` 位照样锁存；`priority == 0` 的源只是**永不参与仲裁**（claim 返回 0、不产生 MEIP）。`priority > threshold` 为严格比较；enable 未置位不参与 |
| A10 | 无合格源 | claim 返回 0 且不改变任何状态 |
| A11 | 多源独立 | 源 A 在 in-service 期间，源 B 的 assert/claim 不受影响 |
| A12 | `pending` 寄存器只读 | 写入被忽略，读数反映网关状态 |

**需刻意反转的既有断言**：`tests/test_plic.c:227` 的 `gateway_is_not_latched`（连同 223–226 行注释）断言"in-service 期间再次 raise 立刻重新拉起 MEIP"。该用例**重写为锁存语义**（即 A4 + A5），而不是删除——旧注释自己声明了"这是一条被测试覆盖的声明，而不是意外"，因此反转也必须留下同等强度的声明。`RUN_TEST` 名称随之更新。

### B. 平台接线（`tests/test_machine.c` / `tests/test_interrupt.c`）

| # | 用例 | 断言 |
| --- | --- | --- |
| B1 | 双向驱动 | 设备线由 asserted 变 deasserted 后，下一次采样把 `pending` 清零 |
| B2 | 采样顺序 | timer tick → 采样 → CPU；设备在两步之间断言时，本步边界即可见 |
| B3 | 不得绕过 PLIC | 设备线 asserted 但该源未 enable（或 `priority == 0`）时 `mip.MEIP` 必须为 0；`yan_bus_pending_interrupts` 只汇总 CLINT 与 PLIC |
| B4 | 结构约束 | `YanUart` 与 `YanTransport` 的成员中不含任何 PLIC 类型或指针（编译期断言或等价的静态检查） |
| B5 | 迁移既有用例 | `tests/test_interrupt.c` 中 `poll_samples_each_device_line` 与 `external_source_interface_drives_meip` 的调用点迁移到 `yan_plic_set_level`，语义断言逐条复核 |

### C. 设备线（`tests/test_uart.c`、`tests/test_transport.c`）

| # | 用例 | 断言 |
| --- | --- | --- |
| C1 | UART 线的真值表 | 仅当 `RX_AVAILABLE && RX_IRQ_ENABLE` 时为 `true`；读走 `RXDATA` 或清 `CONTROL` 后为 `false` |
| C2 | Transport 线的真值表 | 仅当 `IRQ_STATUS.H2G_DATA && IRQ_ENABLE.H2G_DATA` 时为 `true`；清 `IRQ_STATUS` 或清使能后为 `false` |
| C3 | 纯函数 | 连续调用 `pending()` 不改变任何设备状态（前后逐字段比较） |
| C4 | 端到端 | `push_rx` → 采样 → MEIP → claim/complete 且数据仍在 → 重新 pending；随后读走数据 → 线 deassert → 不再 re-pend |

### D. 回归与文档

| # | 项 | 内容 |
| --- | --- | --- |
| D1 | 调用点迁移 | `tests/test_plic.c`（约 20 处）与 `tests/test_interrupt.c`（4 处）的 `yan_plic_raise` 迁移到 `yan_plic_set_level` |
| D2 | 文档 | `docs/STATUS.md` 的 PLIC 简化段更新；`tests/act4/rvmodel_macros.h:81` 的注释同步；`docs/specs/0015` 的源号约定改为引用本规格 |
| D3 | 全量回归 | Debug ASan/UBSan 与 Release 下全量 CTest 通过 |

### E. 变异检查

对 `src/interrupt.c` 注入下列缺陷，确认 A/B/C 三组测试逐一报错、无存活者：

| 变异 | 应被哪条用例检出 |
| --- | --- |
| 去掉 assert 时的 `!in_service` 守卫 | A4 |
| complete 后不重新采样 `level` | A5 |
| deassert 时不清 `pending` | A2 |
| 去掉 source 0 守卫 | A8 |
| MEIP 计算不再要求 enable 与 `priority > threshold` | A9、B3 |

## IMPLE PLAN

1. 本规格与 0014/0015 的修订说明交项目所有者审核；**审核通过前不合并到任何长期分支**（当前骨架可以留在工作分支）。
2. 修改 `include/yan/interrupt.h`（`level` 字段、`yan_plic_set_level`、移除 `yan_plic_raise`）与 `src/interrupt.c`（网关状态机、complete 重采样）。
3. 修改 `src/machine.c` 的双向驱动。
4. 先改测试再改实现：按测试计划 A 重写 `tests/test_plic.c` 的网关部分（含反转 `gateway_is_not_latched`），确认在实现前失败。
5. 迁移调用点（D1），更新文档（D2）。
6. Debug ASan/UBSan 与 Release 全量 CTest，并完成 E 的变异检查。

## VERIFY

见"测试计划"一节。实现、验证与审查状态见 [STATUS](../STATUS.md)。
