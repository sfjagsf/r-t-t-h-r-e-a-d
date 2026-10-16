# 25. 传统串口框架实现：打开、读写与中断/DMA

> 对应源码：`components/drivers/serial/dev_serial.c`。
>
> 传统串口框架的当前实现文件是 `dev_serial.c`；`components/drivers/serial/` 下没有
> `serial.c`。本文件将上一节 `dev_serial.h` 定义的对象、FIFO、DMA 状态和硬件操作表，
> 接入通用 `rt_device` 框架。

承接 [第 24 节](24-传统串口驱动接口-对象配置与硬件操作表.md)。本章按“注册 → 打开
资源 → 收发 → 中断推进 → 关闭”的顺序阅读。下文箭头分支均为流程示意。

## 1. 分层与职责

```text
应用
  │ rt_device_open/read/write/control
  ▼
通用设备框架（components/drivers/core/device.c）
  ▼
传统串口框架（components/drivers/serial/dev_serial.c）
  │ rt_uart_ops
  ▼
BSP UART 驱动（drv_uart.c / drv_usart.c）
  ▼
UART 硬件、中断与 DMA
```

`dev_serial.c` 不直接操作某种芯片的寄存器。它负责通用串口状态、FIFO、DMA 队列和
异步通知；BSP 经 `rt_uart_ops` 提供 `configure`、`control`、`putc`、`getc` 和
`dma_transmit` 等硬件相关实现。

设备框架和串口框架各有一张操作表：

| 操作表 | 调用者 | 被调用者 | 作用 |
| --- | --- | --- | --- |
| `rt_device_ops` | 通用 `device.c` | 串口框架 | 统一 `init/open/read/write/control` |
| `rt_uart_ops` | 串口框架 | BSP UART 驱动 | 实际操作 UART 硬件 |

## 2. 串口通用设备回调

可先按下列函数名定位源码，再深入各模式的辅助函数：

| 阅读入口 | 重点观察 |
| --- | --- |
| `rt_hw_serial_register` | `parent.ops` 与 `serial->ops` 如何分工 |
| `rt_serial_open/close` | `serial_rx/serial_tx` 在何处创建和释放 |
| `rt_serial_read/write` | 哪些 `open_flag` 位选择哪条路径 |
| `rt_hw_serial_isr` | 事件如何推进 FIFO、completion 与 DMA 队列 |
| `rt_serial_control` | 哪些命令在框架处理，哪些透传 BSP |
| `serial_fops_read` | 文件读取如何等待接收通知 |

文件实现六个核心回调：

```c
rt_serial_init();
rt_serial_open();
rt_serial_close();
rt_serial_read();
rt_serial_write();
rt_serial_control();
```

启用 `RT_USING_DEVICE_OPS` 时，它们组成：

```c
static const struct rt_device_ops serial_ops =
{
    rt_serial_init,
    rt_serial_open,
    rt_serial_close,
    rt_serial_read,
    rt_serial_write,
    rt_serial_control
};
```

因此调用链为：

```text
rt_device_write()
→ device.c
→ serial_ops.write
→ rt_serial_write()
→ 轮询 / 中断 / DMA 发送路径
→ BSP 的 putc 或 dma_transmit
```

未启用 `RT_USING_DEVICE_OPS` 时，注册函数把同一批函数填入旧式的
`device->init/read/write/...` 成员，行为不变。

## 3. 注册：`rt_hw_serial_register()`

```c
rt_err_t rt_hw_serial_register(struct rt_serial_device *serial,
                               const char *name,
                               rt_uint32_t flag,
                               void *data);
```

BSP 通常这样调用：

```c
rt_hw_serial_register(&serial, "uart1",
                      RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX,
                      uart_private_data);
```

其执行顺序：

1. 初始化 `serial->spinlock`，用于保护线程与中断并发访问的状态；
2. 将 `parent.type` 设为 `RT_Device_Class_Char`；
3. 清空 `rx_indicate`、`tx_complete`；
4. 安装 `serial_ops`（或旧式六个设备回调）；
5. 将 BSP 私有数据保存至 `device->user_data`；
6. 调用 `rt_device_register()`，使设备可由 `rt_device_find("uart1")` 查找；
7. 启用 `RT_USING_POSIX_STDIO` 时安装 `_serial_fops`；启用 Smart 时注册 TTY。

## 4. 初始化：`rt_serial_init()`

```c
serial->serial_rx = RT_NULL;
serial->serial_tx = RT_NULL;
serial->ops->configure(serial, &serial->config);
```

初始化只清除 RX/TX 运行期状态，并调用 BSP 的 `configure()` 应用默认串口参数。它
通常配置时钟、GPIO 复用、波特率、数据位、停止位、校验、流控及 UART 外设本身。

需要区分：

```text
init：让 UART 硬件处于基础可用状态
open：按本次所选模式分配 FIFO/DMA 状态，并启动中断或 DMA
```

注册阶段只连接对象和回调，首次打开才按需初始化。串口最后一次关闭会清除
`ACTIVATED`，因此再次打开会再次执行本函数，并不是整个设备寿命只初始化一次。

## 5. 打开：`rt_serial_open()`

打开前会验证所请求的模式是否在设备注册能力中声明。例如请求 `INT_RX`，但设备
`flag` 没有 `RT_DEVICE_FLAG_INT_RX` 时，返回 `-RT_EIO`。

传统串口使用打开参数承载收发模式：

```c
rt_device_open(serial, RT_DEVICE_FLAG_INT_RX);
rt_device_open(serial, RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_INT_TX);
rt_device_open(serial, RT_DEVICE_FLAG_DMA_RX | RT_DEVICE_FLAG_DMA_TX);
```

虽然参数名称是 `oflag`，传统串口框架额外使用其中的 `INT_RX`、`DMA_RX` 等位指定
本次工作模式。

每个方向应只选择一种模式。源码在同方向上先判断 INT，再以 `else if` 判断 DMA，
同时设置二者不会组合出一种“中断加 DMA”模式。已经创建的 `serial_rx/serial_tx`
也不会因为重复 `open()` 自动更换对象布局；切换模式应在协调使用者并完整关闭后进行。

### 5.1 轮询模式

未请求中断或 DMA 时，不分配额外对象：`read()` 不断调用 `getc()`，`write()` 不断
调用 `putc()`。实现简单，但需要 CPU 主动查询硬件。

### 5.2 中断接收

请求 `RT_DEVICE_FLAG_INT_RX` 时，框架分配：

```text
struct rt_serial_rx_fifo
+ config.bufsz 字节的软件环形缓冲区
```

随后调用：

```c
control(serial, RT_DEVICE_CTRL_SET_INT, RT_DEVICE_FLAG_INT_RX);
```

由 BSP 开启 UART RX 中断。以后 IRQ 中的数据由 `rt_hw_serial_isr()` 写入该 FIFO。
中断接收要求 `bufsz > 0`，不能将 DMA 无 FIFO 模式的零长度配置套到此路径。

### 5.3 中断发送

请求 `RT_DEVICE_FLAG_INT_TX` 时，框架创建包含 `rt_completion` 的
`rt_serial_tx_fifo`，然后让 BSP 开启 TX 中断。发送寄存器暂不可写时，写线程等待
completion；TX 完成事件到来时被唤醒。

### 5.4 DMA 接收与发送

DMA 代码受 `RT_SERIAL_USING_DMA` 控制。

| 模式 | 行为 |
| --- | --- |
| DMA RX 且 `bufsz == 0` | 一次 `read()` 发起一次 DMA 接收，完成后通知上层 |
| DMA RX 且 `bufsz != 0` | DMA 将数据写入环形缓冲区，应用按需读取 |
| DMA TX | 待发送数据进入 `rt_data_queue`，由 DMA 逐项发送 |

DMA TX 的 `activated` 防止多个调用者同时启动同一通道：首项启动 DMA，后续项只排队；
DMA 完成后再由 ISR 启动下一项。

当前 TX 队列由 `rt_data_queue_init(..., 8, 4, ...)` 建立，表示 8 个数据项及相应低水位，
不是 8 字节缓存。队列满时 `push(..., RT_WAITING_FOREVER)` 可能阻塞，因此 DMA TX
不能简单当成“任何上下文都可立即返回”的函数。

打开流程对若干内存分配使用断言，而且没有检查每次 BSP `control()` 和队列初始化的
返回值。`rt_device_open()` 返回成功不等于所有 DMA 硬件配置均已得到完整的错误验证；
排查失败时应同时查看 BSP 日志、资源配置与运行状态。

## 6. 关闭：`rt_serial_close()`

关闭时，框架按实际模式释放资源：

```text
INT RX → control(CLR_INT, INT_RX) → 释放 RX FIFO
DMA RX → control(CLR_INT, DMA_RX) → 释放 DMA RX 状态或 FIFO
INT TX → control(CLR_INT, INT_TX) → 释放 TX completion
DMA TX → control(CLR_INT, DMA_TX) → 销毁 data_queue 并释放 DMA TX 状态
最后   → control(CLOSE) → 清除 ACTIVATED
```

通用设备框架只在最后一个使用者关闭时才进入串口 `close`，因此共享设备不会被某个
使用者提前释放其 FIFO 或硬件活动状态。

这只保护配对的打开引用，不保证所有读写线程已经退出，也不负责等待 DMA TX 队列全部
发送完。关闭前应停止新提交并按应用需求等待完成；源码还忽略若干底层关闭返回值，
不能把其返回 `RT_EOK` 当成硬件已成功停止的独立证明。

## 7. 读取：`rt_serial_read()`

```text
INT_RX → _serial_int_rx();
DMA_RX → _serial_dma_rx();
其他   → _serial_poll_rx();
```

### 7.1 轮询 `_serial_poll_rx()`

循环调用 `serial->ops->getc()`：返回 `-1` 表示暂时无数据；返回字符则写入用户缓冲。
流模式下读到 `\n` 会提前结束。

### 7.2 中断 FIFO `_serial_int_rx()`

从 `buffer[get_index]` 取数据，更新并环绕 `get_index`。读取时用
`rt_spin_lock_irqsave()` 保护，因为 UART ISR 可能同时更新 `put_index` 和 `is_full`。

### 7.3 DMA `_serial_dma_rx()`

有 FIFO 时，函数计算可读长度，必要时跨环形缓冲区末端进行两次拷贝，再更新
`get_index`。无 FIFO 时，如当前未发起 DMA RX，调用：

```c
dma_transmit(serial, data, length, RT_SERIAL_DMA_RX);
```

若已有 RX DMA 在运行，则设置 `errno = -RT_EBUSY` 并返回 0。

无 FIFO 分支在提交后返回请求长度，表示尝试发起接收，并不表示数据已到达；且当前
实现未检查 `dma_transmit()` 的返回值。调用者应保持目标内存有效，等完成通知后再处理
数据。是否支持这种模型取决于 BSP：第 26 节的 STM32 驱动只在 `dma_transmit` 中处理
TX，RX 是打开时启动到框架 FIFO 的，因此不能直接使用此无 FIFO RX 分支。

## 8. 写入：`rt_serial_write()`

```text
INT_TX → _serial_int_tx();
DMA_TX → _serial_dma_tx();
其他   → _serial_poll_tx();
```

轮询发送逐字调用 `putc()`；中断发送中 `putc()` 暂不可写时，调用
`rt_completion_wait()` 等待 TX 完成事件；DMA 发送把缓冲区指针和长度压入
`data_queue`，首项立即启动，后续项等待前项 DMA 完成。

DMA TX 队列只保存原缓冲区地址与长度，未复制内容。返回请求长度表示已入队，调用者
必须等对应 `tx_complete` 后才修改或释放缓冲区；函数返回后栈上临时数组失效、循环中
立即重写同一缓冲区，都会破坏尚未完成的发送。

`activated` 只协调 DMA 活动状态，不是整个设备的多线程发送互斥锁。多线程输出是否
需要独立队列或锁，应由应用明确协调；不能据此推断整段协议数据一定不交错。

当设备以 `RT_DEVICE_FLAG_STREAM` 打开时，轮询和中断发送路径会自动将：

```text
\n → \r\n
```

这是串口控制台的终端兼容行为；DMA TX 路径直接提交原缓冲区，不执行此转换。已经
带有 `\r\n` 的文本在流模式下还可能再插入一个 `\r`，二进制协议不宜启用流模式。

## 9. 中断桥梁：`rt_hw_serial_isr()`

BSP 的 UART ISR 负责识别硬件事件，再调用：

```c
rt_hw_serial_isr(serial, RT_SERIAL_EVENT_RX_IND);
rt_hw_serial_isr(serial, RT_SERIAL_EVENT_TX_DONE);
rt_hw_serial_isr(serial, RT_SERIAL_EVENT_RX_DMADONE | (length << 8));
rt_hw_serial_isr(serial, RT_SERIAL_EVENT_TX_DMADONE);
```

低 8 位是事件类型；DMA RX 实际长度放在高位。

### 9.1 `RX_IND`

框架循环调用 BSP `getc()`，将每个字符写进 RX FIFO，然后通知上层：

```text
UART IRQ → getc → RX FIFO → rx_indicate(dev, unread_length)
```

这里应逐句理解当前源码，而不是假设它实现了理想的“丢最旧、留最新”环形队列：

```text
写入 buffer[put_index]
→ put_index 前移并环绕
→ 若 put_index == get_index，设置 is_full，并尝试输出一次告警
```

满后继续写确实会覆盖未读内容，但该分支没有在每次覆盖时同步推进读索引，不能保证
持续溢出后的数据仍按“最近 N 字节”的时序排列。RX 通知的长度计算还没有纳入
`is_full`：恰好满时两索引相等会算出 0，从而跳过该次回调。读路径对满标志的判断与
通知路径的长度计算存在区别，学习时必须分开看。

例如 115200、8N1 约每秒接收 11520 字节，64 字节只容纳约 5.6 ms 的连续输入。增大
缓存可缓冲短时调度延迟，仍需让接收线程及时排空，协议层也应能发现丢帧。

### 9.2 `TX_DONE`

调用 `rt_completion_done()`，唤醒正等待中断发送完成的写线程。

它不调用应用的 `tx_complete`；当前实现是在 DMA TX 完成分支调用该回调。不能在普通
轮询/中断 TX 场景中只安装 `tx_complete` 就假定一定会收到通知。

### 9.3 `RX_DMADONE`

无 FIFO 时直接以 DMA 完成长度调用 `rx_indicate`；有 FIFO 时先更新 `put_index`，再
计算总未读长度并通知上层。

### 9.4 `TX_DMADONE`

移除已完成的队列项；若存在下一项，立即启动下一笔 DMA，否则清除 `activated`；最后
调用 `tx_complete(dev, completed_buffer)`，告知上层缓冲区可复用。

## 10. 控制：`rt_serial_control()`

核心通用命令：

| 命令 | 行为 |
| --- | --- |
| `RT_DEVICE_CTRL_SUSPEND` | 设置挂起标志 |
| `RT_DEVICE_CTRL_RESUME` | 清除挂起标志 |
| `RT_DEVICE_CTRL_CONFIG` | 修改 `serial_configure` |
| `RT_DEVICE_CTRL_NOTIFY_SET` | 保存 `rx_notify` |
| `RT_DEVICE_CTRL_CONSOLE_OFLAG` | 返回控制台推荐打开方式 |

`SUSPEND/RESUME` 在本函数中只修改标志，没有调用硬件停止操作；串口读写分发也没有
据此统一阻断访问。`NOTIFY_SET` 只是复制 `rx_notify`，本文件未调用其 `notify` 成员；
日常接收通知仍应追踪 `parent.rx_indicate`。

配置保护：

```text
baud_rate == 0 → -RT_EINVAL
设备已打开且 bufsz 改变 → -RT_EBUSY
```

`bufsz` 决定运行中 RX FIFO 的内存布局，不能在已打开时更改。设备已打开时，先调用
BSP `configure()`；成功后再更新 `serial->config`。未打开时只保存配置，留待下次
初始化生效。

启用 POSIX 支持后，函数还处理 `TCGETS/TCSETS`、`TCFLSH`、`FIONREAD` 以及终端窗口
大小等 termios/ioctl 命令。

## 11. POSIX 文件接口（可选）

`RT_USING_POSIX_STDIO` 下，`_serial_fops` 把文件接口映射至设备接口：

```text
open  → rt_device_open
close → rt_device_close
read  → rt_device_read
write → rt_device_write
ioctl → rt_device_control
poll  → 观察 RX FIFO 与等待队列
```

阻塞读取流程：

```text
POSIX read
→ rt_device_read，暂时无数据
→ 非阻塞：内部暂设 -EAGAIN，再在函数末尾转换为 0 返回
→ 阻塞：在 device->wait_queue 等待
→ UART RX IRQ
→ serial_fops_rx_ind 唤醒 POLLIN 等待者
→ read 再次尝试读取
```

这里描述的是当前 `serial_fops_read()` 的实际行为：它把所有负 `size` 归零，所以
不能照搬 POSIX 常见的“直接返回 `-EAGAIN`”结论。`poll` 在本实现中只检查 RX FIFO
并报告 `POLLIN`，不应推导出它也统一实现了发送就绪事件。

启用 `RT_USING_LWP` 时，ioctl 适配还会核对用户地址并复制支持的参数，避免把用户空间
地址直接当作内核结构体使用；此部分与 MCU 直接调用 `rt_device_control()` 的路径不同。

## 12. 本章小结

1. `dev_serial.c` 是传统串口框架的当前实现文件；
2. `rt_hw_serial_register()` 将串口对象注册为通用字符设备；
3. `open()` 按轮询、中断或 DMA 模式创建并启动相应资源；
4. `read/write()` 依据 `open_flag` 分流至对应收发路径；
5. `rt_hw_serial_isr()` 是 BSP IRQ/DMA 事件进入通用串口框架的桥梁；
6. `control()` 管理串口参数，并可选地提供 POSIX termios/ioctl 兼容；
7. BSP 驱动下一步要实现的重点是 `rt_uart_ops` 与对 `rt_hw_serial_isr()` 的调用。

继续阅读 [第 26 节：STM32 UART 与 HAL 对接](26-STM32传统UART驱动-RTThread框架与HAL对接.md)，
把通用框架提供的模式与 STM32 BSP 真正实现的硬件能力逐项对照。
