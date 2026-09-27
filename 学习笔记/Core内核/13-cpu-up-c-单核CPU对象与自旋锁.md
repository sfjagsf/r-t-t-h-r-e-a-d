# `cpu_up.c`：单核 CPU 对象与自旋锁学习笔记

> 本笔记对应 `src/cpu_up.c`。`UP` 是 Uniprocessor，表示单核版本；它为上层内核提供与 SMP 兼容的 CPU 对象和 spinlock 接口。

## 1. 单核 CPU 对象

```c
static struct rt_cpu _cpu;
```

单核系统只有一个 CPU 对象：

```text
_cpu
 ├─ current_thread -> 当前运行线程的 TCB
 └─ idle_thread    -> 空闲线程的 TCB
```

`rt_cpu_self()` 永远返回 `&_cpu`；`rt_cpu_index(0)` 也返回它，其他编号返回 `RT_NULL`。

这样，上层可以统一写：

```c
rt_cpu_self()->current_thread
```

在单核下取得唯一 CPU 的当前线程；在 SMP 下则取得“本核”的当前线程，无须让调度器为两种系统分别编写接口。

## 2. 为什么单核仍提供 spinlock API

自旋锁原本处理的是多核并发：

```text
CPU0 修改共享数据 <-> CPU1 同时修改同一数据
```

单核没有两个 CPU 真正并行执行，因此：

```c
void rt_spin_lock_init(struct rt_spinlock *lock)
{
    RT_UNUSED(lock);
}
```

初始化不需要建立真实的原子锁状态。

但单核仍可能发生两类打断：

```text
当前线程 -> 被调度器切走 -> 另一线程运行
当前线程 -> 被硬件中断打断 -> ISR 运行
```

所以单核的 spinlock API 仍有意义，只是实现方式从“CPU 间原子互斥”转为“禁止调度，以及必要时关闭中断”。

## 3. `rt_spin_lock()` / `rt_spin_unlock()`

```c
void rt_spin_lock(struct rt_spinlock *lock)
{
    rt_enter_critical();
    RT_SPIN_LOCK_DEBUG(lock);
}

void rt_spin_unlock(struct rt_spinlock *lock)
{
    rt_base_t critical_level;
    RT_SPIN_UNLOCK_DEBUG(lock, critical_level);
    rt_exit_critical_safe(critical_level);
}
```

单核下这对接口的实际效果：

```text
rt_spin_lock()
  -> 进入调度临界区
  -> 当前线程不会被正常调度切换

rt_spin_unlock()
  -> 退出调度临界区
  -> 若已有调度请求，此后可发生切换
```

它不关闭中断。因此它适合只会被普通线程访问、不会被 ISR 同时访问的数据。

## 4. `irqsave` 版本：同时防线程和中断

```c
rt_base_t rt_spin_lock_irqsave(struct rt_spinlock *lock)
{
    rt_base_t level;

    level = rt_hw_interrupt_disable();
    rt_enter_critical();
    RT_SPIN_LOCK_DEBUG(lock);
    return level;
}
```

```c
void rt_spin_unlock_irqrestore(struct rt_spinlock *lock,
                               rt_base_t level)
{
    rt_base_t critical_level;

    RT_SPIN_UNLOCK_DEBUG(lock, critical_level);
    rt_exit_critical_safe(critical_level);
    rt_hw_interrupt_enable(level);
}
```

配对写法：

```c
rt_base_t level = rt_spin_lock_irqsave(&lock);

/* 修改也可能被 ISR 访问的共享数据 */

rt_spin_unlock_irqrestore(&lock, level);
```

`level` 保存的是调用前的中断状态。恢复时不能简单地“开中断”：若调用者原先已经关中断，恢复后必须仍然关闭。

## 5. 四个接口的对比

| 接口 | 禁止普通线程切换 | 禁止本地中断 | 单核下是否忙等 |
| --- | ---: | ---: | ---: |
| `rt_spin_lock_init()` | 否 | 否 | 否 |
| `rt_spin_lock()` | 是 | 否 | 否 |
| `rt_spin_unlock()` | 解除 | 否 | 否 |
| `rt_spin_lock_irqsave()` | 是 | 是 | 否 |
| `rt_spin_unlock_irqrestore()` | 解除 | 恢复原状态 | 否 |

## 6. 调试宏不是锁本体

`RT_SPIN_LOCK_DEBUG(lock)` 与 `RT_SPIN_UNLOCK_DEBUG(lock, critical_level)` 只在相关调试配置开启时记录：

- 锁的持有线程；
- 获取锁的调用位置；
- 获取锁时的临界区层数。

它们帮助发现漏解锁、错误解锁等问题；单核互斥的核心仍是临界区和中断状态控制。

## 7. 与 SMP 的核心区别

```text
UP（cpu_up.c）
  无其他 CPU 并行执行
  -> spinlock 不做原子忙等
  -> 用禁止调度 / 关中断保证互斥

SMP（cpu_mp.c）
  多个 CPU 可同时访问共享数据
  -> 必须使用原子指令获取真实锁
  -> 获取失败时可能忙等（spin）
```

一句话：**UP 版本保留统一 API；SMP 版本才需要真正的自旋锁算法。**

## 8. 逐函数执行顺序

### `rt_spin_lock_init(lock)`

UP 下只执行 `RT_UNUSED(lock)`。这不代表调用者可以传空指针或省略初始化；调用方仍应遵守统一 API 生命周期，便于同一份上层代码切换到 SMP 后继续正确工作。

### `rt_spin_lock(lock)` / `rt_spin_unlock(lock)`

```text
lock：rt_enter_critical() -> 调试记录 owner/临界层数
unlock：清除调试记录 -> rt_exit_critical_safe()
```

它们保护普通线程之间共享的数据。UP 下进入调度临界区后，当前线程不会被调度器换成另一个线程；但硬件中断仍可能发生。

### `rt_spin_lock_irqsave(lock)` / `rt_spin_unlock_irqrestore(lock, level)`

```text
lock_irqsave：保存并关闭本地中断 -> enter_critical -> 调试记录 -> 返回旧中断状态
unlock_irqrestore：清除调试记录 -> exit_critical_safe -> 恢复旧中断状态
```

它用于“普通线程与 ISR 都会访问”的共享数据。`level` 必须原样传回，不能用 `RT_TRUE` 或固定值替代；调用前本就关闭中断时，恢复后仍必须保持关闭。

## 9. 使用边界

持有上述锁或调度临界区时，不应调用可能阻塞的 API，例如带等待时间的：

```text
rt_sem_take()
rt_mutex_take()
rt_mq_recv()
rt_thread_delay()
```

原因不是“锁一定立刻死锁”，而是线程一旦等待，就无法以正常方式离开其持有的临界区，其他需要同一保护的数据路径会被卡住。临界区应只包含短小的链表、计数器或状态更新。

## 10. CPU 对象接口的边界

```c
rt_cpu_self()    /* 永远返回 &_cpu */
rt_cpu_index(0)  /* 返回 &_cpu */
rt_cpu_index(n)  /* n != 0 时返回 RT_NULL */
```

`rt_cpu_index()` 的 UP 实现显式检查编号，避免把“CPU1”的概念误当成真实对象。上层代码处理 `RT_NULL`，才能在单核配置下安全退化。

## 源码锚点：UP 的 spinlock 实际退化为调度临界区

来源：`src/cpu_up.c`：

```c
void rt_spin_lock(struct rt_spinlock *lock)
{
    rt_enter_critical();
    RT_SPIN_LOCK_DEBUG(lock);
}

void rt_spin_unlock(struct rt_spinlock *lock)
{
    RT_SPIN_UNLOCK_DEBUG(lock, critical_level);
    rt_exit_critical_safe(critical_level);
}
```

单核没有其他 CPU 会同时修改数据，因此没有硬件原子自旋；锁的实际作用是进入/退出本核调度临界区。
