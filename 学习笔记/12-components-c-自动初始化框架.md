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
