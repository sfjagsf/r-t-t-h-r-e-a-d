# 25. `signal.c`：线程信号的投递、等待与处理

> 对应源码：`src/signal.c`。本文件仅在启用 `RT_USING_SIGNALS` 时参与编译。

## 1. 信号在 RT-Thread 中是什么

信号是投递给**某一个线程**的异步通知。它不是线程对象本身，也不是 IPC 消息队列。

```text
rt_thread_kill(tid, sig)
        │
        ├─ 在目标线程 tid 中记录待处理信号
        │    ├─ tid->sig_pending：位集合，表示哪些编号的信号待处理
        │    └─ tid->si_list：单链表，保存每条信号的 siginfo_t
        │
        └─ _signal_deliver(tid)：让目标线程有机会处理它
                 │
                 └─ rt_thread_handle_sig()
                         │
                         └─ tid->sig_vectors[signo](signo)
```

这里的“信号”与“杀死线程”不是同义词。`rt_thread_kill()` 的名字沿用 POSIX 习惯，它实际做的是“向 `tid` 投递编号为 `sig` 的信号”；最终效果由这个编号安装的处理函数决定。

## 2. 线程 TCB 中与信号有关的成员

`struct rt_thread` 中的信号字段可理解为一套线程私有的信号收件箱：

```c
rt_sigset_t        sig_pending;  /* 已到达、尚待处理的信号位集合 */
rt_sigset_t        sig_mask;     /* 当前允许处理的信号位集合 */
void              *sig_vectors;  /* 每个信号编号对应的 handler 表 */
void              *si_list;      /* struct siginfo_node 的单链表头 */
```

有效、可处理的信号集合为：

```c
tid->sig_pending & tid->sig_mask
```

含义是“已经到达”且“当前未被屏蔽”的交集。

`sig_mask` 的命名容易造成误解：在本实现中，**位为 1 表示允许处理**。所以 `rt_signal_mask(signo)` 会清该位以屏蔽信号；`rt_signal_unmask(signo)` 会置位以重新允许处理。

## 3. 三层数据表示：位图、信息链表、处理函数表

### 3.1 `sig_pending`：快速判断

它是位图。例如第 3 位为 1，表示 3 号信号待处理。位图适合快速判断“是否存在某号信号”，但不能保存附加信息。

### 3.2 `si_list`：保存 `siginfo_t`

每个 `struct siginfo_node` 包含：

```c
struct siginfo_node
{
    struct rt_slist_node list;
    siginfo_t            si;
};
```

`siginfo_t` 保存 `si_signo`、来源 `si_code`、可选附加值等。节点来自全局固定块内存池 `_siginfo_pool`，而不是每次用普通堆申请。

### 3.3 `sig_vectors`：按信号编号索引的 handler 表

每个可用信号线程持有一张独立表：

```text
tid->sig_vectors[0] → 0 号信号的处理方式
tid->sig_vectors[1] → 1 号信号的处理方式
...
```

`rt_thread_alloc_sig()` 分配此表，并将每个槽初始化为 `_signal_default_handler`。`rt_signal_install()` 随后可替换某个 `signo` 的处理函数。

## 4. 信号不是可无限排队的消息

`rt_thread_kill()` 先检查：

```c
if (tid->sig_pending & sig_mask(sig))
```

若同一个 `sig` 已经挂起，它会在 `si_list` 中找到对应节点，更新 `siginfo_t` 后直接返回，而不是再追加一个节点。

```text
连续投递：SIG_X、SIG_X、SIG_X
结果：pending 位中仍只有一个 SIG_X；通常只保留一条 SIG_X 信息
```

因此普通信号表达的是“某事件已经发生、尚未处理”，不应被当作可累计 N 条独立数据的消息队列。要可靠传递每一笔数据，应使用邮箱或消息队列。

## 5. 源码锚点一：`rt_thread_kill()` 投递信号

核心代码位于 `src/signal.c` 的 `rt_thread_kill()`：

```c
si_node = (struct siginfo_node *) rt_mp_alloc(_siginfo_pool, 0);
...
rt_slist_append(&(si_list->list), &(si_node->list));
tid->sig_pending |= sig_mask(sig);
...
_signal_deliver(tid);
```

逐步理解：

1. `rt_mp_alloc(..., 0)` 从 `_siginfo_pool` 取一个固定块；`0` 表示资源不足时不等待；
2. 将信号信息节点追加到目标线程 `tid->si_list`；
3. 置 `tid->sig_pending` 对应位，使内核可快速发现该信号；
4. 调 `_signal_deliver(tid)`，根据目标线程当前状态决定唤醒、改造返回路径或请求调度。

前两步与 `sig_pending` 的读改写都在 `_thread_signal_lock` 保护下，避免多个 CPU / 中断上下文并发投递时破坏链表或丢失位更新。

## 6. `_signal_deliver()`：让目标线程取得处理机会

投递记录完毕并不等于 handler 已立即运行。`_signal_deliver(tid)` 要结合线程当前状态决定下一步：

```text
目标正在 SUSPEND
  → 恢复为 READY，使其重新可调度

目标就是当前线程且不在中断上下文
  → 标记有信号，并可直接走处理路径

目标是其他运行/就绪线程
  → 标记 SIGNAL / PENDING，必要时修改其返回执行上下文；SMP 下还可能请求其他 CPU 调度
```

这里的 `RT_THREAD_STAT_SIGNAL`、`RT_THREAD_STAT_SIGNAL_PENDING` 是线程调度状态字的附加位，不是 `sig_pending` 位图的替代品：

```text
sig_pending              ：哪几个信号待处理
RT_THREAD_STAT_SIGNAL    ：线程当前有信号处理相关状态
RT_THREAD_STAT_SIGNAL_PENDING：需要在合适的执行点处理信号
```

## 7. 源码锚点二：`rt_thread_handle_sig()` 调 handler

真正执行回调的关键代码：

```c
handler = tid->sig_vectors[signo];
tid->sig_pending &= ~sig_mask(signo);
rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

if (handler) handler(signo);

level = rt_spin_lock_irqsave(&_thread_signal_lock, level);
rt_mp_free(si_node);
tid->error = -RT_EINTR;
```

顺序不可颠倒：

```text
锁内：取节点、从待处理集合移除
锁外：调用用户 handler(signo)
锁内：回收节点、记录 -RT_EINTR
```

用户 handler 是不可控代码，可能耗时、调用其他内核接口甚至再次投递信号。因此必须在调用它之前释放 `_thread_signal_lock`；否则容易死锁，或使所有线程都无法投递/处理信号。

handler 运行后把 `tid->error` 置为 `-RT_EINTR`，表示线程的一次可中断等待或系统调用可能是被信号打断的。上层接口可据此区别“正常获得资源”与“被信号唤醒”。

如果线程正在 `RT_THREAD_STAT_SIGNAL_WAIT`，`rt_thread_handle_sig()` 不会抢先调用异步 handler，因为等待者的语义是由 `rt_signal_wait()` 主动取得 `siginfo_t`。

## 8. `rt_signal_wait()`：同步等待指定信号

调用者给出一个集合 `*set`，例如“等待 2 号或 5 号信号”。函数先检查已到达的 `si_list`；若没有匹配项，线程挂起并启动自己的 `thread_timer`，等待信号或超时。

找到第一个匹配节点后，代码会：

```c
*si = si_node->si;                  /* 返回信号信息 */
tid->sig_pending &= ~sig_mask(signo); /* 清 pending 位 */
rt_mp_free(si_node);                /* 归还固定内存块 */
```

```text
等待集合：{SIG_A, SIG_B}
待处理链表：SIG_C → SIG_B → SIG_A
取出结果：SIG_B
剩余链表：SIG_C → SIG_A
```

若线程定时器到期，函数将线程错误码清回 `RT_EOK`，并向调用者返回 `-RT_ETIMEOUT`，避免超时状态残留在 TCB 中。

## 9. 线程信号资源的生命周期

```text
线程首次需要信号功能
    ↓
rt_thread_alloc_sig()
    └─ 分配 sig_vectors 表，所有槽为默认 handler

信号到达
    ↓
rt_thread_kill()
    └─ 从全局 _siginfo_pool 取 siginfo_node

处理完成 / rt_signal_wait() 取走
    ↓
rt_mp_free(si_node)

线程销毁
    ↓
rt_thread_free_sig()
    ├─ 遍历并释放 si_list 全部节点
    └─ 释放 sig_vectors
```

`rt_thread_free_sig()` 先在锁内将 `tid->si_list` 与 `tid->sig_vectors` 置空，再在锁外回收实际内存。这能先切断并发访问入口，避免持锁执行可能较慢的释放操作。

## 10. `rt_system_signal_init()`：全局信号节点池

```c
_siginfo_pool = rt_mp_create("signal",
                             RT_SIG_INFO_MAX,
                             sizeof(struct siginfo_node));
```

它创建一个数量上限为 `RT_SIG_INFO_MAX` 的全局内存池。优点是分配时间稳定、不会产生普通堆碎片；代价是同时能保存的待处理信号节点有总上限。池耗尽时，`rt_thread_kill()` 返回 `-RT_EEMPTY`。

## 11. 与 IPC 的边界

| 需求 | 更合适的机制 |
|---|---|
| 通知某线程“发生了某类事件” | 信号 |
| 传递每一笔独立数据，不能丢失或合并 | 邮箱 / 消息队列 |
| 计数型资源或事件次数 | 信号量 |
| 多条件状态位同步 | 事件集 |

信号的重点是“异步打断或通知指定线程”；消息队列的重点才是“可靠保存多条独立消息”。

## 12. 本节结论

1. 信号状态分为：`sig_pending` 位集合、`si_list` 信息链表、`sig_vectors` handler 表；
2. 同号普通信号已经待处理时，RT-Thread 更新已有信息，不无限叠加节点；
3. `rt_thread_kill()` 只负责投递，`rt_thread_handle_sig()` 才实际调用 `handler(signo)`；
4. handler 必须在信号锁外执行；处理后 TCB 记录 `-RT_EINTR`；
5. `rt_signal_wait()` 是同步取得指定信号信息的路径，与异步 handler 的语义不同；
6. `RT_USING_SIGNALS` 决定此组件是否编译进入系统。
