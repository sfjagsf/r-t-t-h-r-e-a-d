# `irq.c`：中断上下文与嵌套计数学习笔记

> 本笔记对应 `src/irq.c`。它不实现具体外设中断控制器，而是向 RT-Thread 内核提供“当前是否在中断上下文”的统一判断基础。

## 1. 核心变量：`rt_interrupt_nest`

```text
单核：全局 rt_interrupt_nest
多核：每 CPU 的 rt_cpu_self()->irq_nest
```

语义：

```text
0：当前在线程上下文
1：当前处于第一层 ISR
>1：当前发生 ISR 嵌套
```

示例：

```text
线程运行：nest = 0
SysTick ISR 进入：nest = 1
UART ISR 嵌套进入：nest = 2
UART ISR 退出：nest = 1
SysTick ISR 退出：nest = 0
```

`interrupt_nest > 0` 用于内核判断 API 能否阻塞、Tick 是否确实从 ISR 调用、上下文切换应走线程路径还是中断退出路径。

## 2. BSP 的标准调用位置

典型 ISR 框架：

```c
void SysTick_Handler(void)
{
    rt_interrupt_enter();

    rt_tick_increase();

    rt_interrupt_leave();
}
```

```text
rt_interrupt_enter()：进入中断层，nest + 1
rt_interrupt_leave()：离开中断层，nest - 1
```

应用代码一般不应手工调用它们；应由 BSP/中断入口框架调用。

## 3. Hook 注册

```c
rt_interrupt_enter_sethook(hook);
rt_interrupt_leave_sethook(hook);
```

两函数只保存回调指针，分别在进入 ISR、离开 ISR 时调用。

用途：中断跟踪、执行时间统计、调试埋点。

限制：hook 位于中断路径，必须短小，不能阻塞、延时、等待 IPC 或进行复杂业务。

## 4. `rt_interrupt_enter()`

```text
原子执行 nest + 1
→ 调用可选 enter hook
→ 可选调试日志
```

它是 `rt_weak` 弱符号，架构/BSP 可提供同名强符号覆盖默认实现。

## 5. `rt_interrupt_leave()`

```text
可选调试日志
→ 调用 leave hook
→ 原子执行 nest - 1
```

leave hook 执行期间 nest 仍大于 0，因此系统仍将其视为中断上下文。

该函数本身不直接调用 `rt_schedule()`；若 ISR 中产生了调度请求，架构相关 `rt_hw_context_switch_interrupt()` 通常安排在安全的中断退出路径恢复目标线程。

## 6. `rt_interrupt_get_nest()`

```c
rt_uint8_t rt_interrupt_get_nest(void);
```

短暂关闭本地中断后读取当前 CPU 的 nest，再恢复原中断状态，避免本核嵌套进入/退出导致上下文判断的竞争窗口。

常见判断：

```c
if (rt_interrupt_get_nest() > 0)
{
    /* ISR 上下文：不能调用可能阻塞的 API */
}
```

## 7. 可选中断上下文链表

开启 `ARCH_USING_IRQ_CTX_LIST` 后，提供：

```text
rt_interrupt_context_push(ctx)：压入当前 CPU 的中断上下文单链表
rt_interrupt_context_pop()：弹出最内层中断上下文
rt_interrupt_context_get()：取得当前最内层 context
```

用于需要保存多层嵌套 ISR 上下文的复杂架构；简单平台可能不启用。

## 8. `rt_hw_interrupt_is_disabled()` 不等于中断上下文

默认弱实现：

```c
rt_bool_t rt_hw_interrupt_is_disabled(void)
{
    return RT_FALSE;
}
```

架构可覆盖它以读取 CPU 中断屏蔽寄存器。

必须区分：

```text
rt_interrupt_get_nest() > 0
    当前正在 ISR

rt_hw_interrupt_is_disabled() == RT_TRUE
    当前本地中断被屏蔽；线程上下文中也可能发生
```

```text
中断被关闭 ≠ 当前正在中断服务程序中
```
