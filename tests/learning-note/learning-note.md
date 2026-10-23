@yan-note 1
title: 中断学习（测试夹具）
tag: learning
@end
@block criteria
不看资料，讲清楚一次中断从设备请求到 trap handler 的路径。
说明 CLINT 与 PLIC 各自负责什么。
@block known
@block unknown
CLINT 的 mtimecmp 在计时达到比较值时触发中断。
PLIC 的 claim 与 completion 为什么要成对。
@block untouched
UART 接收中断如何唤醒等待的任务。
@block log
2026-10-21 示例夹具，未进行真实核对；以下一律为模拟信息，不代表闫博的理解。
- 题目：中断从哪里进入处理器。
- 回答要点：示例占位。
- 被指出的问题：示例占位。
- 原文：[[irq#entry]]、[[plic#claim]]、[[trap#handler]]
