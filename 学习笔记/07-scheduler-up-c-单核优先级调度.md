# `scheduler_up.c`：单核优先级调度学习笔记

> 本笔记对应 `src/scheduler_up.c`，即单核（UP，Uniprocessor）调度器的具体实现。`scheduler_comm.c` 放通用状态转换；本文件负责 READY 队列、位图选线程、调度决策与单核上下文切换入口。

## 1. 线程基础状态（以当前源码为准）

```text
RT_THREAD_INIT      已初始化，尚未 startup
RT_THREAD_READY     已进入就绪队列，等待 CPU
RT_THREAD_RUNNING   当前正在某个 CPU 上执行
RT_THREAD_SUSPEND   暂不能运行：等 IPC、延时或被显式挂起
RT_THREAD_CLOSE     已关闭/结束，等待资源回收
```

典型生命周期：

```text
INIT → READY → RUNNING
                 ├─ 时间片耗尽 / yield → READY
                 ├─ 等待 IPC / delay    → SUSPEND → READY
                 └─ 线程结束            → CLOSE
```

`rt_cpu_self()->current_thread` 表示“当前 CPU 正在运行谁”；它补充说明具体运行核。当前源码在调度器选择线程后，也明确设置 `RT_THREAD_RUNNING`。

## 2. 每优先级一个 READY 队列

```c
rt_list_t rt_thread_priority_table[RT_THREAD_PRIORITY_MAX];
```

```text
priority_table[0]：优先级 0 的 READY 线程，最高
priority_table[1]
...
priority_table[N]：优先级 N 的 READY 线程
```

RT-Thread 规则：**优先级数值越小，优先级越高**。

同优先级线程在对应的双向循环链表中排队：

```text
priority_table[5]：head ↔ A ↔ B ↔ C ↔ head
```

队首是该优先级下下一个候选线程。

## 3. READY 优先级位图

```c
rt_uint32_t rt_thread_ready_priority_group;
```

规则：

```text
bit N = 1：priority_table[N] 至少有一个 READY 线程
bit N = 0：priority_table[N] 为空
```

线程进入 READY 队列时：

```c
ready_group |= (1U << priority);      // OR：置位
```

线程离开 READY 队列，且该优先级队列已空时：

```c
ready_group &= ~(1U << priority);     // AND + NOT：清位
```

若 `RT_THREAD_PRIORITY_MAX > 32`，使用两级位图：

```text
rt_thread_ready_priority_group：哪个“8 优先级分组”非空
rt_thread_ready_table[]         ：该分组中哪个具体优先级非空
```

## 4. `__rt_ffs()`：快速找第一个置位 bit

```c
int __rt_ffs(int value);
```

从最低有效位（右侧）开始查找第一个 `1`，返回值从 **1** 开始计数：

```text
__rt_ffs(0b00000000) = 0
__rt_ffs(0b00000001) = 1
__rt_ffs(0b00001000) = 4
```

调度器使用：

```c
highest_prio = __rt_ffs(ready_group) - 1;
```

因此若 bit3 是最小编号的置位 bit：

```text
__rt_ffs() 返回 4
减 1 得 priority = 3
```

优先级数值越小越高，所以最低编号的 `1` 就是最高优先级的非空 READY 队列。

实现会因配置不同而变化：

```text
RT_USING_CPU_FFS：使用 CPU/架构优化指令
RT_USING_TINY_FFS：最低位隔离 + 小表映射
默认实现：按 8 bit 分段，再查 256 项字节表
```

`_scheduler_get_highest_priority_thread()` 的工作：

```text
位图 → 最高 READY 优先级 → 对应 priority_table 的队首线程
```

正常系统中 idle 线程始终 READY，因此调度器不会在空位图上调用该函数。

## 5. 单核调度锁

```c
rt_sched_lock(&level);
rt_sched_unlock(level);
```

单核下通过 `rt_hw_interrupt_disable()` 临时关闭中断保护就绪链表、位图、状态和 `current_thread` 等调度数据，完成后用保存的 `level` 恢复原中断状态。

```c
rt_sched_unlock_n_resched(level);
```

表示“解锁，并因线程状态可能改变而请求一次调度”。

## 6. 调度器初始化与首次启动

`rt_system_scheduler_init()`：

```text
初始化每条 priority_table[] 空链表
ready_priority_group 清零
初始化两级位图（若需要）
调度器临界嵌套计数清零
```

`rt_system_scheduler_start()` 只在系统从初始化阶段进入多线程运行时调用一次：

```text
找最高优先级 READY 线程
→ current_thread = to_thread
→ 从 READY 队列移除 to_thread
→ to_thread.stat = RUNNING
→ rt_hw_context_switch_to(&to_thread->sp)
```

它第一次恢复线程栈上下文，之后不返回初始化代码。

## 7. 核心调度函数 `rt_schedule()`

职责：比较当前 `RUNNING` 线程与最高优先级 `READY` 线程，决定继续运行还是切换。

```text
关中断
→ 若调度器未锁：从位图取得最高 READY 线程 to_thread
→ 比较当前线程 curr_thread
→ 必要时更新 READY/RUNNING 状态并上下文切换
→ 恢复中断
```

### 选择规则

```text
当前 RUNNING 优先级 < 最高 READY 优先级
    当前线程更高，继续运行

当前 RUNNING 优先级 == 最高 READY 优先级，且没有 YIELD
    当前线程继续运行

READY 线程优先级更高，或当前线程带 YIELD
    当前线程让出 CPU，切换到 READY 队首线程
```

### 发生切换时

```text
from_thread = 当前线程
to_thread   = 新选 READY 线程

若 from_thread 是被抢占/时间片轮转：插回 READY 队列
若 from_thread 带 YIELD：先按 YIELD 规则入队，再清掉 YIELD 位
to_thread 从 READY 队列移除，基础状态改 RUNNING
current_thread 改为 to_thread
调用硬件相关上下文切换函数
```

线程上下文：

```text
普通线程上下文：rt_hw_context_switch(&from->sp, &to->sp)
中断上下文：rt_hw_context_switch_interrupt(...)
```

后者通常记录“中断退出后恢复新线程”，避免在 ISR 中间不安全地直接切换栈。

## 8. 时间片与同优先级轮转

时间片在 `scheduler_comm.c` 的 `rt_sched_tick_increase()` 中递减。时间片耗尽后：

```text
remaining_tick = init_tick
stat |= YIELD
请求 rt_schedule()
```

`rt_sched_insert_thread()` 根据 YIELD 决定同优先级位置：

```text
带 YIELD：插入循环链表头之前，即队尾
不带 YIELD：插入循环链表头之后，即队首
```

例：

```text
当前 A RUNNING，priority 5，时间片耗尽
READY 队列：B → C

A 带 YIELD 回队：B → C → A
调度器选择 B 运行
```

高优先级线程变 READY 时可以抢占当前线程，不需要等待当前线程时间片耗尽。

## 9. 线程 startup、入队与出队

`rt_sched_thread_init_priv()`：在线程创建阶段初始化链表节点、优先级、初始/剩余时间片；线程尚未进入 READY 队列。

`rt_sched_thread_startup()`：计算线程优先级的位图掩码，再先设为 `SUSPEND`，使后续可复用“恢复为 READY”的统一路径。

`rt_sched_insert_thread(thread)`：

```text
当前线程自身：保持 RUNNING，不插入 READY 队列
其他线程：设为 READY，按优先级插入 priority_table[]，置对应位图 bit
```

`rt_sched_remove_thread(thread)`：从优先级 READY 链表移除；若该优先级队列变空，清除对应位图 bit。

## 10. 调度临界区

```c
rt_enter_critical();
rt_exit_critical();
```

它们采用 `rt_scheduler_lock_nest` 支持嵌套，语义是“此段代码不允许发生线程调度”。

```text
enter：nest + 1
exit ：nest - 1
nest 仍大于 0：继续禁止调度
nest 回到 0：若 critical_switch_flag 已记录延后调度请求，则调用 rt_schedule()
```

临界区中有更高优先级线程 READY 时，不立即切走当前线程，而是记录延后调度请求，最外层退出临界区后补做调度。

`rt_exit_critical_safe()` 在调试配置下会核对临界层级是否匹配，帮助发现 enter/exit 不成对问题。

## 11. 单核 CPU 绑定

```c
rt_sched_thread_bind_cpu(thread, cpu)
```

在 UP 版本中直接返回 `-RT_EINVAL`，因为只有一个 CPU，没有实际的 CPU 选择或绑定需求。多核绑定逻辑在 `scheduler_mp.c`。
