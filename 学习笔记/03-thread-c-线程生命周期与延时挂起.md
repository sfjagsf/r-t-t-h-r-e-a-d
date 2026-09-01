# RT-Thread `thread.c` 学习笔记：线程生命周期、延时、挂起与恢复

## 文件定位

`src/thread.c` 是线程组件的实现文件。它建立在 `rtdef.h` 定义的 `struct rt_thread` 和 `object.c` 的统一对象管理机制之上。

```text
object.c
  管理：这是一个 Thread 类对象、名称、对象链表、静态/动态生命周期

thread.c
  管理：线程入口、栈、优先级、时间片、延时超时、挂起、恢复、退出
```

## 一、线程 Hook

```c
static void (*rt_thread_suspend_hook)(rt_thread_t thread);
static void (*rt_thread_resume_hook)(rt_thread_t thread);
```

注册函数仅保存回调地址：

```c
rt_thread_suspend_sethook(my_hook);
rt_thread_resume_sethook(my_hook);
```

内核在预埋的 `RT_OBJECT_HOOK_CALL(...)` 位置检查指针并调用。Hook 必须短小，不能阻塞、挂起或进行复杂内核操作。

## 二、线程退出：`_thread_exit()`

线程入口函数返回后，初始栈现场会让 CPU 转入 `_thread_exit()`：

```text
entry(parameter)
    ↓ return
_thread_exit()
    ↓
关闭当前线程
→ 处理仍持有/等待的 mutex
→ 放入 defunct（待回收）线程队列
→ 调度下一条可运行线程
```

线程不能在仍使用自身栈时直接释放自己，所以先进入待回收队列，由后续机制回收。

## 三、线程等待超时：`_thread_timeout()`

每个 `struct rt_thread` 内嵌一个：

```c
struct rt_timer thread_timer;
```

当线程等待 IPC 资源或调用延时 API 时：

```text
线程挂起
├─ 进入 IPC 对象 suspend_thread 链表（若在等资源）
└─ 启动自身 thread_timer（若指定超时）
```

若定时器先到期，`_thread_timeout(thread)` 会：

```text
确认线程仍挂起
→ thread->error = -RT_ETIMEOUT
→ 从等待链表移除
→ 插入调度器就绪队列
→ 请求重新调度
```

## 四、内部线程初始化：`_thread_init()`

`_thread_init()` 只初始化线程特有字段；它不负责把对象作为 Thread 类登记到对象系统。

```text
rt_thread_init()
├─ rt_object_init(&thread->parent, Thread, name)
│  └─ 完成通用对象头和对象链表登记
└─ _thread_init(thread, ...)
   └─ 完成栈、入口、优先级、定时器等线程专属初始化
```

主要初始化内容：

```c
rt_sched_thread_init_ctx(thread, tick, priority);
thread->entry      = entry;
thread->parameter  = parameter;
thread->stack_addr = stack_start;
thread->stack_size = stack_size;
```

- `entry`：线程入口函数。
- `parameter`：入口函数参数。
- `stack_addr` / `stack_size`：线程栈边界和大小。
- `priority` / `tick`：交给调度器初始化优先级与同优先级时间片。

### 栈初始化

```c
rt_memset(thread->stack_addr, '#', thread->stack_size);
```

用 `'#'` 填充栈，供后续检测历史最大栈使用量。

```c
thread->sp = rt_hw_stack_init(thread->entry,
                               thread->parameter,
                               stack_top,
                               _thread_exit);
```

`rt_hw_stack_init()` 按 CPU 架构在新栈中构造“首次切换时的伪造寄存器现场”：

```text
初始 PC      → entry
初始参数     → parameter
初始返回地址 → _thread_exit
初始 SP      → thread->sp
```

首次调度到新线程时，CPU 看起来像从被保存的现场恢复，实际开始执行 `entry(parameter)`。

### 内置线程定时器

```c
rt_timer_init(&thread->thread_timer,
              thread->parent.name,
              _thread_timeout,
              thread,
              0,
              RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_THREAD_TIMER);
```

线程定时器为一次性定时器，到期回调是 `_thread_timeout`，参数就是所属线程。

## 五、相对延时、绝对周期延时

### `rt_thread_delay(tick)`：相对延时

```c
rt_err_t rt_thread_delay(rt_tick_t tick)
{
    return _thread_sleep(tick);
}
```

从调用 API 的当前 Tick 开始等待 `tick` 个 Tick。

```text
工作 30 Tick → rt_thread_delay(100)
→ 下轮开始相对本轮开始约过去 130 Tick
```

`rt_thread_mdelay(ms)` 仅是将毫秒转为 Tick 后进行相对延时：

```c
rt_tick_from_millisecond(ms) → _thread_sleep(tick)
```

### `rt_thread_delay_until(&last_tick, inc_tick)`：计划时间点延时

典型周期任务：

```c
rt_tick_t last_tick = rt_tick_get();

while (1)
{
    do_work();
    rt_thread_delay_until(&last_tick, 100);
}
```

含义：

```text
last_tick：上一次计划唤醒 Tick，函数会更新它
inc_tick：周期长度
目标时刻：last_tick + inc_tick
```

若工作耗时小于周期，函数只等待剩余 Tick，尽量保持固定节拍。

```text
上次计划点：1000
周期：100
工作后当前：1030
下一计划点：1100
等待：70 Tick
```

若工作已经超过周期，函数不再等待，将 `*tick` 重置为当前 Tick，重新建立后续周期基准。

普通延时正常到期时，内部超时回调会写 `-RT_ETIMEOUT` 唤醒线程；延时 API 恢复后会将其清回 `RT_EOK`，因此调用者正常得到成功结果。

## 六、统一线程控制：`rt_thread_control()`

```c
rt_thread_control(thread, cmd, arg);
```

是 C 风格的“对象 + 命令 + 参数”接口。

```text
RT_THREAD_CTRL_CHANGE_PRIORITY
  arg 是 rt_uint8_t *，指向新优先级。

RT_THREAD_CTRL_RESET_PRIORITY
  重新计算/恢复线程优先级，常与 mutex 优先级继承有关。

RT_THREAD_CTRL_STARTUP
  等价于 rt_thread_startup(thread)。

RT_THREAD_CTRL_CLOSE
  静态线程调用 detach；动态线程调用 delete；随后请求调度。

RT_THREAD_CTRL_BIND_CPU
  SMP 下绑定 CPU；本实现将小整数 CPU 编号编码在 void *arg 本身。
```

优先级调整会先锁住调度器，因为线程可能需要从原优先级就绪队列移除并插入新队列。

## 七、线程状态设置辅助函数

```c
_thread_get_suspend_state(suspend_flag)
```

仅进行参数到状态编码的转换：

```text
RT_INTERRUPTIBLE    → RT_THREAD_SUSPEND_INTERRUPTIBLE
RT_KILLABLE         → RT_THREAD_SUSPEND_KILLABLE
RT_UNINTERRUPTIBLE  → RT_THREAD_SUSPEND_UNINTERRUPTIBLE
```

它不修改线程。

```c
_thread_set_suspend_state(thread, suspend_flag)
```

才会写入线程状态：替换低 3 位基础状态，同时保留高位 `YIELD`、`SIGNAL` 等附加标志。

## 八、挂起：`rt_thread_suspend_to_list()`

```c
rt_thread_suspend_to_list(thread,
                          susp_list,
                          ipc_flags,
                          suspend_flag)
```

是线程挂起的核心通用函数：

```text
锁调度器
→ 从就绪队列移除线程
→ 设置 SUSPEND_xxx 状态
→ 若 susp_list 非空，加入对象等待链表
→ 停止旧 thread_timer
→ 解锁调度器
→ 调用 suspend Hook
```

`susp_list` 的例子：

```text
信号量不可用时：&sem->parent.suspend_thread
互斥锁不可用时：&mutex->parent.suspend_thread
```

`ipc_flags` 决定等待者排列：

```text
RT_IPC_FLAG_FIFO：先来先服务
RT_IPC_FLAG_PRIO：按优先级排列，高优先级优先
```

`rt_thread_suspend_with_flag()` 是传入 `susp_list = RT_NULL` 的简化包装。

`rt_thread_suspend(thread)` 则默认使用：

```c
RT_UNINTERRUPTIBLE
```

不要随意挂起其他线程。若目标线程持有 mutex、设备或其他资源，可能造成死锁、饥饿或系统状态不可预测。

## 九、恢复：`rt_thread_resume()`

```c
rt_thread_resume(thread)
```

恢复核心由 `rt_sched_thread_ready(thread)` 完成：

```text
确认线程仍处于挂起状态
→ 停止活动中的 thread_timer
→ 从 IPC 等待链表移除（若存在）
→ 设置为 READY
→ 插入调度器就绪队列
```

随后：

```c
rt_sched_unlock_n_resched(slvl);
```

解锁调度器并在必要时立即重新调度。若被恢复线程优先级更高，它可能马上抢占当前线程。

挂起/恢复对照：

```text
挂起：READY/RUNNING → SUSPEND → 可选加入 IPC 等待链表
恢复：IPC 等待链表 → 移除 → READY → 加入就绪队列
```

## 十、线程查询包装

```c
rt_thread_find(name)
```

复用对象系统：

```c
rt_object_find(name, RT_Object_Class_Thread)
```

```c
rt_thread_get_name(thread, buffer, size)
```

复用：

```c
rt_object_get_name(&thread->parent, buffer, size)
```

线程名称实际保存在 `thread->parent.name`，说明线程对象的通用身份仍由 `rt_object` 提供。

## 建议复习问题

1. 为什么线程入口函数返回后必须进入 `_thread_exit()`？
2. 为什么线程不能在仍使用自身栈时立即释放自己？
3. `thread_timer` 与 IPC 的 `suspend_thread` 链表各自解决什么问题？
4. `rt_thread_delay()` 与 `rt_thread_delay_until()` 的时间基准有什么不同？
5. 挂起与恢复时，线程分别在哪些链表/队列之间移动？
6. 为什么不能任意挂起其他线程？
