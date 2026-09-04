# `idle.c`：空闲线程与后台工作学习笔记

> 本笔记对应 `src/idle.c`。idle 线程是每个 CPU 的最低优先级兜底线程：所有普通线程都不可运行时，它保证调度器仍有线程可选。

## 1. 核心作用

```text
所有普通线程等待 IPC / delay / SUSPEND
    ↓
调度器运行本 CPU 的 idle 线程
    ↓
执行空闲 hook、后台资源回收、低功耗处理
```

每 CPU 一条静态 idle 线程：

```c
static struct rt_thread idle_thread[RT_CPUS_NR];
static rt_uint8_t idle_thread_stack[RT_CPUS_NR][IDLE_THREAD_STACK_SIZE];
```

优先级：

```c
RT_THREAD_PRIORITY_MAX - 1
```

即最低优先级；任意普通 READY 线程都会优先运行。

idle 线程不能被挂起、等待 IPC 或延时，否则所有普通线程阻塞时系统会没有 READY 线程，调度器失去兜底对象。

## 2. Idle hook 表

启用 `RT_USING_IDLE_HOOK` 后：

```c
static void (*idle_hook_list[RT_IDLE_HOOK_LIST_SIZE])(void);
```

默认可容纳 4 个 hook。hook 在 idle 线程循环中反复执行，必须短小且不能阻塞。

### `rt_thread_idle_sethook(hook)`

```text
获取 _hook_spinlock
→ 查找第一个 RT_NULL 空槽
→ 保存 hook 函数指针
→ 成功返回 RT_EOK；表满返回 -RT_EFULL
```

只注册，不会立即调用。

### `rt_thread_idle_delhook(hook)`

```text
获取 _hook_spinlock
→ 查找相同函数指针
→ 将槽设为 RT_NULL
→ 成功返回 RT_EOK；未找到返回 -RT_ENOSYS
```

删除阻止以后循环再次调用该 hook，但不撤销已经在执行中的一次调用。

## 3. `idle_thread_entry()`

所有 idle 线程的入口。

### SMP 的次核

非 CPU 0：

```c
while (1)
    rt_hw_secondary_cpu_idle_exec();
```

这是架构相关空闲处理，通常包含 WFI/WFE、低功耗等待或其他平台机制。

### CPU 0 通用空闲循环

```text
循环执行已注册 idle hook
→ 单核、非 Smart 时执行 rt_defunct_execute()
→ 若启用 PM，调用 rt_system_power_manager()
```

`rt_defunct_execute()` 与 `defunct.c` 对接：清理已 `CLOSE` 的线程。SMP/Smart 系统改由专用 `tsystem` 线程处理，不依赖 idle 时机。

`rt_system_power_manager()` 是空闲进入低功耗的入口，可能降频、sleep、WFI 或实施 tickless 策略。

## 4. `rt_thread_idle_init()`

系统启动时创建每 CPU 的 idle 线程：

```text
初始化 hook 自旋锁（若启用 hook）
→ 对每个 CPU：初始化静态 idle TCB 与静态栈
→ SMP 下绑定到对应 CPU
→ 保存到 rt_cpu_index(i)->idle_thread
→ rt_thread_startup()，使其 INIT → READY
```

线程名称通常为：

```text
tidle0、tidle1、...
```

idle TCB 和 stack 是静态内存，不依赖动态堆；即使 heap 不可用，系统也始终有兜底线程。

## 5. 辅助查询接口

### `rt_thread_idle_gethandler()`

返回当前 CPU 的 idle TCB：

```text
CPU 0 → &idle_thread[0]
CPU 1 → &idle_thread[1]
```

只获取指针，不创建、启动或切换线程。

### `rt_thread_is_idle_thread(thread)`

遍历每 CPU 的 `idle_thread[]`，判断给定 TCB 是否为任意 idle 线程：

```text
RT_TRUE：是 idle 线程
RT_FALSE：不是，或传入 RT_NULL
```

用于拒绝对 idle 线程做阻塞、挂起等破坏调度兜底的操作。
