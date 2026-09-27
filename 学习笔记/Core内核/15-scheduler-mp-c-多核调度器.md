# `scheduler_mp.c`：多核调度器学习笔记

> 本笔记对应 `src/scheduler_mp.c`，仅在 `RT_USING_SMP` 开启时参与编译。当前记录文件前半段：多核就绪队列、调度器锁、线程入队出队与跨核调度通知。

## 1. 多核调度器的难题

单核任意时刻只会有一个线程运行；多核中多个 CPU 可以同时选择线程。因此调度器必须保证：

- 同一线程不能同时被两个 CPU 选中；
- 多 CPU 对就绪链表、优先级位图的修改不冲突；
- 新出现的高优先级线程能及时通知其他 CPU；
- 绑定 CPU 的线程不会被错误迁移。

## 2. 全局与每 CPU 两类 READY 队列

```c
rt_list_t rt_thread_priority_table[RT_THREAD_PRIORITY_MAX];
```

这是**全局**优先级队列，存放未绑定 CPU 的 READY 线程。每个 `struct rt_cpu` 还具有自己的：

```text
priority_table[] / priority_group / ready_table[]
```

用于存放绑定在该 CPU 的 READY 线程：

```text
全局 READY 队列：未绑定线程，任意 CPU 可取走
CPU0 READY 队列：只允许 CPU0 运行的线程
CPU1 READY 队列：只允许 CPU1 运行的线程
```

`bind_cpu == RT_CPUS_NR` 是特殊值，表示“未绑定 CPU”，不是一个实际 CPU 编号。

## 3. `_mp_scheduler_lock`：全局调度器锁

```c
static struct rt_spinlock _mp_scheduler_lock;
```

它保护全局/本地 READY 链表、优先级位图，以及线程调度状态和 `oncpu` 字段。多 CPU 修改这些数据前必须持锁。

`SCHEDULER_LOCK(level)` 的关键动作：

```text
关闭本 CPU 本地中断
  -> 增加当前线程的调度临界层数
  -> 获取 _mp_scheduler_lock 硬件自旋锁
```

三层目的不同：

| 措施 | 防止的问题 |
| --- | --- |
| 关闭本地中断 | 本 CPU ISR 干扰调度状态 |
| 调度临界计数 | 当前线程在临界区中被切走 |
| `_mp_scheduler_lock` | 其他 CPU 并发改链表或位图 |

## 4. 最高优先级线程的选择

`_scheduler_get_highest_priority_thread()` 分别取得：

```text
global highest：全局未绑定 READY 线程的最高优先级
local highest ：本 CPU 绑定 READY 线程的最高优先级
```

RT-Thread 数值越小优先级越高，因此比较两者并选择数字更小者：

```text
全局优先级 3，CPU1 本地优先级 1
  -> CPU1 选择本地优先级 1 的线程
```

位图查找仍使用 `__rt_ffs()`，优先级超过 32 时采用两级位图。

## 5. `_sched_insert_thread_locked()`：使线程 READY

调用者必须已持有 `_mp_scheduler_lock`。函数依次：

```text
检查线程是否已经 READY，或仍被某 CPU 运行
  -> 将基础状态设为 READY
  -> 按 bind_cpu 选择全局或指定 CPU 的优先级链表
  -> 按 YIELD 标志插入链表前/后
  -> 设置对应优先级位图
  -> 必要时向其他 CPU 发送调度 IPI
```

正在运行的线程具有 `oncpu != RT_CPU_DETACHED`；该检查避免同一线程重复入队，或被另一 CPU 同时取走。

## 6. `_sched_remove_thread_locked()`：从 READY 队列取走

当线程即将变为 RUNNING 时：

```text
从它所在 READY 链表删除节点
  -> 若该优先级链表已空
  -> 清除全局或目标 CPU 对应的优先级位图
```

位图必须与链表保持一致；否则调度器会把空队列误判为可运行优先级。

## 7. 调度 IPI：通知其他 CPU 重新选择线程

未绑定线程入全局队列时：

```c
rt_hw_ipi_send(RT_SCHEDULE_IPI, RT_CPU_MASK ^ (1 << cpu_id));
```

通知除当前 CPU 以外的 CPU。绑定线程入某个其他 CPU 的本地队列时，只通知目标 CPU。

目标 CPU 的 BSP IPI ISR 调用：

```c
void rt_scheduler_ipi_handler(int vector, void *param)
{
    rt_schedule();
}
```

完整通路：

```text
CPU0 使高优先级线程 READY
  -> 插入全局或 CPU1 本地队列
  -> 向 CPU1 发送 RT_SCHEDULE_IPI
  -> CPU1 IPI ISR 调用 rt_schedule()
  -> CPU1 必要时抢占低优先级运行线程
```

## 8. 初始化与首次运行

`rt_system_scheduler_init()`：

- 初始化 `_mp_scheduler_lock`；
- 初始化全局每一优先级链表和位图；
- 初始化每个 CPU 的本地优先级链表、位图、当前线程状态。

`rt_system_scheduler_start()`：

```text
持有调度器锁
  -> 从全局/本 CPU READY 队列选最高优先级线程
  -> 将它移出 READY 队列
  -> thread->oncpu = 本 CPU 编号
  -> thread->stat = RUNNING
  -> rt_hw_context_switch_to()
```

`oncpu` 字段是防止一个线程被两个 CPU 同时调度的关键标记。

---

## 9. `rt_schedule()`：普通线程上下文的一次调度

运行期发生线程让出、时间片到期、IPC 唤醒或优先级变化时，最终可能调用 `rt_schedule()`：

```text
关闭本 CPU 中断
  -> 取得本 CPU 的 current_thread
  -> 若在 ISR：设置 irq_switch_flag，延后处理
  -> 若仍在嵌套临界区：设置 critical_switch_flag，延后处理
  -> 获取 _mp_scheduler_lock
  -> _prepare_context_switch_locked() 选择目标线程
  -> 必要时 rt_hw_context_switch()
```

关闭本地中断只防止本 CPU ISR 干扰；`_mp_scheduler_lock` 才排除其他 CPU 对全局调度器结构的并发访问。

## 10. `_prepare_context_switch_locked()`：是否真的切换

它在调度器锁保护下工作：

```text
暂时令当前线程 oncpu = RT_CPU_DETACHED
  -> 比较全局 READY 和本 CPU READY 的最高优先级
  -> 当前线程更高优先级，或同级且未 YIELD：继续占用本 CPU
  -> 否则：当前线程按 bind_cpu 放回正确 READY 队列
  -> 目标线程设为 oncpu = 当前 CPU
  -> 目标线程 READY -> RUNNING，并从 READY 链表移除
```

`YIELD` 表示当前线程主动交出同优先级的剩余时间片；调度完成后标志会清除。

## 11. 中断中调度：延后至中断退出

若 `rt_schedule()` 发现：

```c
rt_atomic_load(&pcpu->irq_nest) != 0
```

它不能立即切换普通线程，只设置：

```text
pcpu->irq_switch_flag = 1
```

中断退出路径调用：

```c
rt_scheduler_do_irq_switch(context);
```

当中断嵌套数已变为 0 时，函数在相同的调度器锁保护下选线程，并调用：

```c
rt_hw_context_switch_interrupt(context, ...);
```

它与普通 `rt_hw_context_switch()` 的差别是要处理 ISR 保存的中断现场 `context`。

## 12. 上下文切换后为何才释放调度器锁

`rt_schedule()` 发起切换时不会立即解锁 `_mp_scheduler_lock`；锁跨越 SP、寄存器和线程归属正在改变的中间阶段。目标线程恢复后执行：

```c
rt_sched_post_ctx_switch(thread);
```

关键步骤：

```text
确认本地中断关闭
  -> 清除旧线程最后一层 critical_lock_nest
  -> 释放 _mp_scheduler_lock
  -> pcpu->current_thread = 新线程
```

这使其他 CPU 不会观察到“栈已切换但 current_thread 仍是旧线程”等不一致状态。

## 13. 对外 READY 队列封装

```c
rt_sched_insert_thread(thread);
rt_sched_remove_thread(thread);
```

二者都要求调用者已持有调度器锁：

- `insert` 调用 `_sched_insert_thread_locked()`，使线程进入 READY；
- `remove` 调用 `_sched_remove_thread_locked()`，并把基础状态设为 `RT_THREAD_SUSPEND_UNINTERRUPTIBLE`。

它们主要供内核线程、IPC、定时器等内部路径调用，不应作为应用层常规 API。

## 14. 线程调度字段的两个初始化阶段

`rt_sched_thread_init_priv(thread, tick, priority)` 在创建线程时初始化：

```text
线程调度链表节点
init_priority / current_priority
init_tick / remaining_tick
critical_lock_nest = 0（SMP）
```

`current_priority` 可因优先级继承变化，`init_priority` 保留创建时基础优先级。

`rt_sched_thread_startup(thread)` 在启动线程前根据 `current_priority` 计算优先级位图掩码，并把线程设置为初始 `SUSPEND`，之后启动流程再将它转为 READY。

## 15. 临界区：计数与延后调度

`rt_enter_critical()`：短暂关闭本地中断，安全取得当前线程后增加：

```c
RT_SCHED_CTX(current_thread).critical_lock_nest++;
```

它不持续关闭中断，也不等于锁住全 CPU 调度器；它表示当前线程处于本地调度临界区。

`rt_exit_critical()` 减少计数：

```text
计数 > 0：仍在外层临界区，不调度
计数 = 0：检查 critical_switch_flag
           若此前有延后调度请求，调用 rt_schedule()
```

`rt_exit_critical_safe()` 在调试配置下还会验证调用者记录的预期层数，帮助发现临界区 / 自旋锁未正确配对的问题。

## 16. CPU 绑定与迁移

```c
rt_sched_thread_bind_cpu(thread, cpu);
```

| `cpu` 参数 | 含义 |
| --- | --- |
| `0 ... RT_CPUS_NR - 1` | 绑定到指定 CPU |
| `>= RT_CPUS_NR` | 转换成 `RT_CPUS_NR`，表示解除绑定 |

READY 线程修改绑定时：

```text
从旧 READY 队列删除
  -> 更新 bind_cpu
  -> 插入全局或目标 CPU READY 队列
  -> 必要时重新调度
```

RUNNING 线程不能直接“瞬移”到目标 CPU，而是更新 `bind_cpu` 后通过 IPI 通知相关 CPU 调度，使线程从原 CPU 让出、进入正确队列，再由目标 CPU 选中。

## 17. 总结

`scheduler_up.c` 主要回答“当前 CPU 运行谁”；`scheduler_mp.c` 还必须处理：

```text
哪个 CPU 运行谁
线程能否迁移或绑定
同一线程不能同时跑在两个 CPU
多 CPU 并发修改调度数据如何同步
IPI 如何触发跨核抢占
中断与临界区中的调度请求如何延后
```

## 18. 调度 Hook：观察而不参与决策

开启 `RT_USING_HOOK` 与 `RT_HOOK_USING_FUNC_PTR` 后，调度器提供：

```c
rt_scheduler_sethook(from_to_hook);
rt_scheduler_switch_sethook(switch_hook);
```

两者由 `rt_schedule()` / `_prepare_context_switch_locked()` 在决定切换时触发：

| Hook | 观察时机 | 参数 / 用途 |
| --- | --- | --- |
| scheduler hook | 已确定 `from -> to` | 观察完整的线程切换关系 |
| scheduler switch hook | 即将执行硬件上下文切换 | 观察即将离开的线程 |

Hook 适合跟踪、统计、调试；不应在其中阻塞、长时间运行或再次破坏调度器临界区。它不是调度策略的一部分，不能用来决定下一线程。

## 源码锚点：SMP 调度在 IRQ 中只登记延后切换

来源：`src/scheduler_mp.c` 的 `rt_schedule()` 开头：

```c
level = rt_hw_local_irq_disable();
cpu_id = rt_hw_cpu_id();
pcpu = rt_cpu_index(cpu_id);
current_thread = pcpu->current_thread;

if (rt_atomic_load(&(pcpu->irq_nest)))
{
    pcpu->irq_switch_flag = 1;
    rt_hw_local_irq_enable(level);
    return;
}
```

若当前仍在 ISR，上述代码不会立刻切换上下文，只给本 CPU 的 `irq_switch_flag` 置位；真正切换留到中断退出安全点。
