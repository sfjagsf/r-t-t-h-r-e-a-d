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

### 3.1 Tick 是模 `2^32` 的逻辑时钟

当前源码中：

```c
typedef rt_uint32_t rt_tick_t;
#define RT_TICK_MAX RT_UINT32_MAX
```

因此 Tick 在 `0xFFFFFFFF` 后会回到 `0x00000000`。它通常表示“自启动以来的累计 Tick”，但严格说是一个 32 位循环计数器：

```text
0xFFFFFFFE -> 0xFFFFFFFF -> 0x00000000 -> 0x00000001
```

若 `RT_TICK_PER_SECOND = 1000`，一次完整回绕约为 49.7 天；若为 100 Hz，约为 497 天。

### 3.2 `rt_tick_get()` 为什么使用原子读取

```c
rt_tick_t rt_tick_get(void)
{
    return (rt_tick_t)rt_atomic_load(&(rt_tick));
}
```

Tick 可能被 Tick ISR 更新，同时被普通线程或其他 CPU 读取。`rt_atomic_load()` 保证单次读取不会得到撕裂的半更新值；但两次独立读取之间 Tick 仍可能改变。

### 3.3 回绕公式的推导

当 `tnow < base` 时，源码计算：

```c
RT_TICK_MAX - base + tnow + 1
```

例：

```text
base = 0xFFFFFFF0
now  = 0x00000010

delta = 0xFFFFFFFF - 0xFFFFFFF0 + 0x10 + 1
      = 0x20
      = 32 Tick
```

`+1` 表示从 `0xFFFFFFFF` 走到 `0x00000000` 也经过了一个 Tick。

因此，不能用 `now >= deadline` 这种普通数值比较判断跨回绕超时。更稳妥的相对时间模式是：

```c
rt_tick_t start = rt_tick_get();

/* ... */
if (rt_tick_get_delta(start) >= timeout)
{
    /* 已经过 timeout Tick */
}
```

`rt_tick_get_delta()` 返回 32 位模运算意义下的差值，适用于普通的短延时/超时；它不能区分“未经过 Tick”和“已完整回绕 `2^32` Tick”这种极长时间间隔。

### 3.4 `rt_tick_set()` 的实际副作用

```c
void rt_tick_set(rt_tick_t tick)
{
    rt_atomic_store(&(rt_tick), tick);
}
```

它只修改当前 Tick 值，不会主动：

```text
调用 rt_tick_increase()
检查软件定时器
立刻唤醒延时/IPC 超时线程
重新计算已有 timer/thread_timer 的 timeout_tick
```

已有定时器、线程延时和 IPC 超时都可能持有基于旧时间轴的绝对 `timeout_tick`。随意向前或向后设置 Tick，会使这些 deadline 提前到期、异常延后或失去原来的时间意义。

它适合启动校准、仿真/单元测试、Tickless 休眠恢复等由系统统一掌控的场景；不应被普通业务线程用作“重置计时器”。

### 3.5 SMP 下的系统 Tick 基准

SMP 当前源码把对外系统 Tick 映射到 CPU0：

```c
#define rt_tick rt_cpu_index(0)->tick
```

各 CPU 可维护本地 Tick 统计，但系统软件定时器到期检查主要由主 CPU 协调，避免多颗 CPU 同时处理同一批定时器。

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

## 源码锚点：一次 Tick 的真实顺序

来源：`src/clock.c` 的 `rt_tick_increase()`：

```c
rt_atomic_add(&(rt_tick), 1);
rt_sched_tick_increase(1);
rt_timer_check();
```

单核下这三行依次推进全局 Tick、递减当前线程时间片、检查定时器。SMP 分支先递增 `rt_cpu_self()->tick`，且非 CPU0 会在定时器检查前返回；因此全局 timer 回调不会在多个核重复运行。
