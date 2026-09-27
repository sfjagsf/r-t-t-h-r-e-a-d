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

## 9. 函数级补充：enter/leave 的精确顺序

`rt_interrupt_enter()`：

```text
irq_nest 原子加一
  -> 调用 interrupt_enter_hook（若开启）
  -> 记录调试日志
```

先加一保证 enter hook 执行时，`rt_interrupt_get_nest()` 已能识别自己正处于 ISR。

`rt_interrupt_leave()`：

```text
记录当前嵌套深度
  -> 调用 interrupt_leave_hook
  -> irq_nest 原子减一
```

leave hook 在减一前执行，因此它观察到的嵌套深度仍包含“正在退出的这一层 ISR”。这对追踪嵌套中断尤其有用。

## 10. 这些函数不直接做调度

`irq.c` 的 `rt_interrupt_enter()` / `rt_interrupt_leave()` 本身只维护“是否在 ISR”和嵌套层数，不在其中直接选择下一线程。

典型路径：

```text
ISR 唤醒高优先级线程
  -> rt_schedule() 发现 irq_nest > 0
  -> 设置 irq_switch_flag，延后切换
  -> ISR 退出，irq_nest 归零
  -> 架构相关中断返回路径调用调度器的 IRQ switch 处理
```

这样不会在仍使用 ISR 栈帧和异常现场的中途切到普通线程。

## 11. 可选 IRQ 上下文链表

开启 `ARCH_USING_IRQ_CTX_LIST` 后，每 CPU 具有 `irq_ctx_head` 单链表：

```text
进入可追踪中断上下文 -> rt_interrupt_context_push()
退出该上下文           -> rt_interrupt_context_pop()
当前顶层 context        -> rt_interrupt_context_get()
```

它是 LIFO 栈式结构，用于架构保存/取得嵌套 IRQ 的上下文指针；普通应用不应直接维护它。

## 12. 弱符号与导出边界

`rt_interrupt_enter()`、`rt_interrupt_leave()`、`rt_interrupt_get_nest()` 和 `rt_hw_interrupt_is_disabled()` 都提供弱实现，BSP/架构可在必要时覆盖；`RTM_EXPORT` 则让动态模块配置下能查到这些 API。

但无论底层实现如何，BSP IRQ 入口/出口都必须正确配对调用 enter 与 leave；漏掉任意一次都会使 `irq_nest` 永远非零或下溢，进而破坏“中断退出后调度”的判断。

## 源码锚点：嵌套计数的实际增减

来源：`src/irq.c`：

```c
rt_weak void rt_interrupt_enter(void)
{
    rt_atomic_add(&(rt_interrupt_nest), 1);
    RT_OBJECT_HOOK_CALL(rt_interrupt_enter_hook, ());
}

rt_weak void rt_interrupt_leave(void)
{
    RT_OBJECT_HOOK_CALL(rt_interrupt_leave_hook, ());
    rt_atomic_sub(&(rt_interrupt_nest), 1);
}
```

入口先加计数再允许 enter hook；出口先执行 leave hook 再减计数。`rt_weak` 表示 BSP/架构可按需要覆盖默认实现。
