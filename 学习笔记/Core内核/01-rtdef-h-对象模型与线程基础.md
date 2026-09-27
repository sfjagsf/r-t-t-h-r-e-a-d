# RT-Thread `rtdef.h` 学习笔记

## 文件定位

`include/rtdef.h` 是 RT-Thread 内核的基础定义文件。它的重点不是调度算法，而是为线程、IPC、定时器、设备等组件提供统一的对象模型、接口形式和可裁剪的数据结构。

## 一、配置宏：可选扩展字段

```c
struct rt_object
{
    ...
#ifdef RT_USING_MODULE
    void *module_id;
#endif
#ifdef RT_USING_SMART
    rt_atomic_t lwp_ref_count;
#endif
};
```

- `#ifdef` 判断的是宏是否**被定义**，不是它是否等于 `1`。即使 `#define X 0`，`#ifdef X` 仍成立。
- `RT_USING_MODULE`：动态模块支持；`module_id` 记录对象所属模块。
- `RT_USING_SMART`：RT-Thread Smart/LWP 支持；`lwp_ref_count` 是引用计数。
- 宏未启用时，对应字段在预处理阶段完全消失，不占 RAM。所有参与同一固件编译的源文件必须使用同一份配置，否则结构体布局会不一致。

## 二、统一对象模型

下面的图表示**嵌入方向**：外层的具体对象包含其 `parent` 成员；箭头从“子对象”指向“它所包含的父对象”。

```text
rt_thread       ─┐
rt_timer        ─┤
rt_device       ─┤── 直接包含 struct rt_object parent
rt_memory       ─┤
rt_memheap      ─┤
rt_mempool      ─┘

rt_semaphore    ─┐
rt_mutex        ─┤
rt_event        ─┤── 包含 struct rt_ipc_object parent
rt_mailbox      ─┤             └── 其内部再包含 struct rt_object parent
rt_messagequeue ─┘
```

RT-Thread 用“**子结构体的第一个成员嵌入父结构体**”模拟 C 中的继承：

```c
struct rt_semaphore
{
    struct rt_ipc_object parent;
    rt_uint16_t value;
};

struct rt_ipc_object
{
    struct rt_object parent;
    rt_list_t suspend_thread;
};
```

因此信号量首地址、其 `rt_ipc_object` 父部首地址和其 `rt_object` 父部首地址相同。`rt_object` 自己只是一段公共头，并不包含或知道任何具体子对象；内核是通过这个公共头把各种实体统一当作 `rt_object` 管理，再依据 `type` 按类别转换为具体对象。

## 三、`struct rt_object`：所有内核对象的公共头

```c
struct rt_object
{
    char       name[RT_NAME_MAX];
    rt_uint8_t type;
    rt_uint8_t flag;
    rt_list_t  list;
};
```

- `name`：对象名，如 `"worker"`、`"uart1"`。
- `type`：对象类别。
- `flag`：通用附加属性。
- `list`：该对象挂入所属类别总链表时使用的侵入式链表节点。

### 对象类别与对象状态不是同一概念

`enum rt_object_class_type` 回答“我是什么对象”：

```text
Thread、Semaphore、Mutex、Event、MailBox、MessageQueue、
MemHeap、MemPool、Device、Timer、Module、Memory ...
```

`RT_Object_Class_Static = 0x80` 是属性位，不是新的业务对象类别：

```text
静态线程 type = Thread | Static = 0x01 | 0x80 = 0x81
```

线程运行状态则保存在 `struct rt_thread` 的调度状态字段中，回答“这个线程现在在做什么”。

## 四、`struct rt_object_information`：每一类对象的登记册

```c
struct rt_object_information
{
    enum rt_object_class_type type;
    rt_list_t                 object_list;
    rt_size_t                 object_size;
    struct rt_spinlock        spinlock;
};
```

它不是一个具体线程/信号量，而是一个类别的管理信息：

```text
Thread 类登记册
├─ type        = Thread
├─ object_size = sizeof(struct rt_thread)
└─ object_list = 所有线程对象
```

- `object_list`：该类别所有已登记对象的双向循环链表。
- `object_size`：动态创建该类别对象时要申请的完整大小。
- `spinlock`：保护该类别对象链表，尤其用于 SMP 并发访问。

## 五、IPC 的共同抽象

```c
struct rt_ipc_object
{
    struct rt_object parent;
    rt_list_t suspend_thread;
};
```

信号量、互斥锁、事件、邮箱、消息队列的特有数据不同，但它们都有“资源不可用时，线程挂到等待链表”的共同机制。

```text
对象          特有状态                 共同点
信号量        value                    suspend_thread
互斥锁        owner / hold             suspend_thread
事件          set 位图                 suspend_thread
邮箱          环形缓冲区索引            suspend_thread
消息队列      消息块队列                suspend_thread
```

## 六、设备接口：C 的函数指针接口表

这一节是“对象、接口、回调”三者最典型的连接处。`struct rt_device` 是一个具体**对象**，其中的函数指针（或 `ops` 指针）是这个对象绑定的**驱动实现**；`rt_device_read()` 等则是提供给上层调用的统一**接口函数**。

```c
struct rt_device_ops
{
    rt_err_t   (*init)(rt_device_t dev);
    rt_err_t   (*open)(rt_device_t dev, rt_uint16_t oflag);
    rt_err_t   (*close)(rt_device_t dev);
    rt_ssize_t (*read)(rt_device_t dev, rt_off_t pos, void *buffer, rt_size_t size);
    rt_ssize_t (*write)(rt_device_t dev, rt_off_t pos, const void *buffer, rt_size_t size);
    rt_err_t   (*control)(rt_device_t dev, int cmd, void *args);
};
```

### 1. 函数指针表里实际存的是什么

例如某 UART 驱动会自己实现：

```c
static rt_ssize_t uart_read(rt_device_t dev, rt_off_t pos,
                            void *buffer, rt_size_t size)
{
    /* UART 专属的读数据逻辑 */
}

static const struct rt_device_ops uart_ops =
{
    .read = uart_read,
    /* .open / .close / .control ... */
};
```

然后它的设备对象保存：

```c
uart_dev.parent.ops = &uart_ops;
```

因此 `ops->read` 保存的不是数据，而是 `uart_read` 这段代码的函数地址。SPI Flash、ADC、网卡等设备可以各自放入不同的 `ops` 表，但表中槽位名称和参数形式一致。

### 2. 上层调用路径

上层并不直接写 `uart_read()`，而是传入一个 `rt_device_t`：

```text
rt_device_read(dev, ...)
    ↓
设备框架检查 dev 非空、类型为 Device、已经 open（ref_count 非 0）
    ↓
取得 dev->ops->read（旧配置中直接取 dev->read）
    ↓
调用该函数指针：read(dev, ...)
    ↓
UART / SPI Flash / 传感器等具体驱动
```

所以同一个 `rt_device_read()` 接口，面对 UART 对象时会跳到 UART 驱动，面对 Flash 对象时会跳到 Flash 驱动。这是 C 语言中接近“多态”的写法。

### 3. `RT_USING_DEVICE_OPS` 只改变函数指针的存放位置

```text
开启 RT_USING_DEVICE_OPS：rt_device 内有 ops 指针，指向独立 rt_device_ops 表
未开启 RT_USING_DEVICE_OPS：init/open/read/... 函数指针直接放在 rt_device 内
```

两种布局的调用意义相同。前者便于多个同类设备对象共享一张只读 `ops` 表，通常更利于组织驱动；后者是旧式的“函数指针直接嵌入对象”。

不要把它和线程、定时器的回调混为一谈：`thread->entry` 与 `timer->timeout_func` 是“对象发生时要执行的用户动作”；设备 `ops` 是“设备框架调用设备驱动时所选择的实现”。信号量、互斥锁等 IPC 对象通常不需要这种每对象的操作表。

## 七、组件初始化导出

这一机制解决的问题是：驱动、文件系统、网络协议栈等模块各自声明“我在何时初始化”，而不必所有模块都去修改同一个巨大的启动函数。

### 1. 宏不是立即调用函数

例如：

```c
static int uart_board_init(void)
{
    /* 配置 UART 引脚、时钟或注册设备 */
    return 0;
}
INIT_BOARD_EXPORT(uart_board_init);
```

这行宏在编译后大意是生成一个静态变量：

```text
__rt_init_uart_board_init = uart_board_init
```

并通过编译器属性把这个“函数指针变量”放进类似 `.rti_fn.1` 的专用链接段。`rt_used` 属性防止链接器因“看起来没人引用该变量”而把它删除。

它不是注册一个 `rt_object`，也不是在写到这一行时调用 `uart_board_init()`；它只是把函数地址放到最终固件镜像中的指定区域。

### 2. 链接脚本把同阶段函数地址排在一起

链接器会把来自不同 `.c` 文件、但段名相同或排序相邻的函数指针连续放置。例如：

```text
.rti_fn.1       uart_board_init 的指针
.rti_fn.1       gpio_board_init 的指针
.rti_fn.1.0     CPU/内存/中断控制器初始化函数的指针
.rti_fn.3       各设备初始化函数的指针
.rti_fn.4       文件系统、网络等组件初始化函数的指针
```

`components.c` 还放置了 `rti_start`、`rti_board_start`、`rti_board_end`、`rti_end` 等边界标记。它们让程序知道“某一阶段函数指针数组从哪里开始、到哪里结束”。

### 3. 启动时怎样真正执行

启动阶段调用 `rt_components_board_init()` 时，代码本质是：

```c
for (fn_ptr = &__rt_init_rti_board_start;
     fn_ptr < &__rt_init_rti_board_end;
     fn_ptr++)
{
    (*fn_ptr)();
}
```

也就是说，程序遍历链接段中的函数地址，再逐个间接调用。随后 `main` 线程中的 `rt_components_init()` 遍历其余阶段。

常见顺序可概括为：

```text
board
→ core / subsys / platform
→ prev
→ device
→ component
→ env
→ app
→ （SMP 次核的 secondary_cpu 阶段）
```

实际可用阶段由 RT-Thread 版本、链接脚本和宏配置决定；阅读时以 `rtdef.h` 中的 `INIT_xxx_EXPORT` 定义和 `components.c` 的两个遍历边界为准。

### 4. 初始化宏与 `RT_USING_xxx` 的区别

```text
RT_USING_SEMAPHORE / RT_USING_DEVICE / RT_USING_HEAP
    → 决定某组件的代码和数据结构是否参与编译。

INIT_DEVICE_EXPORT(fn) / INIT_COMPONENT_EXPORT(fn)
    → 组件已编译进来后，决定其初始化函数被登记到哪个启动阶段。
```

例如关闭 `RT_USING_DEVICE` 时，设备框架源码一般不会编译进固件；即使某文件写了 `INIT_DEVICE_EXPORT()`，也要先有该文件及其宏分支被编译，函数地址才会存在。反过来，组件已编译但没有初始化导出或没有被显式调用，也可能没有完成运行时初始化。

因此，这套机制属于“启动时函数组织方式”，不是“组件开关本身”。

## 八、定时器的两个 Tick 字段

```c
struct rt_timer
{
    rt_tick_t init_tick;
    rt_tick_t timeout_tick;
};
```

- `init_tick`：相对时长/周期，如“等待 100 Tick”。
- `timeout_tick`：本次绝对到期 Tick，如“在 Tick 2350 到期”。

它们不是二选一的两种 API；启动时通常由公共系统 Tick 计算：

```c
timer->timeout_tick = rt_tick_get() + timer->init_tick;
```

每个定时器拥有自己的 `timeout_tick`，但都参照同一条系统 Tick 时间线。

## 九、线程状态、控制命令和 CPU 对象

`rt_object.type` 是对象类别；线程的运行状态在 `rt_thread` 调度上下文中。

```text
低 3 位：INIT / CLOSE / READY / RUNNING / SUSPEND 状态
bit 3：  YIELD 标志
高 4 位：信号处理状态
```

`RT_THREAD_CTRL_xxx` 不是线程状态，而是传给 `rt_thread_control()` 的命令号，如启动、关闭、改优先级、绑定 CPU。

`struct rt_cpu` 表示一个 CPU 核的内核调度信息：

```text
rt_cpu
├─ current_thread：当前正在本核运行的线程
├─ idle_thread：无就绪线程时运行的空闲线程
├─ priority_table：按优先级组织的就绪线程链表（SMP）
├─ irq_nest：中断嵌套层数
└─ cpu_stat：CPU 时间统计（可选）
```

`RT_USING_SMP` 表示同一套 RT-Thread 内核在多个 CPU 核上同时运行，不是每个核各运行一套互相独立的 RT-Thread。

## 十、线程控制块 `struct rt_thread`

```text
rt_thread
├─ parent：统一对象头，type 为 Thread
├─ sp / stack_addr / stack_size：栈与当前栈指针
├─ entry / parameter：线程入口函数及参数
├─ 调度器私有上下文
├─ thread_timer：线程自身的延时/等待超时定时器
├─ cleanup：退出清理回调
├─ IPC、信号、Smart、统计等可选字段
└─ user_data：用户私有数据
```

`thread_timer` 用于 `rt_thread_delay()` 与 IPC 超时等待：资源先到则停止定时器；定时器先到则唤醒线程并记录超时错误。

## 十一、源码锚点：结构体嵌入不是抽象图，而是首地址布局

来源：`include/rtdef.h`：

```c
struct rt_ipc_object
{
    struct rt_object parent;
    rt_list_t suspend_thread;
};

struct rt_semaphore
{
    struct rt_ipc_object parent;
    rt_uint16_t value;
    rt_uint16_t max_value;
    struct rt_spinlock spinlock;
};
```

`rt_semaphore` 的起始地址就是其 `rt_ipc_object parent` 的地址，后者起始地址又是 `rt_object parent` 的地址。调用 `&sem->parent.parent` 才是从具体信号量明确取得公共对象头；并不是 `rt_object` 内部保存 `rt_semaphore`。
