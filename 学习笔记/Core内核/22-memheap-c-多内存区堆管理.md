# `memheap.c`：多内存区可变大小堆管理

> 对应 `src/memheap.c`，仅在 `RT_USING_MEMHEAP` 开启时参与编译。它允许把内部 SRAM、外部 SDRAM、DMA 专用 RAM 等多段独立连续内存分别建立为 `rt_memheap` 对象。

## 1. 与 `small mem` 的定位差异

|维度|`mem.c` / small mem|`memheap.c` / memheap|
|---|---|---|
|典型用途|默认系统堆算法|多个独立 RAM 区域的堆对象|
|分配搜索|从 `lfree` 沿物理块扫描|只遍历空闲块链表|
|块关系|物理块链|物理块链 + 空闲块链|
|并发保护|由外层通用 malloc 封装处理|每个 `rt_memheap` 内嵌信号量锁|
|扩大 `realloc`|通常申请、复制、释放|优先吞并后方相邻空闲块以原地扩容|

```text
internal_heap → 内部 SRAM
sdram_heap   → 外部 SDRAM
dma_heap     → DMA 可访问区域
```

应用可明确指定：

```c
rt_memheap_alloc(&sdram_heap, 1024);
rt_memheap_alloc(&dma_heap, 512);
```

## 2. `struct rt_memheap`：一个独立 heap 对象

```text
rt_memheap
├─ parent             rt_object：名称、MemHeap 类型、对象总链表节点
├─ start_addr         本 heap 所管理 RAM 起点
├─ pool_size          对齐后的总大小
├─ available_size     当前可用字节数
├─ max_used_size      历史最大占用量
├─ block_list         物理块链表入口
├─ free_list          空闲块链表表头
├─ free_header        空闲链表哨兵节点
├─ lock               每 heap 一个信号量锁
└─ locked             外部已持锁标记，避免重复获取 lock
```

`rt_memheap_item` 块头同时属于两种关系：

```text
next / prev
→ 物理地址相邻的块，用于前后合并。

next_free / prev_free
→ 只连接空闲块，用于分配搜索。
```

```text
物理块链： [已用 A] ↔ [空闲 B] ↔ [已用 C] ↔ [空闲 D] ↔ [尾哨兵]
空闲块链： free_header ↔ [空闲 D] ↔ [空闲 B] ↔ 回到 free_header
```

已用块保留在物理块链，但必须从空闲块链移除。

## 3. `magic` 与块大小

```c
#define RT_MEMHEAP_MAGIC  0x1ea01ea0
#define RT_MEMHEAP_USED   0x01
#define RT_MEMHEAP_FREED  0x00
```

每个块头携带固定 magic，最低 bit 表示 `USED/FREED`。这可帮助发现块头被破坏或重复释放。

```c
#define MEMITEM_SIZE(item) \
    ((rt_uintptr_t)item->next - (rt_uintptr_t)item - RT_MEMHEAP_SIZE)
```

块大小由“下一个物理块头地址 - 当前块头地址 - 当前块头大小”计算，不另存一个 size 字段。

## 4. `rt_memheap_init()`：建立初始块和哨兵

```c
rt_memheap_init(&sdram_heap, "sdram", sdram_start, sdram_size);
```

初始化步骤：

```text
1. 注册为 RT_Object_Class_MemHeap 对象。
2. 保存起始地址和对齐后的 pool_size。
3. 初始化 free_header 循环空闲链表表头。
4. 在 start_addr 建立一个覆盖绝大部分区域的大空闲块。
5. 将该大空闲块插入 free list。
6. 在末尾建立一个大小为 0、标记 USED 的 tailer 哨兵块。
7. 初始化 heap->lock，初始计数为 1。
```

```text
start_addr
   ↓
[一个大空闲块] [末尾 USED 哨兵块]
```

尾哨兵不分给用户；它阻止最后一个正常块在释放合并时越过 heap 边界。

调用者应保证 `start_addr` 满足 `RT_ALIGN_SIZE` 对齐，并提供足够容纳两个块头和最小空间的区域。源码只向下对齐 `size`，并不替调用者修正不对齐的起始地址。

## 5. `rt_memheap_alloc()`：只扫描空闲块

```c
void *rt_memheap_alloc(struct rt_memheap *heap, rt_size_t size);
```

流程：

```text
请求大小对齐并提升到 RT_MEMHEAP_MINIALLOC
→ 获取 heap->lock（若外部尚未锁定）
→ 沿 free list 找到第一个足够大的空闲块
→ 可切分则生成新空闲块
→ 当前块从 free list 移除、标记 USED
→ 更新 available_size / max_used_size
→ 释放 lock，返回“块头之后”的用户地址
```

切分前后：

```text
原来：[header_ptr 块头 |              大空闲用户区              ]
之后：[header_ptr 块头 | 已用 size ][new_ptr 块头 | 剩余空闲区]
```

`header_ptr` 从 `free list` 移除但留在物理块链；`new_ptr` 同时插入物理链和空闲链。若余量不足“块头 + 最小用户区”，则整块分配，不产生不可用碎片。

## 6. `rt_memheap_realloc()`：原地扩展优先

|条件|行为|
|---|---|
|`ptr == RT_NULL`|等价于 alloc|
|`newsize == 0`|free 后返回 `RT_NULL`|
|缩小且剩余足够|原地切出一个空闲块，并可与右邻居合并|
|扩大且右邻居空闲、空间足够|吞并右邻居，在 `ptr + newsize` 重建较小空闲块；原指针不变|
|其他扩大情况|alloc 新块、复制旧数据、free 旧块；返回地址可能改变|

原地扩容示意：

```text
前：[当前已用块][后方空闲块][后方块]
后：[      更大的当前已用块      ][剩余空闲块][后方块]
```

因此 `memheap` 比本版本 `small mem` 更擅长减少扩容时的数据复制。但仍必须用临时变量接收 `realloc` 结果，失败时保留旧指针。

## 7. `rt_memheap_free()`：校验与前后合并

用户指针回退一个块头即可得到 `header_ptr`：

```text
[块头 header_ptr][用户区 ptr]
                  ↑ free(ptr)
```

释放前检查：当前块 `magic` 是否为 `MAGIC | USED`，以及下一块头是否仍保有正确 magic。这有助于发现重复释放、错误指针和覆盖到后方块头的越界写；但不能替代完整的内存安全检查。

释放步骤：

```text
标记 FREED，增加 available_size
→ 若左邻居空闲：左块吞并当前块
→ 若右邻居空闲：当前块吞并右块，并把右块从 free list 移除
→ 若未左合并：将当前块插回 free list
→ 更新分配跟踪信息，释放 heap->lock
```

```text
[空闲 A][已用 B][空闲 C]
          ↓ free(B)
[空闲 A][空闲 B][空闲 C]
          ↓ 合并
[             空闲 A+B+C             ]
```

默认 free list 顺序主要受释放与切分历史影响；若开启 `RT_MEMHEAP_BEST_MODE`，释放块按大小插入空闲链，搜索更接近最佳适配。

## 8. 信息、系统堆绑定和自动回退

`rt_memheap_info()` 在加锁后读取：

```text
total    = pool_size
used     = pool_size - available_size
max_used = max_used_size
```

开启 `RT_USING_MEMHEAP_AS_HEAP` 后，memheap 可以成为通用 `rt_malloc()` 的底层实现。

若再开启 `RT_USING_MEMHEAP_AUTO_BINDING`：

```text
默认 memheap 分配失败
→ 遍历 MemHeap.object_list
→ 依次尝试其他已登记 memheap
→ 任一成功即返回
```

释放不需要用户记录来源 heap：

```text
用户 ptr → 回退块头 → header_ptr->pool_ptr → 正确 heap → free
```

## 9. 辅助函数和调试命令

`_remove_next_ptr()` 在调用者已持有 `heap->lock` 时，把一个空闲块同时从：

```text
free list
block list
```

移除。它用于 `realloc` 原地扩容时吞并后方空闲块。

开启 `RT_USING_MEMTRACE` 后：

|命令|用途|
|---|---|
|`memheapcheck [name]`|遍历各 memheap，检查 magic、pool_ptr、地址范围和部分链表关系|
|`memheaptrace [name]`|输出 total/free/max，以及物理块大小和分配时记录的短线程名|

`memheaptrace` 遍历物理块链，不只输出已用块；空闲块通常显示空白线程名。`memheapcheck` 只是辅助诊断，当前源码的部分链关系检查不能证明链表完全正确，且它只关闭本 CPU 中断，SMP 下不是严格一致快照。

## 11. 源码锚点：两条链的真实维护

### 分配只扫描 `next_free`，已用块只从 free list 移除

```c
header_ptr = heap->free_list->next_free;
while (header_ptr != heap->free_list && free_size < size)
{
    free_size = MEMITEM_SIZE(header_ptr);
    if (free_size < size)
        header_ptr = header_ptr->next_free;
}

header_ptr->next_free->prev_free = header_ptr->prev_free;
header_ptr->prev_free->next_free = header_ptr->next_free;
header_ptr->next_free = RT_NULL;
header_ptr->prev_free = RT_NULL;
header_ptr->magic = RT_MEMHEAP_MAGIC | RT_MEMHEAP_USED;
```

这里没有改动 `header_ptr->next/prev`，所以块依旧在物理链；仅从空闲链移除并标记 USED。

### 原地扩容：吃掉右邻居，再在新边界建立空闲块

```c
next_ptr = header_ptr->next;
if (!RT_MEMHEAP_IS_USED(next_ptr))
{
    _remove_next_ptr(next_ptr);
    next_ptr = (struct rt_memheap_item *)((char *)ptr + newsize);
    next_ptr->magic = RT_MEMHEAP_MAGIC | RT_MEMHEAP_FREED;
    next_ptr->pool_ptr = heap;
    next_ptr->prev = header_ptr;
    next_ptr->next = header_ptr->next;
    header_ptr->next->prev = next_ptr;
    header_ptr->next = next_ptr;
}
```

`ptr + newsize` 是新的右侧空闲块头位置；因此当前用户区扩大而 `ptr` 不变。

### 释放后的左合并

```c
if (!RT_MEMHEAP_IS_USED(header_ptr->prev))
{
    (header_ptr->prev)->next = header_ptr->next;
    (header_ptr->next)->prev = header_ptr->prev;
    header_ptr = header_ptr->prev;
    insert_header = RT_FALSE;
}
```

左邻居直接跨过原 `header_ptr`；原块头被左块吞并，所以 `insert_header = RT_FALSE`，不能把同一空闲区域重复插入 free list。

```
