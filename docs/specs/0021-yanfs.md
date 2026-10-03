# 0021：YanFS

> 状态：**Spec Lock**（2026-10-03 项目所有者批准整体提案）。整体范围、格式、接口、失败契约与回收规则已经批准。实现中如需改变外部行为或不变量，重新走 INTENTION / SPEC / IMPLE 审核。实现、验证与用户审查状态见 [STATUS](../STATUS.md)。

## INTENTION

Guest 已能通过块协议把内容写进 Host 镜像，并在新进程读回。YanFS 在这些块上保存文件名、长度与数据位置，使 Guest 能创建、覆盖、删除文件，并在重新挂载后使用这些内容。全部操作遵守同一格式与分配规则，作为一个文件系统能力实现和交付。

方案依托 [0018 块协议](0018-block-protocol.md)、[0019 协作式运行时](0019-cooperative-runtime.md) 和 [0020 持久化块镜像](0020-persistent-block-image.md)。这些已锁定规格定义的块大小、整帧响应、单 waiter 与文件后端失败边界保持不变。

## SPEC

### 设计选择

1. **一个根目录、连续数据区间。** 每文件占一个连续 extent（起始块与块数），目录最多 63 文件，名称区分大小写。没有子目录、文件描述符或部分写入 API。目录已记录全部分配，因此无需另写位图；从低地址寻找能容纳整个文件的第一段空闲区间。代价是碎片可能让空间充足的磁盘仍无法容纳大文件。
2. **覆盖先写新数据，最后发布目录。** 新区间不能与任一当前文件（包括被覆盖文件）重叠。数据全部成功后只写一个目录块；该写入成功才更新内存目录，旧区间此时成为空闲。这样数据写失败不会主动覆盖旧内容，代价是覆盖时同时保留新旧数据，空间不足时不能借用自己的旧区间。
3. **检测损坏、明确停用，不承诺多块事务恢复。** 目录带 CRC 与严格不变量检查。任何实际 I/O 或响应协议错误停用实例。后端不保证块写原子性，目录写失败可能损坏唯一元数据块；新进程挂载可能看到旧状态、新状态或拒绝损坏，不能承诺一定恢复。没有日志、备份目录、数据 CRC 或断电一致性保证。

### 磁盘布局

块大小 4096 字节。块 0 是唯一元数据块；块 1 到 capacity−1 是文件数据。容量至少 1 块，最多 UINT32_MAX 块；容量还须能由 0020 的 Host 文件大小限制表示。元数据记录的容量必须与实际设备容量一致，不支持自动扩容。

所有多字节整数采用小端，逐字段编码解码，不把 C struct 写进磁盘。未用字段和名称填充字节全部为零。

| 元数据偏移 | 长度 | 字段 | 值 |
| --- | --- | --- | --- |
| 0 | 8 | magic | 字节 `Y A N F S 0 1 0x00` |
| 8 | 4 | version | 1 |
| 12 | 4 | block_size | 4096 |
| 16 | 4 | capacity_blocks | 实际容量，u32 |
| 20 | 4 | entry_capacity | 63 |
| 24 | 36 | reserved | 全零 |
| 60 | 4 | metadata_crc32 | 整块 CRC |
| 64 | 4032 | entries | 63 个 64 字节目录项 |

CRC 覆盖完整 4096 字节，计算时将偏移 60–63 当作零。采用 IEEE 反射 CRC32：初值 0xffffffff，每字节从低位处理，反射多项式 0xedb88320，最后异或 0xffffffff。CRC 结果按小端存储。此处直接定义算法；CRC 用于检测意外损坏，不构成强数据完整性或认证保证。

每目录项的布局：

| 项内偏移 | 长度 | 字段 |
| --- | --- | --- |
| 0 | 32 | name：ASCII 名称、零终止、终止后全零 |
| 32 | 4 | size_bytes：逻辑长度，u32 |
| 36 | 4 | start_block：起始数据块，u32 |
| 40 | 4 | block_count：数据块数，u32 |
| 44 | 20 | reserved：全零 |

空槽位 64 字节全零。有效名称长度 1–31 字节，只允许 `[A-Za-z0-9._-]`，拒绝恰好 `.` 和 `..`；比较区分大小写。名称第一个字节为零但目录项其余字节非零属于损坏。目录无重复名称。

空文件有有效名称，size/start/count 均为零。非空文件 count 必须等于 `size / 4096 + (size % 4096 != 0)`；先确认 `1 <= start < capacity`，再检查 `count <= capacity - start`，避免相加溢出。所有非空 extent 互不重叠。单文件逻辑大小最多 UINT32_MAX 字节，并受到连续空闲块数限制。最后块的逻辑长度之外在新写入时填零，读取只返回逻辑长度以内的内容。

### 格式化

初版格式化是 Host 工具 `yan_mkfs --image FILE --blocks N`：以排他创建方式建立新文件，已有路径一律拒绝，包含符号链接指向已有目标的情形。不改变现有文件，不提供 Guest format 或自动格式化。

工具写出 N 个零块，再写入空目录与 CRC；所有写入、刷新与关闭成功才退出 0。非法参数返回 2，Host 创建、写、刷新或关闭失败返回 5。失败创建可能留下不完整的新镜像，明确报错并保留供检查；不自动覆盖、修复或重试该路径。不得以正常输出或文件存在表示格式化成功。

格式化只把布局和空目录建立起来；文件系统 core 不调用 Host stdio，CPU/Bus/Device 不参与解释磁盘字段。

### 挂载检查

mount 先查询容量，再完整读取块 0 到 scratch，验证后才发布 metadata 缓存。依次检查：实际容量范围、magic/version/block_size/entry_capacity、容量完全一致、CRC、全部保留零、名称规范与填充、空槽/空文件规范、重复名、size/count、extent 范围与两两重叠。

设备容量不在 1..UINT32_MAX 或 magic 正确但 version 不为 1 时返回 UNSUPPORTED；magic 错误及其余字段、CRC、分配不变量失败返回 CORRUPT。验证失败保持 UNMOUNTED，不修复、不格式化、不写介质。目录没有数据 CRC，挂载不读取每个数据块，因此不能保证“目录有效就等于所有内容正确”。metadata CRC 恰好碰撞时仍可能无法检测某些内容改变，不宣传为任意损坏保证。

### 操作与空间分配

- create 要求名称不存在，选最低空目录槽；非空内容选从块 1 起的 first-fit 连续空闲区间。空文件不分配数据块。目录满、名称重名、非法输入或没有合适连续空间，在写介质前失败。
- replace 要求文件已存在，保留目录槽与名称，用新 extent 与长度替代原值；不复用自己的旧 extent。长度为零时不写数据，只发布空文件目录项。
- remove 把该槽改为全零，成功提交元数据后旧 extent 可再次分配；不擦数据、不提供安全删除。
- 分配状态每次从当前已验证目录反推；没有持久位图，也不追踪目录未引用的数据块。失败写到新 extent 的字节可能留在盘上，但重挂载后若有效目录没有引用它们，就视为空闲。
- 每次修改先完成输入与分配检查，再逐块写入新数据，用 scratch 构造候选目录和 CRC。元数据写成功后把 scratch 复制到 metadata 缓存。scratch 在数据写期间用于单块填充，最后才构造目录，无需第三个 4 KiB 缓冲。
- 全部成功响应都沿用 0020 的完整写入与 fflush 边界；不调用 fsync，不把一次成功响应宣传为断电持久性。

例如在 16 块空镜像上格式化后，块 0 是空目录，块 1–15 空闲。新建 `hello.txt` 的 5 字节 `hello`：目录槽 0 记 size=5,start=1,count=1。整文件覆盖为 5000 字节时，块 1 仍在使用，新内容写块 2、3；目录提交后槽 0 改为 size=5000,start=2,count=2，块 1 释放。删除成功后槽 0 全零，块 2、3 释放，块上的旧字节不擦除。

若设备只有块 0、1、2，即使只有 5 字节旧文件也无法按此契约覆盖为 5000 字节：新内容需要两块，同时必须保留旧块 1，只剩块 2，返回 NOSPACE，镜像不变。

### 实例状态、串行化与恢复

实例状态为 UNMOUNTED、MOUNTED、FAULTED，另有 busy 标志覆盖一次操作从入口到返回的全过程。调用方先 init；禁止在活动实例或阻塞操作中再次 init 清状态。

任一活动操作通过 block callback 等待时，其他任务或回调重入对同一实例操作立即返回 BUSY；不绑定固定 task id，不新增 runtime current-task ABI。list/stat 虽只读缓存，也遵守相同 busy 守卫，避免观察未发布的修改。unmount 对 busy 返回 BUSY，不做 I/O、不修复，只清挂载状态和缓存。

I/O 或响应协议错误把实例改为 FAULTED，首次操作返回 IO 或 PROTOCOL，后续全部文件操作和 mount 返回 FAULTED；仅 unmount 可在非 busy 时清本地状态。单纯 Guest unmount/mount 不能清除 Host backend 的 failed 标志：生产恢复须停止当前 yan_run，以新进程重新打开镜像，再 mount。测试后端可以显式 reset/reopen，不能用此测试便利承诺 Guest 可自行恢复 Host 文件。

参数错误、重名、找不到、目录满、空间不足、缓冲错误不停用已挂载实例。元数据失败写不保证介质或旧文件可读，故障后禁止从缓存继续对外返回文件视图。重挂载成功可见旧或新状态；不满足 CRC/不变量则拒挂，不自动整理可疑内容。

### 接口

核心位于 `os/yanfs.h/.c`，仅依赖定宽类型与可注入同步块回调。回调可阻塞并让出给协作式调度器，但返回前必须完成一次块操作。

```c
#include <stdbool.h>
#include <stdint.h>

#define YAN_FS_BLOCK_SIZE UINT32_C(4096)
#define YAN_FS_MAX_FILES UINT32_C(63)
#define YAN_FS_NAME_MAX UINT32_C(31)
#define YAN_FS_HEADER_SIZE UINT32_C(64)
#define YAN_FS_ENTRY_SIZE UINT32_C(64)
#define YAN_FS_VERSION UINT32_C(1)

typedef enum {
    YAN_FS_OK = 0, YAN_FS_END = 1, YAN_FS_INVALID = 2,
    YAN_FS_NOT_MOUNTED = 3, YAN_FS_FAULTED = 4, YAN_FS_BUSY = 5,
    YAN_FS_EXISTS = 6, YAN_FS_NOT_FOUND = 7, YAN_FS_DIRECTORY_FULL = 8,
    YAN_FS_NOSPACE = 9, YAN_FS_CORRUPT = 10, YAN_FS_UNSUPPORTED = 11,
    YAN_FS_IO = 12, YAN_FS_PROTOCOL = 13
} YanFsResult;

typedef enum {
    YAN_FS_IO_OK = 0, YAN_FS_IO_ERROR = 1, YAN_FS_IO_PROTOCOL = 2
} YanFsIoResult;

typedef enum {
    YAN_FS_UNMOUNTED = 0, YAN_FS_MOUNTED = 1, YAN_FS_STATE_FAULTED = 2
} YanFsState;

typedef struct {
    void *context;
    YanFsIoResult (*capacity)(void *, uint64_t *blocks);
    YanFsIoResult (*read_block)(void *, uint32_t lba, uint8_t out[4096]);
    YanFsIoResult (*write_block)(void *, uint32_t lba, const uint8_t data[4096]);
} YanFsBlockIo;

typedef struct { char name[32]; uint32_t size_bytes; } YanFsInfo;

typedef struct {
    YanFsBlockIo io;
    bool initialized;
    YanFsState state;
    bool busy;
    uint32_t capacity_blocks;
    uint8_t metadata[4096];
    uint8_t scratch[4096];
} YanFs;

YanFsResult yan_fs_init(YanFs *, YanFsBlockIo);
YanFsResult yan_fs_mount(YanFs *);
YanFsResult yan_fs_unmount(YanFs *);
YanFsResult yan_fs_list(YanFs *, uint32_t *cursor, YanFsInfo *out);
YanFsResult yan_fs_stat(YanFs *, const char *name, YanFsInfo *out);
YanFsResult yan_fs_read(YanFs *, const char *name, uint32_t offset,
                       uint8_t *out, uint32_t length, uint32_t *read_bytes);
YanFsResult yan_fs_create(YanFs *, const char *name, const uint8_t *bytes, uint32_t length);
YanFsResult yan_fs_replace(YanFs *, const char *name, const uint8_t *bytes, uint32_t length);
YanFsResult yan_fs_remove(YanFs *, const char *name);

/* 纯编码辅助函数，不执行 I/O，不给 Guest 提供格式化设备的入口。 */
YanFsResult yan_fs_format_metadata(uint8_t out[4096], uint32_t capacity_blocks);
uint32_t yan_fs_metadata_crc(const uint8_t block[4096]);
```

公开类型、字段与枚举见下列声明。公开 C struct 只描述内存上下文，其字段不能用于磁盘序列化，也不能由调用者绕过 API 修改状态或缓存。

- 调用者先以 `YanFs fs = {0}` 或等效方式零初始化 context，再调用 init。init 检查全部三个回调与参数，只初始化状态，不做 I/O；io.context 可以为 NULL，由回调自行解释。已初始化的 busy 实例返回 BUSY，MOUNTED/FAULTED 实例返回 INVALID，必须先 unmount，不能 reinit 清掉活动或故障状态。未初始化实例的其他 API 返回 INVALID。mount 只允许 UNMOUNTED；重复挂载返回 INVALID，不隐式卸载。
- list 的 cursor 初始为 0，按物理槽顺序返回下一有效项，并更新为下一槽；到尾返回 END，cursor=63。大于 63 为 INVALID。没有回调，不读文件数据。stat 按名称查缓存，不暴露物理 extent 为上层接口。
- read 的 out 容量由 length 指定，read_bytes 记录已返回的完整前缀。offset 到达或超过 EOF 返回 OK 和零字节；请求超过 EOF 截到 EOF。length=0 时 out 可为 NULL；不存在的名字仍为 NOT_FOUND。必须先用 scratch 完整读出一个块，再复制目标字节到 out。
- 若第 k 个数据块读失败，read 返回 IO/PROTOCOL，read_bytes 只计此前完整确认的前缀；失败块的部分内容不复制，前缀后的 out 不改，并停用实例。参数、状态错误时，有效且不与 context 重叠的 read_bytes 置零；read_bytes 本身无效时返回 INVALID，不能写入它。
- create 重名返回 EXISTS；replace/remove/stat/read 不存在返回 NOT_FOUND。create/replace 长度为零时 bytes 可为 NULL，否则须指向至少 length 字节，内容有效直到调用返回。
- 所有输入、输出缓冲与输出参数不得与 YanFs context（包括 metadata/scratch）重叠；调用方负责可访问范围，核心仍检查可识别的 context 地址重叠与算术溢出。不得靠未对齐 C 指针加载解释磁盘。
- 2026-10-03 经项目所有者批准补充双输出约束：read 的 `out[0..length)` 与 `read_bytes` 的 `sizeof(uint32_t)` 字节范围不得彼此重叠；list 的整个 `YanFsInfo` 输出与 cursor 的 `sizeof(uint32_t)` 字节范围不得彼此重叠。按下文既定状态检查顺序进入参数检查后，可检测重叠返回 INVALID，不执行 I/O。read 的有效且不与 context 重叠的 read_bytes 清零；这次清零可以改变与它重叠的 out 字节，不返回文件内容。list 的 cursor 与 info 输出均保持不变。length=0 的 out 范围为空，不构成重叠。原有 context 禁止重叠条款继续有效。
- YanFs 需要超过 8 KiB，必须静态存储或由调用者持有长期内存，不放在 0019 的 4 KiB 任务栈。单次函数栈不再分配整块临时数组。


初始化的检查顺序为：context 非 NULL → 已 initialized 且 busy 时 BUSY → 已 initialized 且非 UNMOUNTED 时 INVALID → 三个回调均存在 → 初始化成功。调用者零初始化是读取旧状态的前置条件。

挂载错误分类顺序为：容量回调错误按 IO/PROTOCOL 停用 → 实际容量超出支持范围为 UNSUPPORTED且不读块 0 → 块 0 读取错误按 IO/PROTOCOL 停用 → magic 不匹配为 CORRUPT → version 不支持为 UNSUPPORTED → 其余字段、容量一致性、CRC与目录不变量失败为 CORRUPT。格式验证失败均保持 UNMOUNTED且不写设备。

三个回调的未知返回值按 PROTOCOL 停用处理。文件 API 的状态检查顺序为：参数 context 有效且已 initialized → busy → FAULTED → MOUNTED → 该操作的其他参数与名称。unmount 在 initialized、非 busy 时允许 UNMOUNTED/MOUNTED/FAULTED，清零两块缓存和 capacity，保留 io 与 initialized。普通失败不修改 list/stat 输出；list 到 END 时只更新 cursor=63。

format_metadata 要求 out 指向可写的完整块且 capacity_blocks>=1，生成规范空目录并计算 CRC；无效参数返回 INVALID 且不写输出。metadata_crc 要求可读的完整块，忽略其中 CRC 字段计算，不修改输入；这是函数的调用前置条件。以上纯辅助函数不改变实例状态。

### Guest 适配公开接口

`os/yanfs_block.h` 的内存上下文与入口如下；请求配对记录不包含块缓冲。

```c
typedef struct {
    bool initialized;
    bool busy;
    bool failed;
    uint16_t next_tag;
    uint8_t pending_op;
    uint16_t pending_tag;
    uint32_t pending_lba;
} YanFsBlockAdapter;

YanFsResult yan_fs_block_init(YanFsBlockAdapter *);
YanFsBlockIo yan_fs_block_backend(YanFsBlockAdapter *);
```

调用者先零初始化 adapter；init 只检查通道配置、清空配对记录，不发送请求或修改环指针。已 initialized 的 busy/failed adapter 不可 reinit 来清状态，分别返回 BUSY/FAULTED。通道前置条件失败返回 PROTOCOL。backend 返回三个同步回调，context 指向 adapter；调用未初始化或已 failed adapter 返回 YAN_FS_IO_PROTOCOL。next_tag 从 0 开始，每次成功提交请求后以 u16 回绕递增，pending 字段记录本次已提交请求。共享 channel 的独占是调用方前置条件，不能由不同 adapter 的各自 busy 标志推导为自动互斥。

### Guest 适配与实际硬件约束

Guest 适配位于 `os/yanfs_block.h/.c`，将回调映射到 block.c + task_wait。整个 transport 由该 adapter 独占，禁止另一个独立 block caller、Host RPC 消费者或 framing 外消费者干扰；初始化前要求 HOST_READY、max_count>=1、两环为空且无 overflow，IRQ route 已配置、调用方在任务上下文且中断使能，Host 正常 pump。

adapter 严格“一请求→完整取回响应→下一请求”，全部数据请求 count=1。上述约束下空 G2H 环总能放入一块写帧，所以 submit AGAIN 是通道契约异常：返回 PROTOCOL 并停用，不能盲目等 H2G_DATA。现有硬件没有 G2H 空间可用中断，不能承诺这样的等待一定能醒。

响应未齐时用只读 predicate 与 task_wait，醒后复检。predicate 不修改 head/tail、不消费、不匹配 tag 来决定睡眠：不足 16 字节继续等；头已齐时从真实 op/status/count 推导合法长度，错误响应仅 16 字节即就绪。未知 op、非法 status/count/reserved、不可容纳的总帧长度应让 adapter 退出为 PROTOCOL，不等待永远不可能的载荷。错 tag 不能让 predicate 永久睡眠，完整帧由 take 后再核对 op/tag/lba/count/status/length。读请求收到 status=2,count=0 的 16 字节错误头，必须立即返回 IO 并使 FS FAULTED。

成功读响应为 4112 字节，容量成功响应 24 字节，写成功与所有错误响应 16 字节。错误状态与数据形态必须匹配，成功读/写 count 必须为 1，容量成功 count=0、lba=0、payload=8。status=2 映射 IO；本 adapter 只发合法且受支持的请求，status=1/3、未知 status 或结构/配对错误映射 PROTOCOL。对带错 tag 但合法大载荷的完整帧正常消费后拒绝；对明显畸形头可以直接停用 adapter，未清理通道只能在新进程恢复。

本阶段没有 clock deadline API；合法头的载荷或 Host 响应永久不来，可能持续阻塞。前置条件是正常 Host pump，不新增 timeout runtime ABI，也不把步数上限当作文件系统超时返回。验证会用受控完整错误响应检验错误退出，挂起探针只作为不完整外部响应的限制证据。

### 不在初版范围

不做子目录、权限、链接、rename、append、seek 文件句柄、随机写、缓存一致性、多挂载实例共享设备、并发事务、数据校验、日志、备份元数据、在线扩容、碎片整理、安全擦除或自动修复。范围若扩展需要另审不变量与格式演进。

## IMPLE PLAN

1. 按本规格在短期任务分支实现整体文件系统，保持 CPU / Bus / Device、Host / Guest 分层。
2. 写 Host 可执行的 FS 核心测试，注入内存块回调、读写失败和回调期间重入；先确认序列化、CRC与挂载拒绝用例失败，再实现核心。数据与元数据候选都用 context 的两缓冲。
3. 实现 create/replace/remove、first-fit 与 cache 发布，配套整体验证和失败逐点注入；读错误前缀与 BUSY单独钉住。
4. 实现 Host mkfs，新路径排他创建与失败退出；用独立 Host 解码核对格式，不能靠 FS 自己 encode/decode互相证明。
5. 实现 Guest adapter，响应就绪 predicate 与 task_wait；用真实 16字节错误、错tag、部分帧和非法头驱动，不复制组合测试只识别成功帧的等待器。
6. Guest完成格式化镜像的mount/list/read/create/replace/remove序列，退出后由新进程挂载读回；Host独立核对数据块、目录、空槽与回收后的再分配。
7. 注册必要默认测试及专项变异门禁，变异绑定 owning assertion，复验负控；运行 Release、ASan/UBSan 默认回归与所有注册变异。总审核读监控、独立审查、文档自洽后按逻辑提交，保留教学证据。

## VERIFY

| 面 | 输入与状态变化 | 必须守住的结果 |
| --- | --- | --- |
| 格式化 | 新路径1块、若干块；已有普通文件/链接；创建写刷新关闭故障 | 有效空目录；不覆盖已有路径；错误返回5，不宣称成功 |
| 挂载 | 各头字段/CRC/保留位/名称填充/重复名/空槽与空文件/extent范围与重叠逐项变坏 | 不写磁盘，拒绝非法格式；未知版本UNSUPPORTED，损坏CORRUPT |
| 文件边界 | 0、1、4095、4096、4097、5000字节；首/末可用块 | 内容和长度正确；尾部填零；所有算术无回绕 |
| 名称与目录 | 31/32字节、大小写、./..、非法字符、63满槽 | 稳定结果；输入错误无I/O；list按slot、删除后的空槽再利用 |
| 分配与回收 | first-fit；有足够总空闲却没有连续区间；覆盖新旧共存；删除后重建 | NOSPACE不改介质；其他文件不变；回收不需第二个持久块更新 |
| 读接口 | 跨块与EOF；第2块读故障；缓存别名 | 完整前缀计数、失败块不泄漏、FAULTED拒绝后续操作 |
| 双输出重叠 | read 的 out/read_bytes 完全或部分重叠；list 的 info/cursor 完全或部分重叠；相邻不重叠与 length=0 | 可检测重叠 INVALID、无I/O；read有效字数输出清零，list两输出不变；非重叠输入保持正常行为 |
| 修改失败 | 每一个数据写及最终metadata写前/后注入错误 | 未发写的校验失败不改介质；I/O后FAULTED；失败不承诺回滚 |
| 重挂载 | 数据写失败留下未引用块；metadata短写/CRC损坏；新进程 | 有效目录可挂载且只引用有效extent，损坏拒绝，不自动修复 |
| 缓存发布 | callbacks在数据写/metadata写阻塞时重入list/remove/unmount | BUSY；磁盘成功前不发布新目录；没有第二waiter |
| Guest响应 | read收到16字节设备错误、错tag、半头、分段载荷、非法count/op/status/reserved | 错误能退出；合法半帧不消费；不会等待固定4112而漏错误 |
| Guest前置 | HOST_READY/max_count/overflow/非空环/独占规则/提交AGAIN | 启动或协议错误显式失败，不能伪造等待G2H空间 |
| 端到端 | hello 5字节→5000字节覆盖→删除→再分配，进程间独立读回 | 所有8项整体能力可观察；Host单独解码元数据与逐字节检查 |
| 检测力 | extent重复、越界、坏CRC接受、提前发布cache、覆盖复用旧extent、错误响应挂起 | 对应owner断言检出；无关断言/无行为变化/崩溃/挂起不计检出 |

实现与验证结果以 STATUS 的本阶段记录为准；本规格的 VERIFY 是验收计划，不表示已通过。
