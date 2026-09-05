# `components.c`：自动初始化框架学习笔记

> 本笔记对应 `src/components.c`。当前部分解释 RT-Thread 如何通过链接器段收集并分阶段调用模块初始化函数。

## 1. 它解决的问题

驱动、组件、文件系统、网络协议栈等各自有初始化函数。若每加一个组件都去修改中央初始化函数，耦合高、顺序难维护。

RT-Thread 的方式：

```text
模块作者：用 INIT_*_EXPORT(fn) 声明初始化阶段
编译器：将 fn 的函数指针放入指定链接器段
链接器：按段名/阶段排列这些函数指针
components.c：遍历段边界间的指针并逐个调用
```

因此模块可自注册初始化动作，不需要修改一个全局函数列表。

## 2. 初始化阶段

`rtdef.h` 中常用宏对应的阶段：

```text
INIT_BOARD_EXPORT(fn)      → "1"
INIT_CORE_EXPORT(fn)       → "1.0"
INIT_SUBSYS_EXPORT(fn)     → "1.1"
INIT_PLATFORM_EXPORT(fn)   → "1.2"

INIT_PREV_EXPORT(fn)       → "2"
INIT_DEVICE_EXPORT(fn)     → "3"
INIT_COMPONENT_EXPORT(fn)  → "4"
INIT_ENV_EXPORT(fn)        → "5"
INIT_APP_EXPORT(fn)        → "6"
INIT_FS_EXPORT(fn)         → "6.0"
```

逻辑顺序：

```text
板级硬件
→ 内核前置/设备
→ 软件组件
→ 环境
→ 应用
→ 文件系统挂载后的后续动作
```

例：

```c
static int uart_board_init(void)
{
    /* UART 早期硬件准备 */
    return 0;
}
INIT_BOARD_EXPORT(uart_board_init);
```

它会自动被收集到 board 初始化阶段。

## 3. 边界标记函数

`components.c` 定义了空函数并通过 `INIT_EXPORT` 放入特殊阶段：

```text
rti_start       → "0"
rti_board_start → "0.end"
rti_board_end   → "1.end"
rti_end         → "6.end"
```

它们不做业务，作用是提供链接器段遍历的起止边界。

```text
__rt_init_rti_board_start
    ↓
INIT_BOARD/CORE/SUBSYS/PLATFORM 导出的函数
    ↓
__rt_init_rti_board_end
    ↓
后续 PREV/DEVICE/COMPONENT/ENV/APP/FS 函数
    ↓
__rt_init_rti_end
```

## 4. `rt_components_board_init()`

作用：执行板级初始化阶段函数。

非调试配置下核心循环：

```c
for (fn_ptr = &__rt_init_rti_board_start;
     fn_ptr < &__rt_init_rti_board_end;
     fn_ptr++)
{
    (*fn_ptr)();
}
```

`__rt_init_rti_board_start/end` 通常来自链接脚本，不是普通 C 手写数组。

此阶段一般在调度器启动前完成，适合时钟、引脚、早期串口、硬件资源等准备。

## 5. `rt_components_init()`

作用：执行 board 阶段之后的组件初始化。

```c
for (fn_ptr = &__rt_init_rti_board_end;
     fn_ptr < &__rt_init_rti_end;
     fn_ptr++)
{
    (*fn_ptr)();
}
```

它会遍历 PREV、DEVICE、COMPONENT、ENV、APP、FS 等阶段。该函数通常由系统 main 线程调用，因此许多组件可在普通线程上下文完成初始化。

## 6. 调试自动初始化

开启：

```c
RT_DEBUGING_AUTO_INIT
```

链接器段不只保存函数指针，而保存：

```c
struct rt_init_desc
{
    const char *fn_name;
    const init_fn_t fn;
};
```

系统会打印类似：

```text
initialize uart_board_init
:0 done
```

便于定位启动过程卡在哪个初始化函数、该函数返回什么结果。

## 7. 与对象 + 接口 + 组件的关系

```text
对象：驱动、设备、文件系统、协议栈等模块可创建各自对象
接口：每个模块提供自己的 init 函数
组件：INIT_*_EXPORT 让模块按阶段接入系统启动流程
```

自动初始化框架让组件“声明自己何时应初始化”，系统统一负责按顺序执行。

---

## 8. 后半段：为什么 RT-Thread 要接管 `main`

裸机程序通常在 C 运行库准备完成后直接进入用户的 `main()`。RT-Thread 必须先建立调度器、时间系统和系统线程，用户代码才能安全使用线程与 IPC。

因此，`components.c` 在 `RT_USING_USER_MAIN` 配置下为不同编译器提供入口包装：

| 编译器 | 包装方式 | 目的 |
|---|---|---|
| ARMCC | `$Sub$$main()` | 在原始 `main` 前插入内核启动；原始函数名为 `$Super$$main()` |
| IAR | `__low_level_init()` | 在 C 启动早期调用 `rtthread_startup()` |
| GCC | `entry()` | 链接参数把入口指向 `entry`，再调用 `rtthread_startup()` |

例如 GCC：

```c
int entry(void)
{
    rtthread_startup();
    return 0;
}
```

这不会丢弃用户 `main()`；而是使其稍后在 RT-Thread 的主线程中执行。

## 9. `main_thread_entry()`：运行组件与用户 `main()` 的线程入口

```c
static void main_thread_entry(void *parameter)
{
    rt_components_init();       /* 若开启 RT_USING_COMPONENTS_INIT */
    rt_hw_secondary_cpu_up();   /* 仅 SMP */
    main();
}
```

`main_thread_entry` 是内核主线程的入口，并非用户业务 `main` 本身：

```text
主线程首次被调度
    -> rt_components_init()：系统/应用阶段自动初始化
    -> SMP 时拉起其他 CPU
    -> 调用用户 main()
```

因此用户 `main()` 执行时，调度器已经工作，能够调用 RT-Thread 的线程、IPC、设备、定时器等 API。

## 10. `rt_application_init()`：创建主线程

```c
tid = rt_thread_create("main", main_thread_entry, RT_NULL,
                       RT_MAIN_THREAD_STACK_SIZE,
                       RT_MAIN_THREAD_PRIORITY, 20);
rt_thread_startup(tid);
```

该函数创建名为 `main` 的线程，并让其进入 `READY` 状态；调度器尚未开始时，它不会实际运行。

| 配置 | 创建接口 | TCB 与栈来源 |
|---|---|---|
| `RT_USING_HEAP` | `rt_thread_create()` | 从堆动态分配 |
| 未开启堆 | `rt_thread_init()` | `main_thread` 和 `main_thread_stack[]` 静态分配 |

堆版本依赖 `rt_hw_board_init()` 已初始化堆，这也是启动顺序不可颠倒的原因。

## 11. `rtthread_startup()`：内核启动总顺序

```text
关闭本 CPU 中断
  -> rt_hw_board_init()
  -> rt_show_version()
  -> rt_system_timer_init()
  -> rt_system_scheduler_init()
  -> rt_system_signal_init()       （可选）
  -> rt_application_init()         （创建 main 线程）
  -> rt_system_timer_thread_init() （可选软定时器线程）
  -> rt_thread_idle_init()
  -> rt_thread_defunct_init()
  -> rt_system_scheduler_start()
```

- **关闭中断**：避免初始化中的内核数据结构被中断提前访问。
- **板级初始化**：准备时钟、串口、中断控制器、堆等。
- **定时器与调度器**：准备时间管理、就绪链表和优先级位图。
- **系统线程**：main、软定时器服务、idle、defunct 回收线程进入可调度状态。
- **启动调度器**：选中最高优先级 `READY` 线程并首次切换上下文；通常不再返回。

## 12. 两阶段自动初始化：时机不能混淆

```text
rt_hw_board_init()
  -> BSP 通常调用 rt_components_board_init()
     -> 板级阶段：驱动、硬件、堆等

调度器启动
  -> main_thread_entry()
     -> rt_components_init()
        -> 系统/应用阶段：文件系统、网络、应用组件等
```

完整地看，`components.c` 的职责是：**用链接器段收集初始化函数，并把它们放在内核启动链的正确阶段执行。**
