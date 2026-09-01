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

```text
rt_object
├─ rt_thread
├─ rt_timer
├─ rt_device
├─ rt_memory / rt_memheap / rt_mempool
└─ rt_ipc_object
   ├─ rt_semaphore
   ├─ rt_mutex
   ├─ rt_event
   ├─ rt_mailbox
   └─ rt_messagequeue
```

RT-Thread 用“第一个成员嵌入父结构体”模拟 C 中的继承：

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

因此信号量首地址、其 `rt_ipc_object` 父部首地址和其 `rt_object` 父部首地址相同。内核可先把各种实体统一当作 `rt_object` 管理，再按类别转换为具体对象。

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

`struct rt_device` 的 `ops` 指向具体设备实现。上层只依赖统一接口：

```text
rt_device_read(dev, ...)
    ↓
dev->ops->read(dev, ...)
    ↓
UART / SPI Flash / 传感器等具体驱动
```

## 七、组件初始化导出

`INIT_BOARD_EXPORT`、`INIT_DEVICE_EXPORT`、`INIT_COMPONENT_EXPORT` 等宏将初始化函数指针放入特定链接段。系统启动时按阶段遍历并调用：

```text
board → core/subsys/platform → device → component → env → app
```

组件只声明自己属于哪个初始化阶段，不必修改一个集中式的大型 `main.c`。

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

## 建议复习问题

1. `rt_object.type` 与线程的 `stat` 分别回答什么问题？
2. 为什么 `rt_ipc_object` 中需要 `suspend_thread`？
3. `struct rt_device_ops` 如何实现“上层不依赖具体驱动”？
4. 为什么 `timeout_tick` 是每个定时器私有、但又基于公共 Tick？
5. 为什么扩展对象必须把 `parent` 放在第一个成员？
