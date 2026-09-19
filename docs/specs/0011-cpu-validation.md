# CPU 验证规格

## INTENTION

当前 CTest 主要验证单条指令和模块边界。它们适合定位实现错误，不能单独回答 CPU 在真实 Guest 指令流中是否持续保持架构状态，也不能发现某一步错误后对后续执行的影响。

验证层需要引入一生一芯使用的逐指令差分思路，并接入 RISC-V 官方架构测试。每条已提交指令都应有可记录的架构状态，测试驱动可以把 YanCPU 与参考模型放在同一条指令边界上比较。

## SPEC

验证分成三层：

1. **模块测试**：现有 Unity / CTest，覆盖 RAM、Bus、CPU、CSR 和异常接口。
2. **架构测试**：接入 RISC-V Architectural Compatibility Tests（ACT）及其 RV32I、M、Zicsr、Zifencei 测试，使用 ELF、签名区和 PASS / FAIL 退出协议。
3. **差分测试**：测试驱动加载同一份 Guest 镜像，让 YanCPU 和外部参考模型逐条执行；每次提交比较 PC、32 个通用寄存器和当前支持的 M-mode CSR。首次不一致时保存指令地址、机器码、DUT 状态、参考状态和最近的执行窗口。

架构测试与差分测试属于可选外部验证层。它们需要明确的 Guest 镜像加载、测试退出、签名区和参考模型适配接口，不能由现有单元测试名称代替。

当前的 `yan_run` 是执行器基础版本。配置 `-DYAN_BUILD_TOOLS=ON` 后，可以使用以下参数加载 RV32 ELF、限制步数、轮询 `tohost`、导出签名区和写出逐条架构状态：

```sh
./yan_run --image test.elf --max-steps 1000000 \
  --tohost 0x80001000 --trace dut.jsonl \
  --signature dut.sig 0x80002000 0x80003000
```

它已经具备镜像执行和状态记录能力；参考模型比较、官方测试环境宏和完整 ACT 流程仍需接入。

## IMPLE PLAN

1. 暴露只读的架构状态快照接口，固定差分比较的数据布局。
2. 增加 ELF32 little-endian Guest 加载器和受控执行器，支持入口地址、最大步数、tohost 退出和签名区导出。
3. 增加统一的 JSONL 提交记录格式，并提供参考模型适配器接口；参考模型优先采用 NEMU，架构兼容性验收采用官方 `riscv-arch-test`。
4. 在 Linux CI 中以显式工具链和参考模型版本运行 RV32IM_Zicsr_Zifencei 子集；依赖缺失时保留本地模块测试，CI 不得把跳过外部测试报告为通过。
5. 用第一条不一致指令的最小复现镜像作为回归用例，回归用例进入 CTest。

## VERIFY

当前提交只落地第 1 步，并验证快照不会改变 CPU、Bus 或内存状态。完成第 2～4 步前，项目不宣称通过 RISC-V 架构兼容性测试，也不把现有 12 组 CTest 解释为 CPU 合规性证明。

参考：[一生一芯 DiffTest 介绍](https://oscpu.github.io/ysyx/events/2021-07-17_Difftest/difftest%E6%A1%86%E6%9E%B6%E4%BB%8B%E7%BB%8D.pdf)、[RISC-V Architectural Test](https://github.com/riscv/riscv-arch-test)。
