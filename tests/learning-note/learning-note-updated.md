@yan-note 1
title: 中断学习（测试夹具）
tag: learning
@end
@block criteria
不看资料，讲清楚一次中断从设备请求到 trap handler 的路径。
说明 CLINT 与 PLIC 各自负责什么。
@block known
CLINT 的 mtimecmp 在计时达到比较值时触发中断。
@block unknown
PLIC 的 claim 与 completion 为什么要成对。
@block untouched
UART 接收中断如何唤醒等待的任务。
@block log
2026-10-21 示例夹具，未进行真实核对；以下一律为模拟信息，不代表闫博的理解。
- 题目：中断从哪里进入处理器。
- 回答要点：示例占位。
- 被指出的问题：示例占位。
- 原文：[[irq#entry]]、[[plic#claim]]、[[trap#handler]]
2026-10-21 测试模拟闫博确认：把一条 unknown 移入 known；这是夹具模拟，不代表真实理解。
- 题目：mtimecmp 在什么时候触发中断。
- 回答要点：模拟答案，仅用于夹具。
- 被指出的问题：模拟记录。
- 原文：[[irq#entry]]、[[plic#claim]]
