# `cpu_mp.c`：多核 CPU 对象与自旋锁学习笔记

> 本笔记对应 `src/cpu_mp.c`，仅在 `RT_USING_SMP` 开启时参与编译。`MP` 表示 Multiprocessor：多个 CPU 能真正并行执行内核代码。

## 1. 每核一个 `struct rt_cpu`

```c
static struct rt_cpu _cpus[RT_CPUS_NR];
```

例如双核：

```text
_cpus[0] -> CPU0 的 current_thread、idle_thread、中断嵌套数、就绪队列等
_cpus[1] -> CPU1 的 current_thread、idle_thread、中断嵌套数、就绪队列等
```

因此 CPU0、CPU1 可以同时运行不同线程；`current_thread` 是每 CPU 状态，并不是全系统唯一变量。

```c
struct rt_cpu *rt_cpu_self(void)
{
    return &_cpus[rt_hw_cpu_id()];
}
```

`rt_cpu_self()` 返回本核 CPU 对象；`rt_cpu_index(n)` 返回指定编号 CPU 对象。跨核访问对方 CPU 的数据必须遵守相应同步规则。

## 2. 真正的硬件自旋锁

```c
void rt_spin_lock(struct rt_spinlock *lock)
{
    rt_enter_critical();
    rt_hw_spin_lock(&lock->lock);
    RT_SPIN_LOCK_DEBUG(lock);
}
```

在多核场景中：

```text
CPU0 持有 lock
CPU1 尝试 rt_hw_spin_lock(lock)
  -> 原子操作失败
  -> CPU1 忙等（spin）
CPU0 解锁
  -> CPU1 以原子方式获得 lock
```

两层保护分别解决不同问题：

| 操作 | 防护对象 |
|---|---|
| `rt_enter_critical()` | 本 CPU 当前线程被调度切走 |
| `rt_hw_spin_lock()` | 其他 CPU 同时进入临界区 |

`rt_spin_unlock()` 的顺序是先释放硬件锁、再退出临界区：不能让线程在仍持锁时先获得被调度走的机会。

## 3. 与单核版本的差异

```text
UP（cpu_up.c）
  没有其他 CPU 并行执行
  -> spinlock 用禁止调度实现
  -> 不会发生忙等

SMP（cpu_mp.c）
  多 CPU 可同时修改共享数据
  -> 必须使用硬件原子锁
  -> 获取失败时会忙等
```

保留相同 API 的好处是：IPC、对象容器、调度器上层代码可以少写大量 `#ifdef RT_USING_SMP`。

## 4. `irqsave`：保护本核中断与其他 CPU

```c
rt_base_t rt_spin_lock_irqsave(struct rt_spinlock *lock)
{
    rt_base_t level = rt_hw_local_irq_disable();
    rt_enter_critical();
    rt_hw_spin_lock(&lock->lock);
    return level;
}
```

```c
void rt_spin_unlock_irqrestore(struct rt_spinlock *lock, rt_base_t level)
{
    rt_hw_spin_unlock(&lock->lock);
    rt_exit_critical_safe(critical_level);
    rt_hw_local_irq_enable(level);
}
```

`local_irq` 仅作用于本 CPU：

```text
CPU0 关本地中断
  -> CPU0 的 ISR 不会进入
  -> CPU1 的 ISR、CPU1 的线程仍可运行
```

因此仍需要硬件 spinlock 来排除 CPU1。`level` 是调用前的中断状态；恢复它而非简单开中断，才能正确支持嵌套临界区。

## 5. `_cpus_lock`：全 CPU 调度临界锁

```c
rt_hw_spinlock_t _cpus_lock;
```

它是保护多 CPU 调度相关状态的特殊全局硬件锁。`rt_cpus_lock()` 执行：

```text
关闭本 CPU 本地中断
  -> 进入调度临界区
  -> 获取 _cpus_lock
```

在 SMP 配置的 `rthw.h` 中，传统接口映射为：

```c
#define rt_hw_interrupt_disable rt_cpus_lock
#define rt_hw_interrupt_enable  rt_cpus_unlock
```

所以 SMP 中某些传统“关中断”调用不仅影响本核中断，也会通过 `_cpus_lock` 串行化 CPU 间的相关临界区。

## 6. `cpus_lock_nest`：避免同线程自死锁

同一线程可能嵌套调用 `rt_cpus_lock()`。硬件自旋锁通常不可重入，重复获取同一把锁会让线程等待自己，造成死锁。

TCB 中的 `cpus_lock_nest` 规则：

```text
0 -> 1：真正进入临界区并获得 _cpus_lock
1 -> 2：只增加计数，不重复取锁
2 -> 1：只减少计数，不释放锁
1 -> 0：真正释放 _cpus_lock 并退出临界区
```

计数为原子类型，因为它涉及多核执行环境。

## 7. `rt_cpu_get_id()` 的安全条件

```c
RT_ASSERT(rt_sched_thread_is_binding(RT_NULL) ||
          rt_hw_interrupt_is_disabled() ||
          !rt_scheduler_is_available());
```

未绑定 CPU 的线程可能在两条语句之间迁移：先读取为 CPU0，随后被调度到 CPU1。因此只有以下场景安全读取 CPU ID：

- 线程已绑定 CPU；
- 本地中断关闭；
- 调度器还未启动。

## 8. 文件定位

```text
cpu_up.c：单核 CPU 抽象与兼容接口
cpu_mp.c：多核 CPU 本地状态、原子自旋锁与跨核同步
scheduler_up.c / scheduler_mp.c：建立在对应 CPU 抽象之上的调度实现
```

一句话：**`cpu_mp.c` 让多个 CPU 同时访问 RT-Thread 内核对象、链表和调度状态时，仍能保持数据一致性。**

## 9. `rt_cpus_lock_status_restore()`：切换后的 CPU 状态衔接

```c
void rt_cpus_lock_status_restore(struct rt_thread *thread)
{
#if defined(ARCH_MM_MMU) && defined(RT_USING_SMART)
    lwp_aspace_switch(thread);
#endif
    rt_sched_post_ctx_switch(thread);
}
```

它由架构相关的上下文切换路径在恢复目标线程后调用。两件事分别是：

- Smart + MMU 时，切换到目标线程所属的用户地址空间；
- 调用 `rt_sched_post_ctx_switch()`，提交新的 `current_thread`，并完成跨上下文切换的调度器锁收尾。

这说明“保存/恢复寄存器与 SP”由 BSP/CPU 架构代码负责；而 CPU 地址空间、`current_thread`、调度器锁的一致性收尾由 RT-Thread 内核的 MP 协作代码负责。

## 10. 两类锁不可混为一谈

| 锁/接口 | 保护范围 | 典型用途 |
|---|---|---|
| `struct rt_spinlock` + `rt_spin_lock()` | 某个对象或容器的共享字段 | IPC 对象、对象容器、驱动共享状态 |
| `_cpus_lock` + `rt_cpus_lock()` | 多 CPU 的传统全局临界区 | 兼容旧 BSP 的全 CPU 调度相关临界区 |
| `_mp_scheduler_lock` | 调度器 READY 队列、位图、`oncpu` | `scheduler_mp.c` 内部 |

它们都可能最终调用 `rt_hw_spin_lock()`，但保护对象和持锁规则不同，不能互相替代。

`rt_spin_lock_init(lock)` 在 MP 下不是空操作：它明确调用 `rt_hw_spin_lock_init(&lock->lock)` 初始化该对象内部的硬件锁状态。与 UP 版本的空实现形成对照；因此一个 `struct rt_spinlock` 必须在第一次使用前初始化。

## 11. `rt_cpus_lock()` / `rt_cpus_unlock()` 的逐步行为

```text
rt_cpus_lock()
  -> 关闭本 CPU 本地中断，保存 level
  -> 读取本 CPU current_thread
  -> current_thread->cpus_lock_nest 加一
  -> 只有从 0 变为 1 时，enter_critical 并取得 _cpus_lock
  -> 返回 level

rt_cpus_unlock(level)
  -> cpus_lock_nest 减一
  -> 只有从 1 变为 0 时，释放 _cpus_lock 并 exit_critical_safe
  -> 恢复调用前的本地中断状态 level
```

启动初期 `current_thread == RT_NULL` 时，函数只保存/恢复本地中断状态；此时尚无线程上下文可记录嵌套计数。

## 12. 持锁规则与 IPI 的区别

硬件 spinlock 只能防止多个 CPU 同时进入同一临界区；它不会自动让另一个 CPU 重新选择线程。若一个 READY 线程应抢占其他 CPU 上的低优先级线程，调度器还必须发送：

```c
rt_hw_ipi_send(RT_SCHEDULE_IPI, mask);
```

因此：

```text
自旋锁：保证共享数据一致
IPI：通知目标 CPU 尽快执行调度决定
```

持锁区域必须短小，且不能等待信号量、互斥锁、消息或延时；否则一个 CPU 持锁睡眠时，其他 CPU 只能忙等，问题比单核更严重。

## 源码锚点：MP 的 spinlock 同时禁止本核调度并取得硬件锁

来源：`src/cpu_mp.c`：

```c
void rt_spin_lock(struct rt_spinlock *lock)
{
    rt_enter_critical();
    rt_hw_spin_lock(&lock->lock);
    RT_SPIN_LOCK_DEBUG(lock);
}

void rt_spin_unlock(struct rt_spinlock *lock)
{
    RT_SPIN_UNLOCK_DEBUG(lock, critical_level);
    rt_hw_spin_unlock(&lock->lock);
    rt_exit_critical_safe(critical_level);
}
```

与 UP 比较，多出的 `rt_hw_spin_lock/unlock` 才能阻止其他 CPU 并发进入；`rt_enter_critical()` 仍只处理本核调度边界，不能单独替代硬件自旋锁。
