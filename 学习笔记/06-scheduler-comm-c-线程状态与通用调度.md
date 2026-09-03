# `scheduler_comm.c`：线程状态与通用调度学习笔记

> 本笔记对应 `src/scheduler_comm.c`。该文件只放单核、SMP 都通用的线程状态转换与调度辅助逻辑；具体的就绪队列和线程选择算法在 `scheduler_up.c`、`scheduler_mp.c`。

## 1. 调度器文件分层

```text
scheduler_comm.c：通用线程状态、时间片、优先级更新、栈检查
scheduler_up.c  ：单核就绪队列、选取下一线程、上下文切换入口
scheduler_mp.c  ：多核调度扩展
clock.c          ：系统 Tick 推进与时间片/定时器调用链
```

## 2. RT-Thread 的线程状态不是单一枚举

理解一个线程，应同时查看多个维度：

```text
基础状态：thread->stat 的低位
等待原因：线程当前挂在哪个对象的等待链表
超时信息：thread->thread_timer 是否启动、timeout_tick 是多少
调度属性：current_priority、remaining_tick 等
扩展信息：信号、CPU 绑定、SMP 运行位置等
```

### 基础状态

```text
RT_THREAD_INIT     已初始化，尚未 startup
RT_THREAD_READY    可参与调度
RT_THREAD_SUSPEND  不能运行，正在等待
RT_THREAD_CLOSE    已结束，等待资源清理
```

RT-Thread 基础状态中通常没有独立 `RUNNING`。当前真正占用 CPU 的线程由：

```text
thread == rt_cpu_self()->current_thread
```

表示；它的基础状态仍可为 `READY`。

### `SUSPEND` 的细化原因

```text
等信号量：挂在 sem->parent.suspend_thread
等互斥锁：挂在 mutex->parent.suspend_thread；pending_object = mutex
等事件：挂在 event->parent.suspend_thread；event_set / event_info 保存条件
等邮箱接收：挂在 mb->parent.suspend_thread
等邮箱发送空间：挂在 mb->suspend_sender_thread
等消息队列接收/发送：对应 mq 的两条等待链表
延时或带超时等待：thread_timer 已启动
```

因此 `SUSPEND` 只说明“不可运行”；实际等待什么，由对象、链表和线程附加字段共同表达。

### `stat` 的附加位

`stat` 除基础状态外还带有标志：

```text
RT_THREAD_STAT_YIELD：本轮已让出/用尽时间片
RT_THREAD_STAT_SIGNAL 等：信号相关附加状态
```

它们不是新的基础生命周期状态，而是与 `READY`、`SUSPEND` 等组合的信息。

## 3. 线程调度上下文初始化

`rt_sched_thread_init_ctx(thread, tick, priority)`：

```text
stat = INIT
SMP：bind_cpu = RT_CPUS_NR（未绑定）；oncpu = RT_CPU_DETACHED
调用 rt_sched_thread_init_priv() 初始化优先级、时间片等私有数据
```

## 4. 线程专用超时定时器

每个线程内嵌 `thread_timer`，用于：

```text
rt_thread_delay()
rt_thread_delay_until()
信号量/事件/邮箱/消息队列/互斥锁等等待超时
```

`rt_sched_thread_timer_start()` 设置 `sched_flag_ttmr_set`，表示该线程超时定时器在工作。

`rt_sched_thread_timer_stop()` 试图停止 `thread_timer`。失败通常意味着超时中断回调正竞争着唤醒此线程；此时调用者不能继续按“自己已成功唤醒线程”处理。

## 5. 挂起线程恢复为 READY

`rt_sched_thread_ready(thread)` 的核心流程：

```text
确认线程确实是 SUSPEND
→ 若有超时定时器，先停止它
→ 从等待链表移除线程节点
→ 清除 Smart wakeup handler（若启用）
→ rt_sched_insert_thread() 插入就绪队列
```

“先停止超时定时器”用于避免事件/IPC 唤醒与超时 ISR 同时唤醒同一线程的竞争。

## 6. 时间片：`rt_sched_tick_increase()`

通常由系统 Tick 中断调用：

```text
取得当前运行线程
→ 调度器加锁
→ remaining_tick 减去本次推进的 tick
→ 未耗尽：解锁，继续运行
→ 耗尽：调用 rt_sched_thread_yield()，请求重调度
```

`tick` 不一定永远为 1；Tickless 场景可能一次推进多个 Tick。

`rt_sched_thread_yield()`：

```text
remaining_tick = init_tick  为下轮预装时间片
stat |= YIELD               标记本轮让出时间片
```

随后 `rt_sched_unlock_n_resched()` 在解锁时请求调度。Tick ISR 中通常先请求，实际上下文切换在安全的中断退出路径完成。

时间片主要用于同优先级线程轮转；高优先级线程变为 READY 时，即使当前线程时间片没耗尽，仍可能发生抢占。

## 7. 优先级更新

```text
init_priority：创建时配置的基础优先级
current_priority：当前实际优先级，可能被优先级继承临时改变
```

```text
rt_sched_thread_change_priority()
    只改 current_priority
    常用于 mutex 优先级继承

rt_sched_thread_reset_priority()
    同时改 init_priority 与 current_priority
    用于真正修改线程基础优先级
```

若目标线程处于 `READY`，更新优先级时必须：

```text
从旧优先级就绪队列移除
→ 改优先级及位图索引字段
→ 重新插入新优先级就绪队列
```

不能只修改 `current_priority`，否则“字段优先级”和“实际所在队列”会不一致。

`number`、`number_mask`、`high_mask` 是为优先级位图快速查找服务的辅助字段；具体位图算法见 `scheduler_up.c`。

## 8. 可选栈溢出检查

开启 `RT_USING_OVERFLOW_CHECK` 后，`rt_scheduler_stack_check(thread)` 检查：

```text
栈边界的 '#' 哨兵字节是否被覆盖
SP 是否仍处于 stack_addr 到 stack_addr + stack_size 的合法范围
```

检测到溢出后调用可选 hook；未处理时进入死循环，防止带着已损坏内存继续运行。

向下生长栈的预警条件通常是 SP 距低地址栈底只剩约 32 字节，应增加栈大小或减少局部大对象、深层调用和递归。
