# RTOS 核心运行模型：从 TCB、状态到上下文切换

> 本文是 RT-Thread 源码学习的总览。它不对应单一 `.c` 文件，而是将 `thread.c`、`ipc.c`、`timer.c`、`scheduler_*.c`、`clock.c`、`defunct.c` 和 `idle.c` 串成一个运行模型。

## 1. 一句话本质

> RTOS 在有限 CPU 上维护“谁能运行、谁在等待什么、谁该先运行、何时必须醒来”，并通过上下文切换让 CPU 恢复执行被选中的线程。

单核系统任意时刻只有一个线程真的占用 CPU；多线程“同时运行”的感觉来自快速切换与等待状态管理。

## 2. 线程不是反复调用的回调

线程入口函数只在第一次运行时作为起点：

```c
void task_entry(void *parameter)
{
    while (1)
    {
        /* 线程业务 */
    }
}
```

线程被切走时，内核保存其 CPU 寄存器、PC、SP 等现场；再次被选中时，从保存的位置继续，而不是再次调用 `task_entry()`。

```text
线程 A 执行到中间
→ 保存 A 上下文到 A 的栈/TCB
→ 恢复 B 上下文
→ B 从上次停止位置继续
→ 日后恢复 A，A 接着原位置执行
```

这与软件定时器不同：软件定时器到期时会调用 `timeout_func(parameter)` 回调。

## 3. TCB：线程控制块

每条线程都有 `struct rt_thread`（TCB），保存内核管理该执行流的档案：

```text
rt_object parent：名称、对象类型、静态/动态属性
sp / stack_addr / stack_size：保存上下文与独立栈
entry / parameter：首次运行入口和参数
sched_thread_ctx：状态、优先级、时间片、链表节点
thread_timer：延时和 IPC 超时
error：等待操作最终结果
mutex / event 等 IPC 专有字段
信号、Smart、CPU 使用率等可选扩展
```

详细字段见：[03-thread-c-线程生命周期与延时挂起.md](03-thread-c-线程生命周期与延时挂起.md)。

## 4. 线程状态机

当前源码的基础状态：

```text
INIT      已初始化，尚未 startup
READY     在就绪队列中，等待 CPU
RUNNING   当前正在某 CPU 执行
SUSPEND   暂不能运行：等待资源、事件、时间或被显式挂起
CLOSE     已结束，等待资源回收
```

典型流转：

```text
INIT → READY → RUNNING
                 ├─ 高优先级线程抢占 / 时间片轮转 → READY
                 ├─ 等 IPC / delay                → SUSPEND → READY
                 └─ entry return / close           → CLOSE → defunct 回收
```

`SUSPEND` 只表示“不能运行”。更具体的等待原因由所在链表、关联对象和 `thread_timer` 共同表达。

## 5. 等待关系：链表的重要作用

链表是 RTOS 维护对象关系的重要工具，但不是全部。

```text
READY：挂在 priority_table[priority]
等待信号量：挂在 sem->parent.suspend_thread
等待事件：挂在 event->parent.suspend_thread
等待邮箱消息：挂在 mb->parent.suspend_thread
等待邮箱空间：挂在 mb->suspend_sender_thread
等待 mutex：挂在 mutex->parent.suspend_thread
CLOSE：挂在 _rt_thread_defunct
```

同一个 `thread_list_node` 同一时刻只能属于一个此类链表。

IPC 对象与等待机制详见：[04-ipc-c-进程间通信组件.md](04-ipc-c-进程间通信组件.md)。

## 6. READY 队列、位图与调度决策

单核 RT-Thread 为每个优先级维护一个 READY 链表：

```text
priority_table[0]：最高优先级 READY 线程
priority_table[1]
...
priority_table[N]：最低优先级 READY 线程
```

优先级数值越小越高。

位图记录哪些优先级队列非空：

```text
线程入队：OR 置对应 bit
队列变空：AND + NOT 清对应 bit
调度时：__rt_ffs() 找最低编号的 1 bit
```

最低编号的置位 bit 对应当前最高优先级的 READY 队列。

调度器比较：

```text
当前 RUNNING 优先级更高：继续运行
同优先级且未 YIELD：继续运行
有更高优先级 READY 线程，或当前线程 YIELD：发生切换
```

详细见：[07-scheduler-up-c-单核优先级调度.md](07-scheduler-up-c-单核优先级调度.md)。

## 7. 上下文切换

调度器选出 `to_thread` 后，硬件相关代码完成：

```text
保存 from_thread 的寄存器现场、SP
→ 恢复 to_thread 的寄存器现场、SP
→ CPU 从 to_thread 上次暂停处继续
```

抽象接口：

```c
rt_hw_context_switch(&from_thread->sp, &to_thread->sp);
```

中断上下文请求切换时，通常由架构相关代码在安全的中断退出路径恢复新线程。

## 8. 时间管理与超时竞争

```text
系统 Tick：统一时基
软件定时器：按绝对 timeout_tick 到期
thread_timer：每线程内嵌，用于 delay 与 IPC 超时
```

例如：

```c
rt_sem_take(sem, 100);
```

```text
资源不可用
→ 线程 SUSPEND，挂入 sem 等待链表
→ thread_timer 启动，截止时间 = 当前 Tick + 100

信号量先释放：停止 timer，线程 READY，返回 RT_EOK
超时先到：timer 回调移出等待链表，线程 READY，返回 -RT_ETIMEOUT
```

停止 timer、移除等待节点、插入 READY 队列必须协调完成，避免正常唤醒与超时回调重复唤醒同一线程。

详见：[05-timer-c-内核定时器与超时.md](05-timer-c-内核定时器与超时.md) 与 [08-clock-c-系统Tick与时间基准.md](08-clock-c-系统Tick与时间基准.md)。

## 9. 并发一致性

内核状态可被线程、中断或多个 CPU 修改，因此要保护：

```text
线程状态
等待链表
READY 队列与优先级位图
定时器链表
当前 CPU 的 current_thread
```

常见手段：

```text
关闭/恢复中断：单核短临界区
自旋锁：多 CPU 或共享对象保护
调度临界区：延后线程切换至安全时机
原子变量：Tick、嵌套计数等
```

目标是让状态转换完整发生：

```text
停止超时 timer
→ 从等待链表移除
→ 修改状态
→ 插入 READY 队列
```

不能被并发路径打断在中间。

## 10. CLOSE、defunct 与 idle

线程结束后不能在自身栈上立刻释放自身：

```text
RUNNING → CLOSE
→ 加入 defunct 链表
→ idle 或 tsystem 在其他安全上下文执行资源回收
```

静态线程 detach；动态线程释放 stack 和 TCB。

idle 线程是每 CPU 的最低优先级兜底：普通线程全阻塞时运行 hook、后台回收或低功耗逻辑。

详见：[09-defunct-c-线程延后回收.md](09-defunct-c-线程延后回收.md) 与 [10-idle-c-空闲线程与后台工作.md](10-idle-c-空闲线程与后台工作.md)。

## 11. 为什么是“实时”

实时不等于单纯更快，而是关键事件的响应延迟可预期、可分析。

```text
中断/IPC 事件
→ 高优先级线程 READY
→ 调度器选择最高优先级 READY 线程
→ 上下文切换
→ 关键处理尽快且可分析地执行
```

RTOS 通过固定优先级抢占、确定的 IPC 唤醒路径、定时器和受控临界区，帮助分析任务响应和截止时间。

## 12. 对象 + 接口 + 组件

```text
对象：thread、timer、sem、mutex、event、mailbox、messagequeue
接口：rt_thread_*、rt_timer_*、rt_sem_*、rt_event_* 等
组件：线程管理、调度器、IPC、定时器、内存、设备框架
```

例如一次 `rt_sem_take()` 关联多个组件：

```text
Semaphore 对象：资源计数与等待链表
Thread TCB：状态、error、thread_timer、链表节点
Timer：超时管理
Scheduler：线程 READY 后决定是否切换
硬件切换：真正恢复目标线程继续执行
```

## 13. 核心源码导航：从概念回到代码

下面列出本模型的关键源码入口。阅读时建议先看“输入状态”，再看函数修改了哪个 TCB 字段、从哪条链表移除/插入、最后是否触发调度。

```text
TCB 定义：              include/rtdef.h，struct rt_thread（约 851 行）
调度字段定义：          include/rtsched.h，struct rt_sched_thread_ctx / priv
线程 TCB 初始化：       src/thread.c，_thread_init()（约 179 行）
线程入口 return：       src/thread.c，_thread_exit()（约 117 行）
线程等待超时：          src/thread.c，_thread_timeout()（约 146 行）
挂起并加入等待链表：    src/thread.c，rt_thread_suspend_to_list()（约 948 行）
IPC 等待/唤醒：         src/ipc.c，_rt_sem_take()、rt_susp_list_dequeue()
SUSPEND → READY：       src/scheduler_comm.c，rt_sched_thread_ready()（约 232 行）
READY 入/出队：         src/scheduler_up.c，rt_sched_insert_thread()/remove_thread()
调度决策与切换：        src/scheduler_up.c，rt_schedule()（约 281 行）
线程时间片/Tick：       src/clock.c，rt_tick_increase()（约 136 行）
软件定时器排序/到期：   src/timer.c，_timer_start() / _timer_check()
线程最终资源回收：      src/defunct.c，rt_defunct_execute()
```

### 13.1 TCB：内核到底保存了什么

源码入口：`include/rtdef.h` 中的 `struct rt_thread`。

```c
struct rt_thread
{
    struct rt_object parent;

    void        *sp;
    void        *entry;
    void        *parameter;
    void        *stack_addr;
    rt_uint32_t stack_size;

    rt_err_t    error;
    RT_SCHED_THREAD_CTX
    struct rt_timer thread_timer;
    rt_thread_cleanup_t cleanup;
    ...
};
```

逐项对应：

```text
parent：线程也是统一对象，可按 name/type 注册、查找、detach/delete
sp：上下文切换保存和恢复的位置；决定“下次从哪里继续执行”
entry / parameter：仅用于第一次启动线程
stack_addr / stack_size：线程独立栈的范围
error：一次等待操作结束后的结果
sched_thread_ctx：状态、链表节点、优先级、时间片、调度标志
thread_timer：该线程自己的延时/超时闹钟
cleanup：线程关闭后、内存释放前的清理回调
```

其中 `RT_SCHED_THREAD_CTX` 在 `include/rtsched.h` 展开为 `sched_thread_ctx`，其核心字段可抽象为：

```c
struct rt_sched_thread_ctx
{
    rt_list_t thread_list_node;
    rt_uint8_t stat;
    rt_uint8_t sched_flag_ttmr_set:1;
    struct rt_sched_thread_priv sched_thread_priv;
};
```

`thread_list_node` 是线程在内核链表间移动的“车票”；`stat` 是基础状态；`sched_thread_priv` 保存优先级和时间片。

### 13.2 第一次运行：不是直接调用 entry，而是先造栈帧

源码：`src/thread.c` 的 `_thread_init()`。

```c
thread->entry      = (void *)entry;
thread->parameter  = parameter;
thread->stack_addr = stack_start;
thread->stack_size = stack_size;

rt_memset(thread->stack_addr, '#', thread->stack_size);

thread->sp = (void *)rt_hw_stack_init(thread->entry,
                                       thread->parameter,
                                       stack_top,
                                       (void *)_thread_exit);
```

含义：

```text
① 记录线程入口、参数和栈范围
② 栈填 '#': 用于栈高水位/溢出检查
③ rt_hw_stack_init(): 在新栈中伪造 CPU 首次恢复所需寄存器现场
④ 入口函数将来 return 时，返回地址是 _thread_exit()
```

所以调度器首次恢复 `thread->sp` 时，CPU 看起来像刚进入 `entry(parameter)`；以后恢复时，则回到线程上次暂停的任意指令位置。

### 13.3 进入等待：RUNNING 到 SUSPEND

以 IPC 等待为例，`rt_thread_suspend_to_list()` 的工作可概括为：

```text
RUNNING/READY 线程
→ 从 READY 队列移除（若在其中）
→ stat 写为 SUSPEND 及等待可中断属性
→ 按 FIFO/PRIO 插入对象的等待链表
→ 停止旧的 thread_timer（若有）
```

例如 `rt_sem_take()` 在信号量计数为 0 且允许等待时，最终会把当前线程挂入：

```text
sem->parent.suspend_thread
```

若带正数 timeout，还会设置并启动：

```text
thread->thread_timer
```

等待的完整状态不是单个 `SUSPEND`：

```text
stat = SUSPEND
+ thread_list_node 在 sem->suspend_thread
+ thread_timer 可能已启动
+ error 预设为等待中的中断/失败状态
```

### 13.4 正常唤醒：SUSPEND 到 READY

源码：`src/scheduler_comm.c` 的 `rt_sched_thread_ready()`，核心顺序：

```c
if (RT_SCHED_CTX(thread).sched_flag_ttmr_set)
    error = rt_sched_thread_timer_stop(thread);

rt_list_remove(&RT_THREAD_LIST_NODE(thread));
rt_sched_insert_thread(thread);
```

这段顺序不能调换：

```text
先停 thread_timer
→ 再从 IPC 等待链表移除
→ 再进入 READY 队列
```

原因是正常 IPC 唤醒与超时回调可能同时争夺同一线程。成功停止 timer 的一方才能继续改变该线程的状态，避免重复入队。

### 13.5 超时唤醒：thread_timer 回调如何接管线程

源码：`src/thread.c` 的 `_thread_timeout()`。

```c
RT_SCHED_CTX(thread).sched_flag_ttmr_set = 0;
thread->error = -RT_ETIMEOUT;

rt_list_remove(&RT_THREAD_LIST_NODE(thread));
rt_sched_insert_thread(thread);
```

它与正常唤醒的结果相似，区别是：

```text
正常 IPC 唤醒：error = RT_EOK
超时定时器唤醒：error = -RT_ETIMEOUT
```

线程从 `rt_sem_take()`、`rt_mb_recv()` 等 API 返回时，就是通过 `thread->error` 知道自己因成功还是超时而恢复。

### 13.6 READY 队列与位图：为什么调度器能快速选线程

源码：`src/scheduler_up.c`。

入队核心：

```c
RT_SCHED_CTX(thread).stat = RT_THREAD_READY | other_flags;
/* 根据 YIELD 标志插入 priority_table[priority] 的队首或队尾 */
rt_thread_ready_priority_group |= thread->number_mask;
```

出队核心：

```c
rt_list_remove(&RT_THREAD_LIST_NODE(thread));
if (rt_list_isempty(&priority_table[priority]))
    rt_thread_ready_priority_group &= ~thread->number_mask;
```

调度器不是扫描所有 TCB，而是：

```text
位图：哪些优先级 READY 队列非空？
__rt_ffs()：最低编号的 1 bit 是哪个？
priority_table[该优先级].next：取该队列队首 TCB
```

优先级数值越小越高，因此最低编号置位 bit 对应最高优先级 READY 线程。

### 13.7 `rt_schedule()`：决策与切换的最小模型

源码：`src/scheduler_up.c` 的 `rt_schedule()`。

核心决策可以抽象为：

```c
to_thread = highest_ready_thread();

if (curr_thread is RUNNING)
{
    if (curr_prio < ready_prio)
        to_thread = curr_thread;       /* 当前更高优先级 */
    else if (curr_prio == ready_prio && !YIELD)
        to_thread = curr_thread;       /* 同优先级且未轮转 */
}
```

若 `to_thread != curr_thread`：

```text
旧 RUNNING 线程：必要时插回 READY 队列
新 READY 线程：从 READY 队列移除，stat 改 RUNNING
current_thread：改为新线程
硬件相关函数：保存旧 sp，恢复新 sp
```

源码调用：

```c
rt_hw_context_switch((rt_uintptr_t)&from_thread->sp,
                     (rt_uintptr_t)&to_thread->sp);
```

这一步是“策略层”与“CPU 架构层”的分界：C 内核决定切谁；汇编/架构代码完成寄存器和栈切换。

### 13.8 Tick、时间片与定时器检查

源码：`src/clock.c` 的 `rt_tick_increase()`。

```c
rt_atomic_add(&(rt_tick), 1);
rt_sched_tick_increase(1);
rt_timer_check();
```

每个 Tick 中断依次：

```text
推进系统统一时间
→ 扣减当前线程 remaining_tick（耗尽会请求轮转）
→ 检查到期软件定时器（可能唤醒超时线程或执行定时器回调）
```

### 13.9 线程结束与延后回收

源码：`src/thread.c` 的 `_thread_exit()` 与 `src/defunct.c`。

```c
rt_thread_close(thread);
_thread_detach_from_mutex(thread);
rt_thread_defunct_enqueue(thread);
rt_schedule();
```

```text
entry return
→ RUNNING 变 CLOSE
→ 退出等待/持有的 mutex
→ 进入 defunct 链表
→ 切到其他线程
→ idle/tsystem 释放死亡线程的 stack 与动态 TCB
```

线程不能在自己的栈上直接释放自己；延后回收是安全性的必要条件。

## 14. 建议的源码跟读实验

选择一个最小场景，按字段和链表逐行跟踪：

```c
rt_sem_take(&sem, 10);
/* 另一个线程或 ISR 中 */
rt_sem_release(&sem);
```

建议在纸上记录以下状态变化：

```text
1. 当前线程 T 的 stat、error、thread_list_node 在哪
2. sem->value 与 sem->suspend_thread 有哪些线程
3. T 的 thread_timer 是否 ACTIVATED，timeout_tick 是多少
4. release 后 T 是否 READY、是否需要 rt_schedule()
5. 调度器最终是否从当前线程切到 T
```

再把 `release` 延后超过 10 Tick，重复同样记录，就能直观看到“正常唤醒”和“超时唤醒”两条路径的差异。
