# `mempool.c`：固定大小块内存池

> 对应 `src/mempool.c`，仅在 `RT_USING_MEMPOOL` 开启时参与编译。内存池预先把一片内存划分为固定大小 block；分配和释放路径短、时间稳定，不产生可变大小堆的外部碎片。

## 1. 适用场景与代价

适合：

```text
网络报文缓冲区、串口接收 buffer、DMA 描述符、固定格式消息、频繁短生命周期对象。
```

|优点|代价|
|---|---|
|分配/释放时间可预测|只能申请固定大小 block|
|不会产生不同大小块造成的外部碎片|小请求也占完整 block，可能有内部浪费|
|block 用尽可选择等待|容量固定，用尽后只能失败或等待|

## 2. 每个 block 的隐藏头部

用户配置的 `block_size` 是用户数据区大小；实际每块还多一个指针大小的隐藏头：

```text
[一个 rt_uint8_t * 管理字段][用户数据区 block_size]
```

实际 block 跨度：

```c
block_size + sizeof(rt_uint8_t *)
```

同一块内存的头字段会复用：

```text
空闲时：保存 next_free，串起空闲单向链表。
已分配时：保存所属 struct rt_mempool *，供 free 自动找回来源池。
```

```text
空闲链：block_list → [block A] → [block B] → [block C] → NULL
```

用户拿到的地址是：

```c
block_ptr + sizeof(rt_uint8_t *)
```

因此管理字段不会暴露给用户。

## 3. `struct rt_mempool` 的关键关系

```text
rt_mempool
├─ parent               rt_object：名称、MemPool 类型、对象总链表节点
├─ start_address        块区域起点
├─ size                 对齐后的区域大小
├─ block_size           对齐后的用户数据区大小
├─ block_list           空闲块单向链表头
├─ block_total_count    总块数
├─ block_free_count     当前空闲块数
├─ suspend_thread       因无 block 而等待的线程链表
└─ spinlock             保护以上可变状态
```

它既像内存组件，也像 IPC：资源是“空闲 block”，资源不足时线程会挂到 `suspend_thread`。

## 4. 生命周期：`init/detach` 与 `create/delete`

|模式|创建|内存来源|销毁|
|---|---|---|---|
|静态|`rt_mp_init()`|用户提供 `struct rt_mempool` 与块区域|`rt_mp_detach()`|
|动态|`rt_mp_create()`|系统堆申请对象与块区域|`rt_mp_delete()`|

`rt_mp_init()`：

```text
对齐 size 和 block_size
→ total_count = size / (block_size + 指针头大小)
→ free_count = total_count
→ 初始化 suspend_thread
→ 将所有 block 串成空闲单向链表
→ 初始化 spinlock
```

无法组成完整 block 的尾部零头不会使用。

`detach/delete` 都会先唤醒 `suspend_thread` 上的所有等待线程，让其得到 `RT_ERROR`；动态 delete 再释放块区域与对象本体。应用仍需先阻止其他线程继续使用 `mp` 指针，避免悬空访问。

## 5. `rt_mp_alloc()`：取固定块，必要时等待

```c
void *rt_mp_alloc(rt_mp_t mp, rt_int32_t time);
```

|`time`|行为|
|---:|---|
|`0`|非阻塞尝试；无 block 时设置 `-RT_ETIMEOUT` 并返回 `RT_NULL`|
|`< 0`|无限等待|
|`> 0`|最多等待对应 Tick 数|

流程：

```text
获取 mp->spinlock
→ free_count 为 0？
  ├─ time == 0：解锁并失败
  └─ 可等待：线程挂入 suspend_thread（FIFO）
             → 有限等待时启动该线程自己的 thread_timer
             → 解锁、rt_schedule()
             → 醒来后按实际经过 Tick 扣减剩余 time，再检查一次
→ free_count--
→ 从 block_list 弹出表头 block
→ block 头由 next_free 改写为 mp 指针
→ 解锁，调用 alloc hook，返回块头后的用户地址
```

等待队列固定使用 `RT_IPC_FLAG_FIFO`，并非优先级排序。

## 6. `rt_mp_free()`：归还资源并唤醒一个等待者

```c
void rt_mp_free(void *block);
```

流程：

```text
block 向前退一个指针大小
→ 读取隐藏的 mp 指针，找回所属内存池
→ 获取 spinlock
→ free_count++
→ 将 block 插入 block_list 表头（LIFO）
→ 若 suspend_thread 非空，移出并唤醒一个等待线程
→ 解锁；若唤醒了线程则请求调度
```

必须准确理解源码顺序：

```text
block 先回到全局 block_list
再唤醒一个等待线程
```

因此这不是“把某个具体 block 直接交给指定等待线程”。被唤醒线程恢复后仍会重新获取锁、检查 `block_free_count`、从 `block_list` 取块；在它真正取到之前，资源并未被保留给它。

若它醒来时资源又被其他线程取走，`rt_mp_alloc()` 会凭 while 循环再次等待；有限等待时间会按实际经过 Tick 扣减，避免每次唤醒后重新获得完整超时额度。

## 7. 所有权与错误使用

`rt_mp_free()` 依赖隐藏头部的 `mp` 指针找回归属，但没有 `memheap` 那样完整的 magic 校验。以下错误风险很高：

```text
重复 free；
将非 mempool 分配的指针传入 rt_mp_free；
传入 block 内部地址而不是起始地址；
释放后继续读写 block。
```

第一次 free 后，块头会从 `mp` 指针改回 next_free 指针；第二次 free 会把 next_free 错当成 `mp`，可能造成非法访问或破坏内存。

所有权规则：

```text
成功 rt_mp_alloc() 的调用方拥有 block；
它必须恰好调用一次 rt_mp_free(block)；
free 后指针立即作废。
```

## 8. Hook

当 `RT_USING_HOOK` 与函数指针 Hook 机制启用时：

```c
rt_mp_alloc_sethook(hook);
rt_mp_free_sethook(hook);
```

只是登记 hook 函数地址；真正的调用点在成功分配后与释放开始处。它可用于调试、统计、trace，不应在 hook 内执行阻塞或破坏内存池状态的操作。

## 9. 源码锚点：固定块链与线程等待的真实实现

### 初始化：每个空闲 block 开头保存下一个 block 地址

```c
mp->block_total_count =
    mp->size / (mp->block_size + sizeof(rt_uint8_t *));
mp->block_free_count = mp->block_total_count;

for (offset = 0; offset < mp->block_total_count; offset++)
{
    *(rt_uint8_t **)(block_ptr +
        offset * (block_size + sizeof(rt_uint8_t *))) =
        block_ptr + (offset + 1) *
        (block_size + sizeof(rt_uint8_t *));
}
*(rt_uint8_t **)(block_ptr +
    (offset - 1) * (block_size + sizeof(rt_uint8_t *))) = RT_NULL;
```

它构造的是 block 地址的单向链，不是 `rt_list_t` 双向循环链。

### 无 block 时：复用线程挂起与超时机制

```c
thread->error = RT_EOK;
rt_thread_suspend_to_list(thread, &mp->suspend_thread,
                          RT_IPC_FLAG_FIFO, RT_UNINTERRUPTIBLE);
if (time > 0)
{
    rt_timer_control(&thread->thread_timer,
                     RT_TIMER_CTRL_SET_TIME, &time_tick);
    rt_timer_start(&thread->thread_timer);
}
rt_spin_unlock_irqrestore(&(mp->spinlock), level);
rt_schedule();
```

线程节点进入 `mp->suspend_thread`，有限超时使用线程自身的 `thread_timer`；这正是 IPC 等待框架在内存池中的复用。

### 取块与归还：同一隐藏头字段的两种含义

```c
/* alloc：头字段原为 next_free，取出后改成 mp */
block_ptr = mp->block_list;
mp->block_list = *(rt_uint8_t **)block_ptr;
*(rt_uint8_t **)block_ptr = (rt_uint8_t *)mp;
return block_ptr + sizeof(rt_uint8_t *);

/* free：回退头字段找 mp，再重新接回空闲链 */
block_ptr = (rt_uint8_t **)(block - sizeof(rt_uint8_t *));
mp = (struct rt_mempool *)*block_ptr;
*block_ptr = mp->block_list;
mp->block_list = (rt_uint8_t *)block_ptr;
```

`free` 的源码顺序是“先重新接入空闲链，再唤醒等待者”，因此没有为被唤醒线程保留某个特定 block。

```
