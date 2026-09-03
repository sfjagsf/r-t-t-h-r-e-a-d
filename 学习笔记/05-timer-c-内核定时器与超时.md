# `timer.c`：RT-Thread 内核定时器与超时学习笔记

> 本笔记对应 `src/timer.c`。它实现用户定时器，也支撑线程延时、IPC 超时等待等机制。

## 1. 定时器在系统中的位置

```text
硬件 SysTick / 硬件时基定时器中断
    ↓
系统 Tick 递增
    ↓
rt_timer_check()
    ├─ 检查硬定时器，直接执行到期回调
    └─ 检查软定时器，释放信号量唤醒 timer 线程
```

RT-Thread 的 `rt_timer` 是**软件定时器对象**。多个软件定时器通常共享同一个系统 Tick 硬件来源；它不是“每个软件定时器各绑定一个 MCU 硬件定时器”。

## 2. `struct rt_timer` 的关键字段

```text
parent        rt_object 基础对象
timeout_func  到期后调用的回调函数
parameter     回调函数参数
init_tick     配置的相对延时或周期
timeout_tick  本次实际到期的绝对 Tick
row[]         跳表的多层链表节点
```

示例：

```text
当前 Tick = 1000
init_tick = 100
调用 rt_timer_start()
    ↓
timeout_tick = 1100
```

`init_tick` 是配置；`timeout_tick` 是每次 start 时由当前 Tick 推导出的本次实际截止时刻。

## 3. 硬定时器与软定时器

```text
硬定时器（hard timer）
    到期回调在 Tick 中断上下文直接执行
    响应快；回调必须极短、不能阻塞

软定时器（soft timer）
    Tick 中断释放 _soft_timer_sem
    timer 线程被唤醒后执行到期回调
    可做更多工作，但有调度延迟
```

硬定时器回调适合置标志、释放信号量、发通知等短操作；不适合阻塞式 I2C/SPI、动态内存、长循环、等待 IPC 等。

周期读取数据通常更适合普通线程配合 `rt_thread_delay_until()`；微秒级精确采样/触发则适合 MCU 硬件定时器、ADC/DMA 与中断协作。

## 4. 定时器跳表

硬、软定时器各自维护一套跳表：

```text
硬定时器：_timer_list[] + _htimer_lock
软定时器：_soft_timer_list[] + _stimer_lock
```

所有活动定时器按 `timeout_tick` 从早到晚排列。

```text
最早到期 ──► T1 → T2 → T3 ──► 最晚到期
```

源码中 `row[RT_TIMER_SKIP_LIST_LEVEL - 1]` 是包含所有活动定时器的基础链表；其他层只保留部分节点，作为快速索引。

一个活动定时器可能同时位于多个跳表层；因此 `_timer_remove()` 会循环移除 `timer->row[]` 的全部层节点。

Tick 会回绕，源码以“Tick 差值与 `RT_TICK_MAX / 2` 比较”判断到期先后，故 `init_tick` 被限制为：

```text
time < RT_TICK_MAX / 2
```

## 5. 生命周期接口

```text
静态定时器：rt_timer_init()   → rt_timer_detach()
动态定时器：rt_timer_create() → rt_timer_delete()
```

### 初始化/创建

`rt_timer_init()`：用户提供 `struct rt_timer` 的内存；先 `rt_object_init(... Timer ...)`，再调用内部 `_timer_init()`。

`rt_timer_create()`：先 `rt_object_allocate(... Timer ...)` 从内核堆申请对象，再调用 `_timer_init()`；失败返回 `RT_NULL`。

`_timer_init()`：

```text
保存 flag、timeout_func、parameter、init_tick
timeout_tick 置 0（尚未开始计时）
清除 ACTIVATED 标志
初始化 timer->row[]
```

### 拆除/删除

`rt_timer_detach()`：仅用于静态定时器；从跳表移除、停止、从对象系统移除，不释放用户内存。

`rt_timer_delete()`：仅用于动态定时器；从跳表移除、停止、从对象系统移除，并释放内核申请的对象内存。

## 6. 启动与停止

### `rt_timer_start()` / `_timer_start()`

```text
选择硬/软链表和对应锁
→ 若是 thread_timer，通知调度器
→ timeout_tick = rt_tick_get() + init_tick
→ 按到期时间插入跳表
→ 设置 RT_TIMER_FLAG_ACTIVATED
```

重复 `start` 会先移除旧位置，再按“当前 Tick + init_tick”重新计时；不是恢复旧的剩余时间。

两个相同到期 Tick 的定时器，先插入者先执行。

线程内嵌的 `thread_timer` 用于：

```text
rt_thread_delay()
rt_thread_delay_until()
IPC 等待超时
```

### `rt_timer_stop()`

```text
从跳表所有层移除
→ 清除 ACTIVATED
```

未运行时调用 `stop` 返回 `-RT_ERROR`。停止不释放对象；之后可重新 `start`，并从新的当前 Tick 重新计时。

## 7. 到期检查：`_timer_check()`

`_timer_check()` 总是检查最早到期者；若其已到期：

```text
从跳表移除
→ 一次性定时器清除 ACTIVATED
→ 临时挂入局部 list
→ 解锁
→ 执行 timeout_func(parameter)
→ 重新加锁
→ 周期定时器若仍有效且未被回调停止/删除，则重新 start
```

执行回调前先从活动表移除、并解锁，允许回调安全地操作定时器，也避免长时间持有定时器自旋锁。

本版本的周期定时器下一次期限按“回调结束后的当前 Tick + init_tick”重新计算；回调耗时会使后续周期相应后移。

## 8. `rt_timer_control()`

常用命令：

```text
GET_TIME          获取 init_tick
SET_TIME          设置 init_tick
SET_ONESHOT       改为一次性
SET_PERIODIC      改为周期性
GET_STATE         获取 ACTIVATED / DEACTIVATED
GET_REMAIN_TIME   当前实现返回 timeout_tick（绝对到期 Tick）
GET_FUNC/SET_FUNC 获取/设置回调函数
GET_PARM/SET_PARM 获取/设置回调参数
```

重要：`SET_TIME` 若发现定时器正在活动，会先从跳表移除并停止它，但**不会自动重新启动**：

```c
rt_timer_control(timer, RT_TIMER_CTRL_SET_TIME, &ticks);
rt_timer_start(timer);
```

`GET_REMAIN_TIME` 的名字容易误导：当前源码没有计算剩余 Tick，而是直接返回 `timeout_tick`。实际剩余 Tick 应自行计算：

```text
timeout_tick - rt_tick_get()
```

## 9. Tick 中断入口与 Tickless

`rt_timer_check()` 必须在系统 Tick 中断中调用：

```text
硬定时器：直接调用 _timer_check(_timer_list, ...)
软定时器：检查最早到期项；到期则 release _soft_timer_sem
```

SMP 下只有 CPU 0 负责全局定时器检查，防止多个核重复处理。

`rt_timer_next_timeout_tick()` 不执行回调，仅返回硬、软链表中最早的绝对到期 Tick；若无活动定时器返回 `RT_TICK_MAX`。它常用于 Tickless 低功耗：计算下次必要唤醒时间，让硬件不必每个 Tick 都触发中断。

## 10. 系统初始化

```text
rt_system_timer_init()
    初始化 _timer_list[] 各层链表头和 _htimer_lock

rt_system_timer_thread_init()（启用 RT_USING_TIMER_SOFT 时）
    初始化 _soft_timer_list[]、_stimer_lock、_soft_timer_sem
    初始化并启动系统 timer 线程
```

软定时器线程入口：

```c
while (1)
{
    _timer_check(_soft_timer_list, &_stimer_lock);
    rt_sem_take(&_soft_timer_sem, RT_WAITING_FOREVER);
}
```

它等待 Tick 中断的信号量通知，再在线程上下文执行已到期的软定时器回调。

## 11. 选型速记

```text
短小超时动作、快速通知          → 硬定时器
一般延时回调、可容忍调度延迟      → 软定时器
周期读取/处理业务数据             → 线程 + rt_thread_delay_until()
微秒级精确采样、PWM、触发         → MCU 硬件定时器 + ISR/DMA
```
