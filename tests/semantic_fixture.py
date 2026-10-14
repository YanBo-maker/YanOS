"""Independent YanFS v1 fixture encoder, including physical directory slots."""
import struct
import zlib

def image(files, capacity=None):
    """files: [(ASCII name, bytes)] in physical slot order; None leaves a hole."""
    assert len(files) <= 63
    needed = 1 + sum((len(data)+4095)//4096 for item in files if item for _,data in [item])
    capacity = needed if capacity is None else capacity
    assert capacity >= needed
    result = bytearray(capacity*4096)
    result[:8] = b'YANFS01\0'
    struct.pack_into('<IIII',result,8,1,4096,capacity,63)
    block = 1
    for slot, item in enumerate(files):
        if item is None: continue
        name, data = item
        encoded = name.encode('ascii')
        assert 1 <= len(encoded) <= 31
        at = 64+64*slot
        result[at:at+len(encoded)] = encoded
        count = (len(data)+4095)//4096
        struct.pack_into('<III',result,at+32,len(data),block if count else 0,count)
        if count: result[block*4096:block*4096+len(data)] = data
        block += count
    struct.pack_into('<I',result,60,zlib.crc32(result[:4096]))
    return bytes(result)

def note(title, body):
    return ('@yan-note 1\ntitle: '+title+'\n@end\n'+body).encode('utf-8')

# Fixed before real model queries. Relevance labels name primary and acceptable
# supporting passages; evaluation reports top5 precision and reciprocal rank.
QUALITY_FILES = [
    ('irq.txt', note('外设中断', '外设经PLIC发送中断请求。CPU进入陷阱处理程序读取claim，处理设备事件后写completion。UART在收到字符时可以发出这一通知。\n')),
    ('uart.txt', note('串口等待', 'UART RX_READY表示接收寄存器已有字符。等待任务开启接收中断并挂起，字符到达后由驱动唤醒；不能在等待循环中反复读空寄存器。\n')),
    ('timer.txt', note('定时器', 'CLINT用mtime计数，mtimecmp规定下一次定时中断的期限。mtime达到比较值后，中断挂起，处理程序推进期限。\n')),
    ('disk.txt', note('磁盘持久化', '写入镜像后需要检查刷新与关闭错误。fflush使stdio缓冲交给操作系统，但不承诺突然断电后数据一定保存。\n')),
    ('publish.txt', note('原子发布', '旁置索引先写同目录临时文件，检查完整写入、刷新和关闭，再rename替换旧索引。发布前失败保留旧文件；发布后输出失败保留新索引。\n')),
    ('identity.txt', note('检索身份', '内容SHA256绑定笔记、模型和推理程序。任何逻辑笔记或模型变化都使旧向量索引过期，需要显式重建。修改时间和文件大小不能代替内容身份。\n')),
    ('task.txt', note('协作任务', '任务主动让出CPU或等待事件后，调度器才能运行其他任务。单个任务一直执行而不yield，会阻止其他协作任务得到运行机会。\n')),
    ('utf8.txt', note('文本编码', 'UTF8用一到四个字节表示Unicode标量。分块读取可能截断一个汉字的编码，所以解码器保留尚未完成的字节，不能把读取边界当作字符边界。\n')),
    ('lines.txt', note('换行与出处', 'LF和CRLF都表示行终止。段落分界不能拆开CR与LF；检索结果记录原文件的半开字节范围以及包含元数据头的物理行号。\n')),
    ('vector.txt', note('向量检索', 'Embedding模型把问题和段落转换为数值向量。余弦相似度比较方向，返回相似的原文候选，不生成答案，也不是答案正确概率。\n')),
    ('copy.txt', note('文件副本', '复制为目标分配新的目录槽和数据块，源文件保留。重命名只改变名称，保留数据块；两者都不覆盖已有目标。\n')),
    ('paging.txt', note('页表概念', '虚拟地址通过页表转换为物理地址。隔离进程地址空间通常依靠特权级和分页；本实验阶段仍只有机器模式，分页属于后续计划。\n')),
    ('bus.txt', note('Memory mapped devices', 'A bus decodes an address and routes the access to RAM or a device. Memory mapped device registers expose hardware state through ordinary loads and stores.\n')),
    ('hash.txt', note('Checksums and authentication', 'A checksum detects accidental corruption. An unkeyed hash stored beside editable data does not authenticate the author, because an editor can replace both the data and its checksum.\n')),
    ('garden.txt', note('花园', '番茄需要阳光、适度浇水和排水良好的土壤。根部积水可能造成腐烂，幼苗移植后应观察叶片状态。\n')),
    ('bread.txt', note('面包', '面团由面粉、水和酵母组成。发酵产生气体，揉面形成筋度，烘烤使表皮上色。温度会影响发酵速度。\n')),
    ('music.txt', note('练琴', '练习节拍时可以使用节拍器，从慢速开始，保持稳定拍点，再逐步增加速度。\n')),
    ('walk.txt', note('散步', '雨后公园的小路湿滑，散步时沿平坦道路慢行。树叶上有水滴，湖面映着云。\n')),
    ('budget.txt', note('旅行预算', '旅行预算包含交通、住宿和餐饮。提前记录固定费用，为临时改变行程预留余量。\n')),
    ('tea.txt', note('泡茶', '水温、茶叶用量和浸泡时间会改变茶汤风味。较长浸泡可能增加苦涩，应按个人口味调整。\n')),
    ('archive.txt', note('保留证据', '测试日志记录实际命令、退出码和断言。构建失败、超时或崩溃不能当作测试检出了预期缺陷。\n')),
]
QUALITY_QUERIES = [
    ('设备如何叫醒处理器', ['irq.txt','uart.txt']),
    ('停电后刚写的内容是否还在', ['disk.txt']),
    ('修改笔记以后检索缓存怎么办', ['identity.txt']),
    ('为什么读文件不能从任意字节切开汉字', ['utf8.txt']),
    ('why can one busy cooperative task prevent others from running', ['task.txt']),
    ('how can a damaged derived file be replaced without losing the old copy', ['publish.txt']),
    ('a checksum is not proof of who wrote the data', ['hash.txt']),
    ('如何保持弹奏时节奏稳定', ['music.txt']),
    ('月球上火山形成于什么年代', []),
    ('股票下周价格会涨多少', []),
]
