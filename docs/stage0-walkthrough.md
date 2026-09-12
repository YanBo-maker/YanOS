# 阶段 0：一次内存访问

阅读入口是 [memory_demo.c](../examples/memory_demo.c)。它创建一台机器、装入四个字节，再通过 Bus 读回数据。示例中的机器码作为普通字节保存，当前阶段尚无指令执行。

## 创建机器

```c
YanMachine machine = {0};
yan_machine_init(&machine);
```

零初始化让对象处于可初始化状态。实际程序应检查初始化返回值，完整示例已包含错误处理。

[machine.c](../src/machine.c) 分配 32 MiB RAM，并让 Bus 关联这块 RAM。Machine 拥有缓冲区，Bus 借用 RAM 引用，因此活跃的 Machine 应保留在原位置，释放前避免按值复制。

## Guest 地址与 RAM 偏移

Bus 将 RAM 映射到 `0x80000000`。访问 `0x80000004` 时，对应 RAM 偏移为：

```text
0x80000004 - 0x80000000 = 4
```

Guest 地址通过这个转换定位字节。[bus.c](../src/bus.c) 先检查地址是否对齐、是否属于 RAM 区域，再把偏移传给 RAM。Host 指针始终由 RAM 模块根据已检查的偏移计算。

## 小端读写

示例装入 `93 02 70 00`。从映射起点读取 4 字节时，[ram.c](../src/ram.c) 按位组合得到 `0x00700293`。

低地址字节进入结果的低 8 位。逐字节组合让这种行为独立于 Host 的字节序，也避免通过强制指针转换读取未对齐数据。1、2 字节读取的结果使用无符号 32 位值，高位补零。

## 访问失败

4 字节访问要求 Guest 地址为 4 的倍数。示例向 `0x80000001` 写入时，Bus 返回 `YAN_UNALIGNED`，RAM 保留原值。

读取 `0x10000000` 返回 `YAN_UNMAPPED`。地址落在 RAM 内、但整个访问超出末尾时，返回 `YAN_OUT_OF_BOUNDS`。错误结果同时保存地址、宽度和读写种类，便于定位问题。

边界检查使用减法验证剩余空间，避免计算末地址时整数回绕。[RAM 测试](../tests/test_ram.c) 包含 `SIZE_MAX` 偏移，[Bus 测试](../tests/test_bus.c) 覆盖 `0xFFFFFFFF` 附近的访问。

## 复位与释放

`yan_machine_reset` 清零已分配的 RAM，保留映射。`yan_machine_load_image` 覆盖镜像范围并保留后面的内存，单独装载失败时原内容保持不变。

`yan_machine_destroy` 解除 Bus 引用并释放 RAM。释放后的对象可以再次初始化。[Machine 测试](../tests/test_machine.c) 验证了这个过程以及两个实例之间的独立性。

构建后执行 `ctest --test-dir build -C Debug -V`，可以看到各组行为用例及示例输出。
