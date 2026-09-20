# 0009：M-mode 同步异常

## INTENTION

把 CPU 的指令与访存故障交给 Guest 处理，完成异常进入、CSR 访问和异常返回。当前仅支持 M-mode，采用直接异常入口，不实现 U/S 模式、分页或委托。中断（`mie` / `mip`、CLINT、PLIC）由[机器模式中断规格](0012-machine-interrupts.md)定义。

## SPEC

### 单步与异常进入

`yan_cpu_step` 正常完成返回 `YAN_OK`；进入异常返回新增的 `YAN_TRAP`，本步不执行处理程序首条指令。异常保留通用寄存器和 RAM，更新 mepc、mcause、mtval、mstatus 和 PC。空参数、失效的 Bus / RAM 等 Host 错误保持原状态并返回原错误。

本规格替代早期单步规格中“Guest 错误保持 PC 并返回 Host 错误”的行为。独立 RAM、Bus 和 `yan_cpu_fetch` 接口保持原语义。

| 事件 | mcause | mtval |
| --- | --- | --- |
| 指令地址未对齐 | 0 | 出错取指地址或跳转目标 |
| 取指未映射或跨界 | 1 | 取指地址 |
| 非法指令、未实现扩展、非法 CSR 访问 | 2 | 原始指令字 |
| EBREAK | 3 | 原 PC |
| Load 未对齐 / 访问失败 | 4 / 5 | 有效地址 |
| Store 未对齐 / 访问失败 | 6 / 7 | 有效地址 |
| M-mode ECALL | 11 | 0 |

mepc 保存故障指令 PC，低两位清零。跳转目标错误记录跳转指令位置；目标映射错误在下一次取指发生。取指检查先于解码，非法编码先于数据访问，数据对齐检查先于映射检查。

异常入口令 MPIE=MIE、MIE=0、MPP=3，再把 PC 设置为 mtvec BASE。同步异常不受 MIE 屏蔽。嵌套异常覆盖现有异常 CSR，Guest 负责保存现场。入口未映射时，下一次单步产生新的取指异常；没有 Host 递归执行。

### CSR 范围

| 地址 | CSR | 读写约束与复位 |
| --- | --- | --- |
| 0x300 | mstatus | 仅 MIE(3)、MPIE(7) 可写，MPP(12:11) 固定 3，其余为零；复位 0x1800 |
| 0x305 | mtvec | 仅 Direct，写入清低两位；复位 0 |
| 0x340 | mscratch | 32 位读写，复位 0 |
| 0x341 | mepc | 32 位读写，低两位固定零，复位 0 |
| 0x342 | mcause | 保存 32 位写入值，复位 0；硬件仅产生表中同步异常码 |
| 0x343 | mtval | 32 位读写，复位 0 |
| 0x301 / 0x310 | misa / mstatush | 固定读零，写入忽略；misa 暂不提供 ISA 探测值 |
| 0x304 / 0x344 | mie / mip | 由[机器模式中断规格](0012-machine-interrupts.md)定义，本表不再固定读零 |
| 0xf11～0xf14 | mvendorid / marchid / mimpid / mhartid | 只读零 |

其余 CSR 未实现，Guest 访问产生非法指令异常。提供 Host 的 `yan_cpu_read_csr` / `yan_cpu_write_csr` 用于状态检查和配置；Host 调用失败不触发 Guest 异常。结构体用于观察，CSR 写入经过接口。

实现 CSRRW、CSRRS、CSRRC 及三个立即数版本，遵循 Zicsr 的读写抑制规则。读旧 CSR 值及源操作数后再写回，允许 rd 与 rs1 重合。CSRRS/CSRRC 以 rs1 编号是否为零决定是否写入，寄存器值为零不能免除只读检查。立即数版本以五位 uimm 是否为零决定。

### 返回与系统指令

MRET 的精确编码为 0x30200073：PC=mepc，MIE=MPIE，MPIE=1，MPP 保持 3。MRET 不自动给 mepc 加 4；Guest 决定重试还是跳过。ECALL / EBREAK 仅接受精确编码 0x00000073 / 0x00100073。

补齐 FENCE 与 FENCE.I，opcode=0x0f、funct3=0/1；当前单 hart、同步 RAM 访问且每次取指都重读 RAM，两者都只推进 PC，fm/pred/succ/rs1/rd 不参与运算。该 opcode 下 funct3 的其余编码产生非法指令异常。未实现扩展不再作为普通单步错误返回。

### 规范依据

机器模式依据 [特权架构 20240411，Machine-Level ISA 1.13](https://docs.riscv.org/reference/isa/v20240411/_attachments/riscv-privileged.pdf) 的 mstatus、mtvec、mepc、mcause、mtval、异常优先级和 MRET 章节。CSR 指令依据 [Zicsr 2.0 / ISA 20260120](https://docs.riscv.org/reference/isa/v20260120/unpriv/zicsr.html)，ECALL、EBREAK、FENCE 依据同版 RV32I 2.1。本阶段是机器模式异常子集，不声明完整特权架构符合性。

## IMPLE PLAN

1. 提交规格；添加 CSR 状态、访问约束与测试。
2. 添加异常测试，确认实现前失败，再实现异常转换并更新回归断言。
3. 实现 CSR 指令、ECALL、EBREAK、MRET 和 FENCE，验证 Guest 处理流程。
4. 运行 Unity / CTest、Debug ASan/UBSan、Release 与 CI，更新进度。

## VERIFY

检查每个异常原因和 mtval、故障指令地址、寄存器与内存保持、Host 错误隔离、嵌套异常、无效入口、CSR 掩码、六种 CSR 指令的零操作数与重合字段、MRET 状态恢复。Guest 处理流程放在自动化集成测试中，使用固定机器码与逐步断言。实现、验证与审查状态见 [STATUS](../STATUS.md)。
