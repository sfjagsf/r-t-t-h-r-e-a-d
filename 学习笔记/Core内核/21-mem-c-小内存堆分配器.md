# `mem.c`：Small Memory 小内存堆分配器

> 对应 `src/mem.c`，仅在 `RT_USING_SMALL_MEM` 开启时参与编译。它是 RT-Thread 常见默认动态堆实现之一，为动态线程、动态 IPC、动态定时器等对象提供底层内存。

## 1. 它管理的是什么

`mem.c` 接收 BSP 提供的一整段连续 RAM，把它分成可变大小的内存块：

```text
一段连续 RAM
→ rt_small_mem 管理对象 + heap 区
→ heap 被分割为多个物理连续的块
→ 每块由“块头 + 用户数据区”组成
```

```text
┌───────────────────────┬──────────────────────────┐
│ rt_small_mem_item 块头│ 用户数据区               │
│ pool_ptr / next / prev│ rt_smem_alloc() 返回地址 │
└───────────────────────┴──────────────────────────┘
```

用户永远拿到块头之后的地址；块头是分配器的私有元数据。

## 2. 两层管理结构

```c
struct rt_small_mem_item
{
    rt_uintptr_t pool_ptr;
    rt_size_t    next;
    rt_size_t    prev;
};

struct rt_small_mem
{
    struct rt_memory          parent;
    rt_uint8_t               *heap_ptr;
    struct rt_small_mem_item *heap_end;
    struct rt_small_mem_item *lfree;
    rt_size_t                 mem_size_aligned;
};
```

### 块头 `rt_small_mem_item`

|字段|意义|
|---|---|
|`pool_ptr`|所属 `rt_small_mem` 堆地址，最低 bit 同时记录块是否已分配|
|`next`|下一个物理块相对 `heap_ptr` 的字节偏移|
|`prev`|上一个物理块相对 `heap_ptr` 的字节偏移|

`next`/`prev` 不是普通指针。若 `heap_ptr = 0x20000000` 且 `next = 0x80`，下一个块地址是 `0x20000080`。

### 堆对象 `rt_small_mem`

|字段|意义|
|---|---|
|`parent`|`rt_memory` 对象，提供名称、算法名、总量、当前使用量、历史峰值等统计|
|`heap_ptr`|真正可分配 heap 的起点|
|`heap_end`|末尾已用哨兵块，不会分给用户|
|`lfree`|当前已知地址最靠前的空闲块，作为下一次扫描起点|
|`mem_size_aligned`|对齐后的可管理空间大小|

`lfree` 不是空闲块专用链表头；所有块仍通过物理顺序的 `next/prev` 连接。它只是避免每次分配都从 heap 起点扫描。

## 3. 用低位保存“是否已用”

```c
#define MEM_USED(_mem)  (堆地址 | 0x1)
#define MEM_FREED(_mem) (堆地址 | 0x0)
```

堆对象地址按对齐要求放置，最低 bit 本来是 `0`，因此 `pool_ptr` 可以同时携带：

```text
高位：该内存块属于哪个 small_mem 堆
最低 bit = 0：空闲
最低 bit = 1：已分配
```

这是地址低位复用，并不是额外分配一个 `used` 字段。

## 4. `rt_smem_init()`：把 RAM 建成 heap

```c
rt_smem_t rt_smem_init(const char *name, void *begin_addr, rt_size_t size)
```

步骤：

```text
1. 对齐起止地址。
2. 在给定 RAM 前部放置 rt_small_mem 管理对象。
3. 余下部分作为 heap。
4. 建立一个覆盖大部分 heap 的初始空闲块。
5. 建立 heap_end 末尾已用哨兵块。
6. lfree 指向第一个空闲块。
```

初始化后的逻辑布局：

```text
heap_ptr
  ↓
┌──────────────────┐      ┌──────────────────┐
│ 第一个大空闲块   │ ───→ │ heap_end 哨兵    │
│ pool_ptr = FREED │      │ pool_ptr = USED  │
└──────────────────┘      └──────────────────┘
```

哨兵块避免遍历与合并代码在末尾到处编写特殊的空指针/越界判断。

## 5. `rt_smem_alloc()`：扫描、切分、返回数据区

```c
void *rt_smem_alloc(rt_smem_t m, rt_size_t size)
```

总体流程：

```text
请求 size
→ 按 RT_ALIGN_SIZE 对齐
→ 小于 MIN_SIZE_ALIGNED 则提升
→ 从 lfree 起沿物理块 next 扫描
→ 找到第一个容纳得下的空闲块
→ 剩余空间足够则切分，否则整块分配
→ 标记 MEM_USED，更新 used/max/lfree
→ 返回 块头 + SIZEOF_STRUCT_MEM
```

它从 `lfree` 起找到第一个足够大的空闲块即停止，属于带扫描起点优化的首次适配；不是扫描完整个堆后选择最小可用块的最佳适配。

### 切分规则

只有余量至少还能容纳：

```text
一个块头 SIZEOF_STRUCT_MEM
+ 最小数据区 MIN_SIZE_ALIGNED
```

才会切成两块：

```text
分配前：[mem 块头 |              大空闲数据区              ]
分配后：[mem 块头 | 已用 size ][mem2 块头 | 剩余空闲数据区]
```

否则整块交给用户，避免产生无法再使用的微小碎片。

`parent.used` 统计的是已分配块的总大小，包含块头开销；`parent.max` 是历史峰值。

## 6. `rt_smem_realloc()`：缩小优先原地，扩大申请复制

|条件|行为|
|---|---|
|`rmem == RT_NULL`|等价于 `rt_smem_alloc()`|
|`newsize == 0`|释放原块，返回 `RT_NULL`|
|大小相同|直接返回原指针|
|缩小且剩余空间足够成为合法块|原地切出新的空闲 `mem2`，原地址不变|
|扩大|申请新块、复制 `min(old,new)`、释放旧块|

本版本扩大时不尝试把后方相邻空闲块直接并入旧块。因此 `realloc()` 的返回地址不保证等于旧地址。

```c
void *tmp = rt_realloc(ptr, new_size);
if (tmp != RT_NULL)
{
    ptr = tmp;
}
```

不能直接覆盖 `ptr`，否则分配失败时可能丢失原内存地址。

## 7. `rt_smem_free()` 与 `plug_holes()`：释放和合并

```c
void rt_smem_free(void *rmem)
```

释放时先从用户指针回退块头：

```text
[块头 mem][用户区 rmem]
             ↑ free(rmem)

mem = rmem - SIZEOF_STRUCT_MEM
```

随后从 `mem->pool_ptr` 找回所属堆，检查对齐、已用标志、堆类型及地址范围，标记为 `MEM_FREED`，减少 `used`；若该块地址比 `lfree` 更靠前，则令 `lfree = mem`。

`plug_holes()` 再进行两方向合并：

```text
向后：当前空闲块 + 后方空闲块 → 一个大空闲块
向前：前方空闲块 + 当前空闲块 → 一个大空闲块
```

```text
[空闲 A][已用 B][空闲 C]
          ↓ free(B)
[空闲 A][空闲 B][空闲 C]
          ↓ 合并
[             空闲 A+B+C             ]
```

核心规律：**分配时切分，释放时合并。** 这能减少外部碎片，但无法保证任意时刻都存在足够大的连续块。

## 8. 并发边界与调试接口

`rt_smem_alloc()`/`rt_smem_free()` 本身没有在函数体内加锁；通用 `rt_malloc()`、`rt_free()` 的上层封装负责串行化访问，相关封装在 `kservice.c`。

若同时开启 `RT_USING_FINSH` 与 `RT_USING_MEMTRACE`：

|命令|作用|
|---|---|
|`memcheck`|遍历 small heap，检查块偏移、所属堆和链关系是否损坏|
|`memtrace`|显示 heap 的 total/used/max、每块大小及分配时记录的线程名|

`memtrace` 的线程名只是分配当时的短名称，不是调用栈；`memcheck` 能发现堆已坏，但通常不能反推出最早发生越界写入的源代码位置。

## 9. 源码锚点：关键操作的真实实现

### 分配从 `lfree` 开始扫描物理块链

```c
for (ptr = (rt_uint8_t *)small_mem->lfree - small_mem->heap_ptr;
     ptr <= small_mem->mem_size_aligned - size;
     ptr = ((struct rt_small_mem_item *)&small_mem->heap_ptr[ptr])->next)
{
    mem = (struct rt_small_mem_item *)&small_mem->heap_ptr[ptr];
    if ((!MEM_ISUSED(mem)) &&
        (mem->next - (ptr + SIZEOF_STRUCT_MEM)) >= size)
    {
        /* 找到第一块足够大的空闲物理块 */
    }
}
```

循环末尾读取当前块的 `next` 偏移，因此遍历的是全部物理块；条件扣除块头后判断用户区能否容纳 `size`。

### 切分：创建右侧空闲块并返回块头后的数据区

```c
ptr2 = ptr + SIZEOF_STRUCT_MEM + size;
mem2 = (struct rt_small_mem_item *)&small_mem->heap_ptr[ptr2];
mem2->pool_ptr = MEM_FREED(small_mem);
mem2->next = mem->next;
mem2->prev = ptr;

mem->next = ptr2;
((struct rt_small_mem_item *)&small_mem->heap_ptr[mem2->next])->prev = ptr2;
mem->pool_ptr = MEM_USED(small_mem);
return (rt_uint8_t *)mem + SIZEOF_STRUCT_MEM;
```

`mem->next` 被改为新块头位置，原右邻居的 `prev` 改为 `mem2`；`mem` 的低位状态改为 USED。用户拿到的是块头之后的地址。

### 合并：`plug_holes()` 修改物理块的前后关系

```c
nmem = (struct rt_small_mem_item *)&m->heap_ptr[mem->next];
if (mem != nmem && !MEM_ISUSED(nmem) &&
    (rt_uint8_t *)nmem != (rt_uint8_t *)m->heap_end)
{
    nmem->pool_ptr = 0;
    mem->next = nmem->next;
    ((struct rt_small_mem_item *)&m->heap_ptr[nmem->next])->prev =
        (rt_uint8_t *)mem - m->heap_ptr;
}
```

当前块吞并右侧空闲块：当前块直接跨过 `nmem` 指向其右邻居，右邻居再回指当前块；`nmem` 块头失效。
