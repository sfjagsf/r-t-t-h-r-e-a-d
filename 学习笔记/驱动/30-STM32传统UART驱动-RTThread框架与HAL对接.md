# 30. STM32 传统 UART 驱动：RT-Thread 框架与 HAL 对接

> 对应源码：
>
> - `bsp/stm32/libraries/HAL_Drivers/drivers/drv_usart.c`
> - `bsp/stm32/libraries/HAL_Drivers/drivers/drv_usart.h`
>
> 本章选用 STM32 BSP 共用 UART 驱动作为具体实例。它将第 29 节传统串口框架的
> `rt_uart_ops` 落实为 STM32 HAL、NVIC 中断、UART 寄存器与 DMA 操作。

## 1. 从通用 API 到 STM32 硬件的完整链路

```text
应用
  │ rt_device_open/read/write/control
  ▼
通用设备框架（components/drivers/core/device.c）
  │ rt_device_ops
  ▼
传统串口框架（components/drivers/serial/dev_serial.c）
  │ rt_uart_ops
  ▼
STM32 UART 驱动（bsp/stm32/.../drv_usart.c）
  │ STM32 HAL、NVIC、DMA、UART 寄存器
  ▼
USART/UART/LPUART 硬件
```

前两层处理设备注册、引用计数、FIFO、DMA 队列和事件通知；本文件只处理 STM32 特有
内容：HAL 句柄、寄存器名差异、IRQ、DMA 通道和 STM32 UART 错误标志。

两张操作表的职责应分清：

| 操作表 | 填写者 | 调用者 | 用途 |
| --- | --- | --- | --- |
| `rt_device_ops` | 传统串口框架 | 通用设备框架 | `init/open/read/write/control` |
| `rt_uart_ops` | STM32 UART 驱动 | 传统串口框架 | 配置、硬件收发、DMA |

STM32 驱动提供的操作表是：

```c
static const struct rt_uart_ops stm32_uart_ops =
{
    .configure = stm32_configure,
    .control = stm32_control,
    .putc = stm32_putc,
    .getc = stm32_getc,
    .dma_transmit = stm32_dma_transmit
};
```

## 2. STM32 UART 对象与配置对象

头文件定义：

```c
struct stm32_uart_config
{
    const char *name;
    USART_TypeDef *Instance;
    IRQn_Type irq_type;
    const struct stm32_dma_config *dma_rx;
    const struct stm32_dma_config *dma_tx;
};
```

这是**编译期硬件资源描述**：设备名、外设实例、IRQ 编号及可选 DMA 配置均由 BSP 宏
生成。

```c
struct stm32_uart
{
    UART_HandleTypeDef handle;
    struct stm32_uart_config *config;
    rt_uint32_t DR_mask;
    rt_uint32_t tx_block_timeout;
    /* 可选 RX/TX DMA 句柄和状态 */
    rt_uint16_t uart_dma_flag;
    struct rt_serial_device serial;
};
```

这是**运行期对象**。各成员含义：

| 成员 | 含义 |
| --- | --- |
| `handle` | STM32 HAL 使用的 UART 句柄 |
| `config` | 指向对应硬件资源描述 |
| `DR_mask` | 从接收寄存器取数据时的有效位掩码 |
| `tx_block_timeout` | `putc` 等待发送完成的最大循环次数 |
| `dma_rx` / `dma_tx` | 启用 DMA 时的 HAL DMA 句柄和 RX 计数 |
| `uart_dma_flag` | 当前 UART 实际支持的 DMA RX/TX 能力位 |
| `serial` | RT-Thread 传统串口对象 |

`handle` 放在结构体第一个成员有实际意义：HAL 回调给出 `UART_HandleTypeDef *` 后，
驱动可将其转换为 `struct stm32_uart *`，从而恢复完整的 RT-Thread 串口对象。

嵌入关系：

```text
stm32_uart
  └─ rt_serial_device serial
       └─ rt_device parent
```

## 3. 多 UART 的配置表和对象表

驱动根据 BSP 配置宏决定参与编译的外设：

```c
BSP_USING_UART1
BSP_USING_UART2
...
BSP_USING_UART8
BSP_USING_LPUART1
```

每个已启用 UART 在 `uart_config[]` 中形成一项，如 `UART1_CONFIG`。同时，
`uart_obj[]` 为每一项配置创建一个运行期 `stm32_uart` 对象。

```text
uart_config[i]
  → 名称、实例、IRQ、DMA 的静态资源定义

uart_obj[i]
  → HAL 句柄、DMA 状态、rt_serial_device 的运行期状态
```

源文件会在未启用任何 `BSP_USING_UARTx`/`BSP_USING_LPUART1` 时触发预处理错误，防止
错误地把 UART 驱动编入一个没有任何 UART 的 BSP。

## 4. `stm32_configure()`：框架参数映射到 HAL

串口框架传入的是：

```c
struct serial_configure *cfg
```

而 STM32 HAL 需要填写：

```c
uart->handle.Init
```

`stm32_configure()` 承担映射工作。

| RT-Thread 配置 | STM32 HAL 配置 |
| --- | --- |
| `cfg->baud_rate` | `Init.BaudRate` |
| `RT_SERIAL_FLOWCONTROL_NONE` | `UART_HWCONTROL_NONE` |
| `RT_SERIAL_FLOWCONTROL_CTSRTS` | `UART_HWCONTROL_RTS_CTS` |
| `STOP_BITS_1` | `UART_STOPBITS_1` |
| `STOP_BITS_2` | `UART_STOPBITS_2` |
| `PARITY_NONE` | `UART_PARITY_NONE` |
| `PARITY_ODD` | `UART_PARITY_ODD` |
| `PARITY_EVEN` | `UART_PARITY_EVEN` |

### 4.1 数据位和校验位的特殊映射

STM32 的 `WordLength` 包含校验位，因此：

```text
RT-Thread：8 个有效数据位 + 无校验
STM32 HAL：UART_WORDLENGTH_8B

RT-Thread：8 个有效数据位 + 奇/偶校验
STM32 HAL：UART_WORDLENGTH_9B
```

这避免校验位占用后使有效载荷错误地少一位。

### 4.2 过采样、DMA 计数和初始化

具备 `USART_CR1_OVER8` 的系列中，高波特率（大于 5 Mbit/s）使用
`UART_OVERSAMPLING_8`，其他情况使用 `UART_OVERSAMPLING_16`。

DMA 编译启用时，若设备尚未打开，`dma_rx.remaining_cnt` 被设为接收缓冲区长度，供
后续 DMA+IDLE 计算新增数据长度使用。

最后调用：

```c
HAL_UART_Init(&uart->handle);
```

成功后由 `stm32_uart_get_mask()` 计算 `DR_mask`，并设置发送超时。

## 5. `DR_mask`：为何读取寄存器后还要掩码

不同 WordLength 与校验组合下，接收寄存器中真正有效的数据位不同。例如：

```text
8 位、无校验 → 0x00FF
8 位、有校验 → 0x007F
9 位、无校验 → 0x01FF
9 位、有校验 → 0x00FF
```

`stm32_getc()` 读取 RDR 或 DR 后执行：

```c
ch = register_value & uart->DR_mask;
```

避免校验位或无效高位作为应用数据进入 RT-Thread RX FIFO。

## 6. `stm32_control()`：响应串口框架命令

### 6.1 开启中断

收到：

```c
RT_DEVICE_CTRL_SET_INT
```

驱动会：

```text
设置 UART IRQ 的 NVIC 优先级
→ 在 NVIC 启用 UART IRQ
→ 若方向为 INT_RX，开启 UART_IT_RXNE
```

`RXNE` 表示接收数据寄存器非空。之后每接收一个字节，UART 会触发中断。

### 6.2 关闭中断或 DMA

收到：

```c
RT_DEVICE_CTRL_CLR_INT
```

驱动先禁用 UART IRQ；若方向是 `INT_RX`，还禁用 `UART_IT_RXNE`。DMA 模式下会调用
`stm32_dma_deinit()`，拆除 RX 或 TX DMA 配置。

### 6.3 配置 DMA

收到：

```c
RT_DEVICE_CTRL_CONFIG
```

在 DMA 模式下转发至 `stm32_uart_dma_config()`，并以参数区分 DMA RX 和 DMA TX。

### 6.4 关闭硬件与私有超时控制

```text
RT_DEVICE_CTRL_CLOSE
→ HAL_UART_DeInit()

UART_CTRL_SET_BLOCK_TIMEOUT
→ 设置 putc 的非零等待超时
```

`UART_CTRL_SET_BLOCK_TIMEOUT` 是此驱动的私有命令，值为零时返回错误。

## 7. 基础收发：`stm32_putc()` 与 `stm32_getc()`

### 7.1 `stm32_putc()`

发送一个字符时：

```text
清除 TC 标志
→ 向 TDR（新系列）或 DR（旧系列）写入字符
→ 等待 TC 置位，或等待超时
→ 成功返回 1，超时返回 -1
```

因此该实现是带有限超时的阻塞单字节发送。`dev_serial.c` 的轮询路径直接使用它；
其他发送路径也以它作为最底层字符写出动作。

### 7.2 `stm32_getc()`

```text
RXNE 未置位 → 返回 -1，代表当前无数据
RXNE 已置位 → 读取 RDR（新系列）或 DR（旧系列）
               → 与 DR_mask 相与
               → 返回字符
```

传统串口框架的 `rt_hw_serial_isr()` 会反复调用它，直到返回 `-1`，以取空当前已到达的
硬件接收数据。

## 8. UART IRQ：从 STM32 硬件事件到框架事件

以 UART1 为例：

```c
void USART1_IRQHandler(void)
{
    rt_interrupt_enter();
    uart_isr(&uart_obj[UART1_INDEX].serial);
    rt_interrupt_leave();
}
```

`rt_interrupt_enter/leave()` 让内核正确识别 ISR 上下文；实际状态识别集中于
`uart_isr()`。

### 8.1 RXNE：中断接收

```text
RXNE 置位且 RXNE 中断已开启
→ rt_hw_serial_isr(serial, RT_SERIAL_EVENT_RX_IND)
→ dev_serial.c 调用 stm32_getc()
→ 字节进入 RT-Thread RX FIFO
→ rx_indicate() 通知应用
```

### 8.2 TC：普通发送完成

```text
TC 置位且 TC 中断已开启
→ DMA TX 模式：交 HAL_UART_IRQHandler() 继续处理
→ 非 DMA TX：关闭 TC 中断
              → rt_hw_serial_isr(serial, RT_SERIAL_EVENT_TX_DONE)
              → 唤醒等待发送的线程
→ 清除 TC 标志
```

### 8.3 错误标志

若没有上述数据事件，驱动检查并清除：

| 标志 | 含义 |
| --- | --- |
| `ORE` | 溢出错误 |
| `NE` | 噪声错误 |
| `FE` | 帧错误 |
| `PE` | 校验错误 |

还会按芯片系列处理 LIN、CTS、TXE、RXNE 等额外状态。及时清标志可避免 UART 卡在错误
中断状态，或不断重复进入 ISR。

## 9. DMA 发送

传统串口框架请求 DMA TX 时调用：

```c
stm32_dma_transmit(serial, buf, size, RT_SERIAL_DMA_TX);
```

驱动使用：

```c
HAL_UART_Transmit_DMA(&uart->handle, buf, size);
```

DMA 完成后，HAL 回调：

```c
HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
```

将 HAL 句柄转为 `stm32_uart`，再报告：

```c
rt_hw_serial_isr(&uart->serial, RT_SERIAL_EVENT_TX_DMADONE);
```

之后 `dev_serial.c` 负责取出已完成发送项、启动队列下一项，并调用上层
`tx_complete` 回调。因此 STM32 驱动负责“报告硬件完成”，串口框架负责“推进软件发送
队列”。

## 10. DMA 接收与 IDLE：不定长帧的关键机制

### 10.1 配置与启动

DMA RX 配置后，驱动：

```text
建立 UART RX 与 DMA 句柄关联
→ HAL_UART_Receive_DMA(handle, rx_fifo->buffer, config.bufsz)
→ 关闭错误中断位 EIE
→ 开启 UART IDLE 中断
```

DMA 直接写入传统串口框架分配的 RX FIFO 缓冲区。

### 10.2 三种通知来源

| 通知 | 含义 |
| --- | --- |
| IDLE | 一段时间没有新字节，常被当作一帧数据结束线索 |
| HT | DMA 缓冲区已传输一半 |
| TC | DMA 缓冲区已完整传输一轮 |

IDLE 在 `uart_isr()` 中识别；HT/TC 由 HAL DMA 回调识别。它们最终都进入：

```c
dma_recv_isr(serial, isr_flag);
```

### 10.3 如何计算新增数据长度

DMA 硬件的计数器表示“剩余待搬运数量”。驱动将当前计数与
`uart->dma_rx.remaining_cnt` 对比：

```text
计数变小：同一轮 DMA 中新增数据
计数变大：DMA 环形缓冲已回绕，需要加上 bufsz
```

得出 `recv_len` 后更新 `remaining_cnt`，并报告：

```c
rt_hw_serial_isr(serial,
                 RT_SERIAL_EVENT_RX_DMADONE | (recv_len << 8));
```

第 29 节的串口框架据此更新 FIFO 写索引并触发 `rx_indicate()`。这正是 STM32 常用
“DMA 环形接收 + IDLE 分帧”机制与 RT-Thread 框架的连接点。

## 11. 最终初始化：`rt_hw_usart_init()`

初始化函数首先扫描 BSP DMA 配置，给每个 UART 的 `uart_dma_flag` 加上：

```c
RT_DEVICE_FLAG_DMA_RX
RT_DEVICE_FLAG_DMA_TX
```

随后遍历 `uart_obj[]`：

```c
uart_obj[i].config = &uart_config[i];
uart_obj[i].serial.ops = &stm32_uart_ops;
uart_obj[i].serial.config = RT_SERIAL_CONFIG_DEFAULT;

rt_hw_serial_register(&uart_obj[i].serial,
                      uart_obj[i].config->name,
                      RT_DEVICE_FLAG_RDWR |
                      RT_DEVICE_FLAG_INT_RX |
                      RT_DEVICE_FLAG_INT_TX |
                      uart_obj[i].uart_dma_flag,
                      RT_NULL);
```

所以每个已启用 UART 都以字符设备注册，默认声明支持读写和 RX/TX 中断；若 BSP 配置了
DMA，则额外声明对应 DMA 能力。

应用最终只需：

```c
rt_device_t uart = rt_device_find("uart1");
rt_device_open(uart, RT_DEVICE_FLAG_INT_RX);
rt_device_write(uart, 0, data, length);
```

硬件细节会经设备框架、串口框架一路分发到本驱动。

## 12. 本章小结

1. `drv_usart.c` 用 `rt_uart_ops` 将传统串口框架对接到 STM32 HAL；
2. `stm32_uart_config` 描述硬件资源，`stm32_uart` 保存 HAL、DMA 与 RT-Thread 运行期状态；
3. `stm32_configure` 正确处理数据位和校验位到 STM32 WordLength 的映射；
4. `stm32_control` 管理 NVIC、RXNE 中断、DMA 配置与 UART 关闭；
5. `stm32_putc/getc` 处理 TDR/DR 与 RDR/DR 的系列差异，并使用 `DR_mask` 保留有效数据；
6. `uart_isr` 将 RXNE、TC、IDLE 和错误标志转为框架或 HAL 所需动作；
7. DMA RX 的 IDLE、HT、TC 事件经 `dma_recv_isr` 汇集为框架的 `RX_DMADONE` 事件；
8. `rt_hw_usart_init` 注册每个启用的 STM32 UART，使其可按 `uart1` 等名称被访问。

下一节可阅读 `bsp/stm32/libraries/HAL_Drivers/drivers/drv_dma.c`，理解本驱动调用的
`stm32_dma_setup()`、`stm32_dma_deinit()` 如何配置具体 DMA 控制器、通道与请求源。
