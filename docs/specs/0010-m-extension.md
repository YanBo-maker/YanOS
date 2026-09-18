# M 扩展规格

## INTENTION

为 RV32I CPU 增加 RISC-V M 扩展的整数乘除指令，让 Guest 可以使用 32 位整数乘法、除法和余数运算。

## SPEC

实现 RV32M 的八条指令。它们使用 `opcode=0x33`、`funct7=0x01`，由 `funct3` 区分：

| funct3 | 指令 | 结果 |
| --- | --- | --- |
| 000 | MUL | 乘积低 32 位 |
| 001 | MULH | 有符号乘积高 32 位 |
| 010 | MULHSU | 有符号乘以无符号乘积高 32 位 |
| 011 | MULHU | 无符号乘积高 32 位 |
| 100 | DIV | 有符号商 |
| 101 | DIVU | 无符号商 |
| 110 | REM | 有符号余数 |
| 111 | REMU | 无符号余数 |

除数为零时，DIV / DIVU 返回全一，REM / REMU 返回被除数。`INT_MIN / -1` 返回 `INT_MIN`，对应 REM 返回零。除法边界情况不会产生 Guest 异常。

M 扩展保持 RV32I 的寄存器、x0、PC 和单步提交语义；未定义的 `funct7` 编码仍进入非法指令异常。

## IMPLE PLAN

1. 用独立的 Unity / CTest 用例覆盖乘法高低位、四种有无符号组合、除法边界和所有寄存器字段。
2. 在整数 R 型解码中识别 `funct7=0x01`，使用无符号位模式承载结果，避免宿主有符号溢出未定义行为。
3. 运行 Debug（含 ASan / UBSan）和 Release 全量测试，并更新项目状态。

## VERIFY

测试参考 RISC-V Unprivileged ISA 的 M 扩展定义，并使用 64 位宿主整数计算独立期望值。测试确认除零与有符号溢出保持定义结果，非法编码仍保持 Guest 异常路径。

参考：[RISC-V Unprivileged ISA，M 扩展](https://docs.riscv.org/reference/isa/v20240411/unpriv/m-st-ext.html)。
