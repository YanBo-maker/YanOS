# 0005：ADDI 与单步执行

单步中的 Guest 错误处理已由 [0009：M-mode 同步异常](0009-guest-traps.md) 更新；本文件保留对应阶段的指令规格。

## INTENTION

CPU 已能读取机器码。本阶段实现第一条指令 ADDI，建立一次调用完成一条指令的执行入口。

## SPEC

### 接口与状态提交

`yan_cpu_step(YanCpu *cpu, const YanBus *bus)` 返回 `YanStatus`。每次调用使用当前 PC 取指，识别指令并计算结果，成功后写回目标寄存器，将 PC 增加 4，按 32 位回绕。RAM 保持不变。

取指失败沿用 Bus 状态；CPU、Bus 参数为空返回 `YAN_INVALID_ARGUMENT`。当前只接受 ADDI，其余编码返回新增的 `YAN_UNSUPPORTED_INSTRUCTION`，涵盖尚未实现和无效的编码。所有失败均保留 PC、寄存器与 RAM。该状态属于 Host 接口，Guest 异常机制尚未实现。

下一条指令的地址在下一次调用时检查。因此当前指令成功后，PC 可以指向未映射区域。原有 `yan_cpu_fetch` 继续只读取机器码。

### ADDI

指令的 opcode（位 6:0）为 `0x13`，funct3（位 14:12）为零。rd 位于 11:7，rs1 位于 19:15，12 位立即数位于 31:20。

立即数符号扩展至 32 位，与 rs1 相加，低 32 位写入 rd。x0 读取为零，写入丢弃；rd 与 rs1 相同也使用写回前的源值。标准 NOP 编码作为 ADDI 执行。

C 实现使用无符号位运算完成符号扩展和模加法，避免有符号溢出及负数右移。当前不引入公共解码结构、执行循环或指令计数器。

### 规范依据

采用 [RISC-V ISA 20260120 / RV32I 2.1](https://docs.riscv.org/reference/isa/v20260120/unpriv/rv32.html) 的 Base Instruction Formats、Integer Register-Immediate Instructions 和 NOP Instruction。项目当前仅支持 ADDI，不声明完整 RV32I 兼容性。

## IMPLE PLAN

1. 提交本规格。
2. 在 Unity / CTest 中添加单步测试，确认实现前失败。
3. 添加状态码、接口和 ADDI 执行逻辑，与测试一起提交。
4. 更新实现范围与验证记录，检查 Linux 和 Windows CI。

## VERIFY

验收覆盖立即数的全部 4096 种编码、正负边界、算术回绕、全部寄存器编号、源与目标重叠、x0、NOP、连续单步、PC 回绕、opcode / funct3 拒绝及取指失败的状态保持。测试中的固定机器码独立于实现解码逻辑。

本地与 CI 结果记录在 [STATUS](../STATUS.md)，用户审查单独记录。
