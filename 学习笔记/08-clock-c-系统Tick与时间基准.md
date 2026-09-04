# `clock.c`：系统 Tick 与时间基准学习笔记

> 本笔记对应 `src/clock.c`。它将硬件时基中断接入 RT-Thread 内核：维护系统 Tick、推进线程时间片、检查内核定时器，并提供毫秒/Tick 转换。

## 1. 总体 Tick 中断链

```text
硬件 SysTick / 硬件时基定时器 ISR
    ↓
rt_tick_increase() 或 rt_tick_increase_tick(n)
    ↓
执行可选 Tick hook
    ↓
更新 CPU/线程运行时间统计（若开启）
    ↓
全局 Tick 增加
    ↓
rt_sched_tick_increase()：当前线程时间片递减
    ↓
rt_timer_check()：检查硬、软软件定时器
    ↓
中断退出路径可能执行延后的线程切换
```

`rt_tick_increase()` 必须在中断上下文调用：

```c
RT_ASSERT(rt_interrupt_get_nest() > 0);
```

## 2. 全局 Tick

单核：

```c
static volatile rt_atomic_t rt_tick = 0;
```

表示系统启动至今经过的 Tick 数；它的单位不是固定毫秒，而取决于：

```c
RT_TICK_PER_SECOND
```

```text
1000 Hz：1 Tick = 1 ms
100 Hz ：1 Tick = 10 ms
```

SMP 下，CPU 0 的 Tick 作为全局时间基准；各核也可推进本地 Tick 和本核线程的时间片。全局软件定时器检查只在 CPU 0 进行，防止同一回调被多个核重复执行。

## 3. 读取、设置与 Tick 回绕

```c
rt_tick_get()          // 原子读取当前绝对 Tick
rt_tick_set(tick)      // 原子写入 Tick，通常仅供系统恢复、测试或校时
rt_tick_get_delta(base)// 计算从 base 到当前的经过 Tick
```

`rt_tick_get_delta()` 显式处理 Tick 溢出回绕：

```text
base = 0xFFFFFFF0
now  = 0x00000010
```

虽然数值上 `now < base`，函数仍能得到实际经过 Tick 数。不要仅用普通大小关系判断跨回绕的时间先后。

随意调用 `rt_tick_set()` 会影响线程延时、IPC 超时和软件定时器，不应作为普通业务接口使用。

## 4. Tick hook

```c
rt_tick_sethook(void (*hook)(void));
```

它只注册回调地址；每次 Tick 中断中通过 `RT_OBJECT_HOOK_CALL` 调用。

因为调用频率很高且处于中断路径，hook 必须极短、不可阻塞，不能做长循环、等待 IPC、阻塞式外设访问等操作。

## 5. `rt_tick_increase()` 与批量推进

```c
rt_tick_increase();             // 推进 1 Tick
rt_tick_increase_tick(tick);    // 一次推进多个 Tick
```

两个函数流程相同，区别仅为推进数量。批量版本通常用于 Tickless 低功耗：MCU 休眠期间不逐 Tick 产生中断，醒来后根据实际经过时间一次补偿多个 Tick。

每次推进会调用：

```c
rt_sched_tick_increase(tick);
```

使当前运行线程的 `remaining_tick` 递减；耗尽后设置 YIELD 并请求重调度。同优先级线程可由此轮转。

随后调用：

```c
rt_timer_check();
```

```text
硬定时器：在 Tick ISR 中直接执行到期回调
软定时器：Tick ISR 释放 timer 线程信号量，回调在线程上下文执行
```

## 6. 毫秒转 Tick：`rt_tick_from_millisecond()`

```c
rt_tick_t rt_tick_from_millisecond(rt_int32_t ms);
```

用于将用户常用的毫秒单位转换为内核 Tick：

```c
rt_thread_delay(rt_tick_from_millisecond(10));
```

转换对不足一个 Tick 的时间采用**向上取整**，保证“请求延时”不会因为整数截断而变成 0 Tick。

例如 `RT_TICK_PER_SECOND = 100`：

```text
1 Tick = 10 ms
请求 1 ms  → 1 Tick，实际约等待 10 ms
请求 11 ms → 2 Tick，实际约等待 20 ms
```

特殊输入：

```text
ms < 0：返回 RT_WAITING_FOREVER（永久等待）
ms = 0：返回 0（不等待）
```

## 7. Tick 转毫秒：`rt_tick_get_millisecond()`

```c
rt_weak rt_tick_t rt_tick_get_millisecond(void);
```

返回系统启动以来的大致/准确毫秒数，但只有当：

```text
1000 % RT_TICK_PER_SECOND == 0
```

时能用整数精确换算。

```text
1000 Hz、500 Hz、100 Hz：可以精确表示毫秒
128 Hz：1 Tick = 7.8125 ms，无法仅靠整数 Tick 精确表示每毫秒
```

频率不满足条件时默认实现会给出编译警告并返回 0；它是 `rt_weak` 弱符号，BSP/应用可用更高精度硬件定时器实现同名强符号函数覆盖。

## 8. 可选 CPU 使用率统计

开启：

```c
RT_USING_CPU_USAGE_TRACER
```

Tick 中断会调用 `_update_process_times(tick)`，累计当前线程/CPU 的运行 Tick。统计结构包括：

```text
user / system / irq / idle
```

它们是累计 Tick，不是直接百分比；利用率需通过“分类累计 Tick ÷ 总 Tick”计算。
