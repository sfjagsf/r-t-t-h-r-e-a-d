# `kservice.c`：内核通用服务学习笔记

> 本笔记对应 `src/kservice.c`。它提供不属于某个具体内核对象的基础服务：位图操作、控制台输出、回溯、堆 API 包装、断言等。当前记录已讲部分，后续函数继续追加。

## 1. 文件定位

```text
scheduler_up.c / scheduler_mp.c
  -> __rt_ffs()：从 READY 优先级位图找到最高优先级

components.c
  -> rt_show_version()：系统启动时打印版本

所有内核模块
  -> rt_kprintf() / rt_kputs()：输出诊断信息
```

## 源码锚点：通用 `rt_malloc()` 负责锁，算法实现由配置选择

来源：`src/kservice.c`：

```c
rt_weak void *rt_malloc(rt_size_t size)
{
    rt_base_t level;
    void *ptr;
    level = _heap_lock();
    ptr = _MEM_MALLOC(size);
    _heap_unlock(level);
    RT_OBJECT_HOOK_CALL(rt_malloc_hook, (&ptr, size));
    return ptr;
}
```

`_MEM_MALLOC` 会按编译配置映射到 small mem、memheap、slab 等实际算法；这层统一封装先加 heap 锁、后调用算法、最后通知 hook。`rt_free()` 同样经 `_heap_lock()` 后调用 `_MEM_FREE(ptr)`。

它是“内核公共工具层”，而非线程、IPC 或调度器本体。

## 2. `__rt_ffs()` 与 `__rt_fls()`

| 函数 | 含义 | 查找方向 | 返回值 |
| --- | --- | --- | --- |
| `__rt_ffs(value)` | first set bit | 从最低位向高位找第一个 `1` | 位置从 1 开始；输入 0 返回 0 |
| `__rt_fls(value)` | last set bit | 找最高的 `1` | 位置从 1 开始；输入 0 返回 0 |

示例：

```text
value = 0b0010_1000
__rt_ffs(value) = 4
__rt_fls(value) = 6
```

### 调度器为什么使用 `__rt_ffs()`

优先级位图中 bit0 对应优先级 0，而数值越小优先级越高：

```text
ready bitmap = 0b0010_1001
__rt_ffs() = 1
__rt_ffs() - 1 = 0
```

因此调度器能快速定位优先级 0 的 READY 队列，而不必顺序扫描所有优先级。

### 三种实现选择

| 配置/方案 | 实现 | 权衡 |
| --- | --- | --- |
| 默认 | 按字节检查 + 256 项查表 | 直观、可移植 |
| `RT_USING_TINY_FFS` | 最低置位 bit 映射到 37 项表 | 常量更小，位运算更难理解 |
| `RT_USING_CPU_FFS` | CPU/BSP 提供实现 | 可利用 CLZ/CTZ 等硬件指令 |

`RT_USING_TINY_FFS` 中：

```c
(value & (value - 1)) ^ value
```

会只保留 `value` 最低的那个 `1`；再通过 `% 37` 查表换算为一基 bit 位置。

## 3. `rt_show_version()`：启动版本输出

```c
void rt_show_version(void)
{
    rt_kprintf("... %d.%d.%d build %s %s ...",
               RT_VERSION_MAJOR, RT_VERSION_MINOR, RT_VERSION_PATCH,
               __DATE__, __TIME__);
}
```

`components.c` 的 `rtthread_startup()` 调用它，输出 RT-Thread 类型（普通/Nano/Smart）、版本和构建时间。它只做诊断输出，不参与调度。

## 4. 控制台设备与设备对象模型

开启 `RT_USING_CONSOLE`、`RT_USING_DEVICE` 后，内核维护当前控制台设备 `_console_device`：

```c
rt_device_t rt_console_get_device(void);
rt_device_t rt_console_set_device(const char *name);
```

`rt_console_set_device("uart1")` 的逻辑：

```text
rt_device_find("uart1")
  -> 从 Device 对象容器找到设备
  -> 关闭旧 console（若有）
  -> 以读写 + STREAM 模式打开新设备
  -> _console_device 指向新设备
```

之后 `rt_kprintf()` 的输出会经由设备的 `rt_device_write()` 发送到该设备。

## 5. 弱符号回退：`rt_hw_console_output()`

```c
rt_weak void rt_hw_console_output(const char *str)
{
    RT_UNUSED(str);
}
```

未设置控制台设备时，内核调用此函数。RT-Thread 提供空弱实现；BSP 可以提供同名普通函数覆盖它，例如直接驱动早期 UART。

```text
已有 console device -> rt_device_write()
没有 console device -> rt_hw_console_output()
                       -> BSP 可覆盖弱函数输出
```

这使通用内核不依赖任何具体芯片或串口驱动。

## 6. 输出开关

若开启 `RT_USING_CONSOLE_OUTPUT_CTL`：

```c
rt_console_output_set_enabled(RT_FALSE);
```

会使 `rt_kputs()` / `rt_kprintf()` 直接返回，不再产生输出。适用于高频日志抑制和性能测试。

配对查询接口是 `rt_console_output_get_enabled()`；`rt_kputs()` 与 `rt_kprintf()` 都先查询该状态再决定是否输出。

## 7. `rt_kputs()`、`_kputs()`、`rt_kprintf()`

```text
rt_kputs(str)
  -> 检查输出开关
  -> _kputs(str, rt_strlen(str))

rt_kprintf(fmt, ...)
  -> rt_vsnprintf() 格式化到静态 rt_log_buf
  -> _kputs(rt_log_buf, length)
```

`_kputs()` 是最终输出分流点：有 console device 则 `rt_device_write()`，否则走 BSP 弱函数。

`rt_kprintf()` 是弱函数，可由项目自己的日志实现用同名普通函数替换。格式化结果会限制在 `RT_CONSOLEBUF_SIZE - 1` 内，过长输出会被截断。

## 8. `RT_USING_THREADSAFE_PRINTF`：为什么有两把锁

`rt_kprintf()` 使用全局静态缓冲区：

```c
static char rt_log_buf[RT_CONSOLEBUF_SIZE];
```

并发调用会造成缓冲区覆盖、设备输出内容交错。开启线程安全打印后：

| 保护目标 | 机制 | 作用 |
| --- | --- | --- |
| `rt_log_buf` 格式化缓冲区 | `_prbuf_lock` | 不允许多个线程同时格式化到同一缓冲区 |
| 控制台设备输出所有权 | `_syscon_lock`、`_pr_curr_user` | 防止多条日志在设备层交错 |
| 当前输出线程 | `rt_enter_critical()` | 输出期间避免被普通线程调度切走 |

当控制台被另一线程占用时，`_console_take()` 会先释放自旋锁再 `rt_thread_yield()`，随后重试；它不持续持锁忙等。`_pr_curr_user_nested` 允许同一个线程嵌套打印，只有嵌套计数回到 0 才真正释放控制台所有权。

## 9. 回溯（Backtrace）：通用框架与架构职责

回溯使用统一栈帧描述：

```c
struct rt_hw_backtrace_frame
{
    rt_uintptr_t fp;   /* Frame Pointer，栈帧指针 */
    rt_uintptr_t pc;   /* Program Counter，代码地址 */
};
```

通用内核不假设 CPU 的寄存器和栈布局，而规定两个由 CPU 架构/BSP实现的接口：

| 架构接口 | 作用 |
| --- | --- |
| `rt_hw_backtrace_frame_get(thread, &frame)` | 得到指定线程最内层栈帧 |
| `rt_hw_backtrace_frame_unwind(thread, &frame)` | 将当前帧推进到调用者帧 |

`kservice.c` 的两个实现是弱函数，默认返回 `-RT_ENOSYS`；若 BSP 未覆盖，完整回溯不可用。

### `rt_backtrace()`：当前线程

```text
取得当前线程和当前 fp/pc
  -> 先 unwind 一次，跳过 rt_backtrace() 自己
  -> rt_backtrace_frame() 逐帧打印 PC
```

输出的是地址，需要用：

```text
addr2line -e rtthread.elf -a -f <PC地址>
```

翻译为函数名、源文件和行号。

### 其他回溯接口

| 接口 | 用途 |
| --- | --- |
| `rt_backtrace_frame()` | 从给定帧逐层打印地址 |
| `rt_backtrace_to_buffer()` | 不打印，将 PC 链保存到用户数组；可用 `skip` 额外跳过调用帧 |
| `rt_backtrace_formatted_print()` | 打印已有的 PC 地址数组 |
| `rt_backtrace_thread(thread)` | 回溯指定线程，需要架构代码从其 TCB/保存栈中构造初始帧 |

在 `RT_USING_LIBC` 与 `RT_USING_FINSH` 下，FinSH 导出：

```text
backtrace
backtrace <线程对象地址>
```

后者会通过 Thread 对象容器确认目标 TCB，再执行 `rt_backtrace_thread()`。

## 10. 可选 CPU 使用率统计

开启 `RT_USING_CPU_USAGE_TRACER` 后，`clock.c` 在 Tick 中累计线程运行时间和每 CPU 时间统计；`kservice.c` 提供：

```c
rt_uint8_t rt_thread_get_usage(rt_thread_t thread);
```

它返回最近采样窗口中的整数百分比 `0~100`：

```text
thread_delta = 本线程当前累计时间 - 上次快照
total_delta  = 所有 CPU 的 user + system + idle 时间增量总和
usage        = thread_delta * 100 / total_delta
```

它不是开机以来平均值。采样周期由 `RT_CPU_USAGE_CALC_INTERVAL_MS` 决定；首次调用仅建立快照，初始使用率为 0。双核中一个线程长期占满一个核，按“全部 CPU 容量”计算可能约为 50%。

统计遍历 Thread 对象容器时使用对象容器的 `spinlock`，避免与线程创建、退出和对象链表增删并发冲突。

## 11. 统一系统堆接口：算法与 API 分层

```text
应用 / 内核代码
  -> rt_malloc() / rt_realloc() / rt_calloc() / rt_free()
  -> kservice.c：锁、Hook、统一入口
  -> mem.c / memheap.c / slab.c：实际分配算法
```

当前堆算法在编译期选定：

| 配置 | 底层实现 |
| --- | --- |
| `RT_USING_SMALL_MEM_AS_HEAP` | `rt_smem_*`，来自 `mem.c` |
| `RT_USING_MEMHEAP_AS_HEAP` | `rt_memheap_*`，来自 `memheap.c` |
| `RT_USING_SLAB_AS_HEAP` | `rt_slab_*`，来自 `slab.c` |

上层始终调用统一的 `rt_malloc()` / `rt_free()`，不用了解具体算法。

## 12. 堆 Hook

开启 `RT_USING_HOOK` 后可注册：

```c
rt_malloc_sethook(...);
rt_realloc_set_entry_hook(...);
rt_realloc_set_exit_hook(...);
rt_free_sethook(...);
```

Hook 参数使用 `void **ptr`，可观察地址、尺寸和成功/失败结果。malloc/realloc exit/free 等 Hook 在堆锁之外执行，避免 Hook 中的日志或统计逻辑直接形成堆锁递归。常规 Hook 应只记录，不应随意改写返回指针。

## 13. 系统堆锁

| 配置 | `_heap_lock()` 的机制 | 主要适用范围 |
| --- | --- | --- |
| `RT_USING_HEAP_ISR` | `rt_spin_lock_irqsave()` | 堆可在 ISR 使用 |
| `RT_USING_MUTEX` | 堆专用 mutex | 普通线程上下文 |
| 其他 | `rt_enter_critical()` | 简化配置 |

线程系统尚未启动、`rt_thread_self()==RT_NULL` 时，mutex 版本不尝试取 mutex；启动阶段没有多线程竞争。

若开启 `RT_USING_UTESTCASES`，源码还会以 `rt_heap_lock()` / `rt_heap_unlock()` 名字导出内部 `_heap_lock()` / `_heap_unlock()`，仅供单元测试观察锁行为；应用代码不应依赖这两个测试入口。

## 14. 初始化系统堆

```c
rt_system_heap_init_generic(begin_addr, end_addr);
```

流程：

```text
BSP 提供可用 RAM 的起止地址
  -> 上/下对齐边界
  -> _MEM_INIT("heap", aligned_start, aligned_size)
  -> 初始化堆并发保护锁
```

`rt_system_heap_init()` 是弱函数，BSP 可以覆盖它，以便在通用初始化前后加入 heap sanitizer、多内存区处理或板级检查。

## 15. `rt_malloc` 家族

### `rt_malloc(size)`

```text
取得堆锁 -> _MEM_MALLOC(size) -> 释放堆锁 -> malloc Hook -> 返回地址/RT_NULL
```

### `rt_realloc(ptr, newsize)`

底层可能原地扩展，也可能分配新块、复制旧数据、释放旧块。正确写法：

```c
void *new_ptr = rt_realloc(ptr, new_size);
if (new_ptr != RT_NULL)
{
    ptr = new_ptr;
}
```

不要直接把返回值覆盖旧指针，否则失败时可能遗失仍有效的旧地址。

### `rt_calloc(count, size)`

等于 `rt_malloc(count * size)` 后以 `rt_memset()` 清零。当前源码未显式检查 `count * size` 乘法溢出；尺寸来自外部数据时调用者应先验证。

### `rt_free(ptr)` 与 `rt_memory_info()`

`rt_free(RT_NULL)` 是无操作。其他地址必须确实由匹配的 `rt_malloc` 家族接口返回，不能重复释放或释放栈/静态内存。

`rt_memory_info(&total, &used, &max_used)` 返回系统堆总容量、当前用量和历史峰值用量。

## 16. slab 专用页分配

仅 `RT_USING_SLAB_AS_HEAP` 时提供：

```c
rt_page_alloc(npages);
rt_page_free(addr, npages);
```

它管理连续页，适合页粒度或大块连续内存需求，不等同于普通 `rt_malloc()` 小块分配。

## 17. 指定对齐分配

```c
void *rt_malloc_align(rt_size_t size, rt_size_t align);
void rt_free_align(void *ptr);
```

适用于 DMA、缓存行或硬件描述符要求地址对齐的场景。实现会额外申请空间，计算对齐后的用户地址，并在用户地址前一个指针宽度保存原始 `rt_malloc()` 地址：

```text
[额外空间][真实地址 real_ptr][对齐后的用户地址 align_ptr ...]
                              ^
                           返回地址
```

因此 `align_ptr` 必须由 `rt_free_align()` 释放；不能直接 `rt_free(align_ptr)`。实现使用 `align - 1` 位掩码，调用者应传入 2 的幂对齐值，如 4、8、16、32、64。

## 18. BSP 弱硬件接口

| 弱接口 | 默认行为 | BSP 应做什么 |
| --- | --- | --- |
| `rt_hw_us_delay(us)` | 打印不支持警告 | 用硬件定时器/循环实现微秒延时 |
| `rt_hw_cpu_reset()` | 打印警告后返回 | 实现芯片复位寄存器操作 |
| `rt_hw_cpu_shutdown()` | 关中断、触发断言 | 实现可靠停机/低功耗行为 |

默认实现只用于暴露“板级能力尚未实现”，不能作为真正硬件行为依赖。

`RT_HW_BACKTRACE_FRAME_GET_SELF` 也可由 `cpuport.h` 覆盖；GNU 默认版用 `__builtin_frame_address()` 与标签地址取得当前 `fp/pc`，非 GNU 默认版返回 0，因此无法启动回溯。

## 19. 断言：`RT_ASSERT` 与 `rt_assert_handler()`

开启 `RT_DEBUGING_ASSERT` 后：

```c
RT_ASSERT(EX)
```

在 `EX` 为假时等价于：

```c
rt_assert_handler(#EX, __FUNCTION__, __LINE__);
```

默认处理：

```text
输出失败表达式、函数、行号
  -> rt_backtrace()
  -> 无限循环停机，保留故障现场
```

若运行在动态模块中，源码会调用 `dlmodule_exit(-1)` 结束该模块；若已用 `rt_assert_set_hook()` 注册断言 Hook，则由 Hook 接管处理策略（例如记录故障后复位）。Hook 必须谨慎，断言表示内核假设或编程不变量已被破坏。

关闭 `RT_DEBUGING_ASSERT` 后，`RT_ASSERT(EX)` 不会进入 handler；不要把必须执行的业务副作用依赖在断言表达式中。

## 20. 全文件速记

```text
__rt_ffs()：位图中最低置位 bit，用于快速选最高 READY 优先级
rt_show_version()：启动诊断信息
console device：设备模型承接日志输出
rt_hw_console_output()：无设备时由 BSP 覆盖的弱回退接口
rt_kprintf()：静态缓冲区格式化，再输出
THREADSAFE_PRINTF：分别保护格式化缓冲区与控制台所有权
backtrace：通用内核循环回溯，CPU/BSP 负责栈帧还原
rt_malloc 家族：统一 API；实际算法由 mem / memheap / slab 提供
heap lock：保护同一系统堆的并发访问
rt_malloc_align：隐藏保存原始地址，必须配 rt_free_align
RT_ASSERT：打印、回溯、停机或交给断言 Hook
```
