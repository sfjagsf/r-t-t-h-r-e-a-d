# `defunct.c`：线程延后回收学习笔记

> 本笔记对应 `src/defunct.c`。它处理已关闭（`CLOSE`）线程的最终资源回收。

## 1. 为什么线程不能自行释放资源

线程函数返回或被关闭时，线程仍在使用自己的栈执行退出和切换路径：

```text
线程 A 正在自己的 stack 上运行
→ A 结束
→ 若 A 立刻释放 A.stack
→ 后续退出/上下文切换代码继续使用该栈
→ 非法访问或系统崩溃
```

所以正确路径：

```text
线程结束
→ 状态变 CLOSE
→ 加入 defunct（待回收）链表
→ 调度到其他执行上下文
→ 由 idle 或 tsystem 线程安全释放其资源
```

## 2. 全局数据

```c
static rt_list_t _rt_thread_defunct;
static struct rt_spinlock _defunct_spinlock;
```

```text
_rt_thread_defunct：已结束、等待最终清理的线程队列
_defunct_spinlock：保护该链表，防止并发增删损坏 next/prev 指针
```

在 SMP 或 Smart 配置下，还会有：

```text
rt_system_thread（名称 tsystem）：专用后台回收线程
system_sem：有新的 defunct 线程时用于唤醒 tsystem
```

## 3. `rt_thread_defunct_enqueue(thread)`

作用：把已关闭线程加入 defunct 队列。

```text
获取 _defunct_spinlock
→ 将 RT_THREAD_LIST_NODE(thread) 插入 _rt_thread_defunct
→ 释放锁
→ SMP/Smart 下 release(system_sem)，通知 tsystem 清理
```

它不释放内存，仅转移线程链表归属：线程已经不在 READY/IPC 等待链表中，改为处于待回收链表。

## 4. `rt_thread_defunct_dequeue()`

作用：从 defunct 队列取出一个待清理线程。

```text
队列空：返回 RT_NULL
队列非空：取头后第一个节点，移出链表，返回对应 rt_thread 指针
```

函数只完成安全出队；实际清理由 `rt_defunct_execute()` 完成。

## 5. `rt_defunct_execute()`

作用：循环处理全部 defunct 线程，直至队列为空。

每个线程的清理顺序：

```text
① 可选：销毁动态模块（RT_USING_MODULE）
② 可选：释放信号相关资源（RT_USING_SIGNALS）
③ 提前保存 thread->cleanup 回调指针
④ 判断线程是静态对象还是动态对象
⑤ 静态线程：rt_object_detach()
⑥ 执行 cleanup(thread) 回调（若存在）
⑦ 动态线程：先释放 stack，再 rt_object_delete() 释放 TCB
```

必须提前保存 `cleanup`，因为动态线程最后会释放 `struct rt_thread`；释放后不能再访问 `thread->cleanup`。

### 静态线程

```c
static struct rt_thread worker;
static rt_uint8_t worker_stack[512];
```

内存由用户提供，内核只：

```text
从对象系统 detach
不释放 worker 和 worker_stack
```

### 动态线程

```c
rt_thread_create(...)
```

内核申请 TCB 与栈，回收时：

```text
先 RT_KERNEL_FREE(thread->stack_addr)
再 rt_object_delete(thread)
```

若启用硬件栈保护，实际释放的是 `stack_buf`。

## 6. `rt_thread_system_entry()`（条件编译）

仅在 `RT_USING_SMP` 或 `RT_USING_SMART` 时存在，是 `tsystem` 专用回收线程入口：

```text
while (1)
    等待 system_sem（永久等待）
    → 被 defunct_enqueue() 唤醒
    → rt_defunct_execute() 清空当前所有待回收线程
```

没有待清理线程时它阻塞，不消耗 CPU。

## 7. `rt_thread_defunct_init()`

系统初始化阶段调用：

```text
初始化 _defunct_spinlock
```

`_rt_thread_defunct` 已通过 `RT_LIST_OBJECT_INIT` 静态初始化为空链表。

SMP/Smart 下额外：

```text
初始化初值为 0 的 system_sem
创建 tsystem 线程：优先级 RT_THREAD_PRIORITY_MAX - 2，时间片 32
startup tsystem
```

`tsystem` 优先级很低，表明资源回收属于后台工作，不应抢占正常实时业务线程。

## 8. 生命周期速记

```text
thread entry return / close
→ CLOSE
→ rt_thread_defunct_enqueue()
→ _rt_thread_defunct
→ idle 或 tsystem 调用 rt_defunct_execute()
→ 静态线程 detach；动态线程释放 stack + TCB
```
