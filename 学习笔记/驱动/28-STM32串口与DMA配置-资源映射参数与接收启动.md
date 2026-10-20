# 28. STM32 串口与 DMA 配置：资源映射、参数与接收启动

> 对应源码：
>
> - `bsp/stm32/libraries/HAL_Drivers/drivers/drv_config.h`
> - `bsp/stm32/libraries/HAL_Drivers/drivers/config/f4/dma_config.h`
> - `bsp/stm32/libraries/HAL_Drivers/drivers/config/f4/uart_config.h`
> - `bsp/stm32/libraries/HAL_Drivers/drivers/drv_dma.h`
> - `bsp/stm32/libraries/HAL_Drivers/drivers/drv_usart.c`
> - `components/drivers/include/drivers/dev_serial.h`
> - `components/drivers/serial/dev_serial.c`
>
> 本章使用 STM32F4 UART1 RX DMA 作为具体配置例子，不表示读者的板子一定是 F4。
> 资源映射、GPIO 引脚、HAL 功能和编译条件必须以实际芯片及 BSP 为准。

承接 [第 27 节](27-STM32-DMA公共层-描述符初始化与资源关联.md)。上一节解释公共 DMA
初始化接口，本章回答这些接口所需参数从哪里来，并串起“宏 → 配置对象 → 注册 → 参数设置
→ 缓冲区 → 硬件接收”的完整路径。

## 1. 三类配置，不要混为一谈

| 配置类型 | 典型内容 | 主要位置 |
| --- | --- | --- |
| 硬件资源配置 | USART1、DMA Stream、Channel、IRQ、时钟位 | 系列 `uart_config.h` / `dma_config.h` |
| 通信与软件缓冲参数 | 波特率、数据位、停止位、校验、`bufsz` | `struct serial_configure` |
| 板级连接配置 | TX/RX 引脚、GPIO 复用、外设时钟 | 板级 MSP 等初始化代码 |

例如，“使用 USART1”“波特率 115200”“PA9/PA10 用作收发引脚”分别回答用哪个外设、
怎样通信、信号从哪里连接。这些配置共同起作用，设备注册成功不等于全部条件都已正确。

## 2. drv_config.h：选择当前系列的头文件

F4 对应分支为：

```c
#elif defined(SOC_SERIES_STM32F4)
#include "f4/dma_config.h"
#include "f4/uart_config.h"
/* 其他外设配置省略 */
```

这是预处理阶段选择，不是运行时检测芯片。F4 在这里先包含 DMA 映射，再包含 UART
配置，使 UART 初始化宏可以使用相应资源定义；公共初始化宏来自已包含的 `drv_dma.h`。
实际头文件搜索路径由构建配置提供。

工程的系列、实例和 DMA 选项应通过已有配置流程管理。BSP 的 `rtconfig.h` 应从 `.config`
通过 `scons --menuconfig` 或 `scons --pyconfig-silent` 等流程生成，不手工修改生成文件。

还要注意：`#ifdef X` / `defined(X)` 检查的是“是否定义”，不是“宏值是否非零”。
将某个选项写成 `#define X 0` 并不能使这样的条件自动关闭。

## 3. UART1_CONFIG：描述串口本身

F4 `uart_config.h` 中的定义为：

```c
#if defined(BSP_USING_UART1)
#ifndef UART1_CONFIG
#define UART1_CONFIG             \
    {                            \
        .name = "uart1",         \
        .Instance = USART1,      \
        .irq_type = USART1_IRQn, \
    }
#endif
/* UART1 DMA 配置省略 */
#endif
```

- `name`：设备框架中的名称，应用通过 `rt_device_find("uart1")` 查找；
- `Instance`：实际 USART1 外设寄存器实例；
- `irq_type`：USART1 自身的中断编号，不是 DMA 中断编号。

`#ifndef UART1_CONFIG` 表示在已有定义时保留已有配置，但已有定义必须在相应包含顺序中
提前可见，不能在宏已被使用后再定义并期待追溯修改。

### 3.1 宏不是变量，也不是一次初始化函数调用

驱动使用它初始化数组：

```c
static struct stm32_uart_config uart_config[] =
{
#ifdef BSP_USING_UART1
    UART1_CONFIG,
#endif
    /* 其他已启用实例省略 */
};
```

展开后，UART1 对应元素就是：

```c
{
    .name = "uart1",
    .Instance = USART1,
    .irq_type = USART1_IRQn,
}
```

这是带字段名的结构体初始化内容。反斜杠 `\` 只是让宏定义续到下一行，并不会导致运行时
逐行执行。真正存储这些值的是数组元素，不是宏名称本身。

## 4. dma_config.h：DMA 实例、Channel 与中断从哪里来

F4 文件按不同 Stream 组织候选配置。UART1 RX 在 DMA2 Stream2 组中有以下候选分支：

```c
#elif defined(BSP_UART1_RX_USING_DMA) && !defined(UART1_RX_DMA_INSTANCE)
#define UART1_DMA_RX_IRQHandler DMA2_Stream2_IRQHandler
#define UART1_RX_DMA_RCC        RCC_AHB1ENR_DMA2EN
#define UART1_RX_DMA_INSTANCE   DMA2_Stream2
#define UART1_RX_DMA_CHANNEL    DMA_CHANNEL_4
#define UART1_RX_DMA_IRQ        DMA2_Stream2_IRQn
```

各宏作用为：

| 宏 | 本候选分支中的值 | 含义 |
| --- | --- | --- |
| `UART1_RX_DMA_INSTANCE` | `DMA2_Stream2` | 实际搬运单元 |
| `UART1_RX_DMA_CHANNEL` | `DMA_CHANNEL_4` | 为该 Stream 选择的硬件请求映射 |
| `UART1_RX_DMA_RCC` | `RCC_AHB1ENR_DMA2EN` | DMA2 的时钟使能位 |
| `UART1_RX_DMA_IRQ` | `DMA2_Stream2_IRQn` | NVIC 使用的中断编号 |
| `UART1_DMA_RX_IRQHandler` | `DMA2_Stream2_IRQHandler` | 中断处理函数名的别名 |

### 4.1 Stream 和 Channel 不是同一个概念

在这里的 F4 模型中：

```text
DMA2           → DMA 控制器
Stream2        → 控制器内实际执行搬运的单元
Channel4       → 为该 Stream 选择的请求来源映射
```

Channel4 并不是 Stream2 之外又独立分配的一个搬运单元。两个外设即使选择不同 Channel，
也不能因此认为可以同时独占使用同一个 Stream。合法映射要符合具体芯片的硬件规定。

### 4.2 为什么不能断言 UART1 RX 一定使用 Stream2

这一段前面是 `#elif`，同组前面还有 ADC2、内存到内存等候选条件；文件后面也为 UART1 RX
提供了 DMA2 Stream5 候选分支。

`!defined(UART1_RX_DMA_INSTANCE)` 避免已确定实例后再次选择，配合按顺序排列的条件
形成编译期默认映射。它不是运行时检测“哪个 Stream 空闲”，也不是完备的冲突检查器。
多个外设同时启用或使用自定义映射时，仍须核对最终展开结果和资源冲突。

下文为了展示展开结果，假设最终选择了 Stream2；实际选择 Stream5 时应替换对应实例、
中断编号和函数入口，不能只改其中一个字段。

## 5. UART1_DMA_RX_CONFIG：组合成按字节循环接收的配置

F4 的 UART1 RX 配置宏是：

```c
#define UART1_DMA_RX_CONFIG                   \
    STM32_DMA_RX_BYTE_CIRCULAR_CONFIG_INIT_EX( \
        UART1_RX_DMA_INSTANCE,                \
        UART1_RX_DMA_RCC,                     \
        UART1_RX_DMA_IRQ,                     \
        UART1_RX_DMA_CHANNEL,                 \
        0U,                                  \
        UART1_RX_DMA_PRIORITY,                \
        UART1_RX_DMA_PREEMPT_PRIORITY,        \
        UART1_RX_DMA_SUB_PRIORITY)
```

将名字拆开：`RX` 表示接收，`BYTE` 表示按字节搬运，`CIRCULAR` 表示循环模式，
`CONFIG_INIT` 表示提供配置初始化内容。它不是一个名为 CONFIG_INIT 的运行时函数。

默认优先级是：

```c
#define UART1_RX_DMA_PRIORITY         DMA_PRIORITY_MEDIUM
#define UART1_RX_DMA_PREEMPT_PRIORITY 0
#define UART1_RX_DMA_SUB_PRIORITY     0
```

这些默认值由 `#ifndef` 保护，可以由提前可见的适配配置覆盖。DMA 仲裁优先级与 NVIC
优先级彼此独立；NVIC 抢占/子优先级的实际划分还受优先级分组和芯片实现影响。

参数 `0U` 对应公共宏接口中的 request 参数位置。F4 这里使用 Channel 选择，不定义
`STM32_DMA_USES_REQUEST`，相关字段初始化会被宏省略，不能解释成“选中了请求 0”。

## 6. 展开后的配置长什么样

以下为 Stream2 情况的关键字段示意，省略 FIFO/Burst 等默认字段，不是完整展开输出：

```c
{
    .common =
    {
        .Instance = DMA2_Stream2,
        .type = STM32_DMA_TYPE_DMA,
        .dma_rcc = RCC_AHB1ENR_DMA2EN,
        .dma_irq = DMA2_Stream2_IRQn,
        .priority = DMA_PRIORITY_MEDIUM,
        .preempt_priority = 0,
        .sub_priority = 0,
        .direction = DMA_PERIPH_TO_MEMORY,
        .mode = DMA_CIRCULAR,
    },
    .config.classic =
    {
        .periph_inc = DMA_PINC_DISABLE,
        .mem_inc = DMA_MINC_ENABLE,
        .periph_data_alignment = DMA_PDATAALIGN_BYTE,
        .mem_data_alignment = DMA_MDATAALIGN_BYTE,
        .channel = DMA_CHANNEL_4,
    },
}
```

### 6.1 为什么外设地址固定、内存地址递增

UART 接收时，DMA 反复读取同一个接收数据寄存器，而收到的数据要依次放入不同内存位置：

```text
UART 接收数据寄存器 → buffer[0]
UART 接收数据寄存器 → buffer[1]
UART 接收数据寄存器 → buffer[2]
...
```

所以外设地址不递增，内存地址递增。`BYTE` 表示每次访问宽度为字节，不表示总共只能接收
一个字节，也不表示缓冲区只能有一个字节。

### 6.2 循环模式不是无限缓冲区

完成指定数量的搬运后，循环模式重新从缓冲区起点继续。DMA 不会等待应用把旧数据读完，
应用处理太慢时仍会发生覆盖。硬件循环模式与框架的软件读写索引需要共同配合，详见
[第 25 节](25-传统串口框架实现-打开读写与中断DMA.md) 和
[第 26 节](26-STM32传统UART驱动-RTThread框架与HAL对接.md) 的接收事件分析。

当前 F4 UART TX 的字节初始化宏使用 `DMA_MEMORY_TO_PERIPH` 和 `DMA_NORMAL`，
不能把 RX 循环模式无条件推广到 TX 或全部其他系列。

## 7. 描述符何时成为真实对象

`stm32_uart_get_dma_config()` 中的 UART1 RX 代码为：

```c
uart_obj[UART1_INDEX].uart_dma_flag |= RT_DEVICE_FLAG_DMA_RX;

static const struct stm32_dma_config uart1_dma_rx = UART1_DMA_RX_CONFIG;

uart_config[UART1_INDEX].dma_rx = &uart1_dma_rx;
```

三行分别完成：记录 DMA RX 能力、定义并初始化静态描述符、将描述符地址保存到 UART
资源配置中。局部 `static` 对象具有静态存储期，并非每次进入函数临时放在栈上再失效；
`const` 表示这份描述符不应被修改。

随后 `rt_hw_usart_init()` 关联 UART 对象：

```c
uart_obj[i].config = &uart_config[i];
uart_obj[i].serial.ops = &stm32_uart_ops;
uart_obj[i].serial.config = config;
```

这里两个 `config` 容易看混：

| 表达式 | 类型或含义 |
| --- | --- |
| `uart_obj[i].config` | 指向 `stm32_uart_config`，描述硬件资源 |
| `uart_obj[i].serial.config` | 内嵌的 `serial_configure`，保存通信与缓冲参数 |
| `uart_obj[i].config->dma_rx` | 指向 `stm32_dma_config`，描述 RX DMA 配置 |

设备注册标志再组合 `RT_DEVICE_FLAG_DMA_RX/TX` 等能力位。声明能力不等于开启传输，
应用仍须请求合适的打开模式，而且设备可能已经被控制台或其他模块打开。

## 8. IRQ 别名如何变成真正的中断函数

驱动写的是逻辑名称：

```c
void UART1_DMA_RX_IRQHandler(void)
{
    rt_interrupt_enter();
    HAL_DMA_IRQHandler(&uart_obj[UART1_INDEX].dma_rx.handle);
    rt_interrupt_leave();
}
```

若前面的映射为：

```c
#define UART1_DMA_RX_IRQHandler DMA2_Stream2_IRQHandler
```

那么预处理后，函数名称本身变成 `DMA2_Stream2_IRQHandler`，与实际中断向量入口对应。
这不是先进入 UART1_DMA_RX_IRQHandler 再调用另一个同名 DMA 函数，而是编译前的符号替换。

`DMA2_Stream2_IRQn` 是配置 NVIC 使用的编号，`DMA2_Stream2_IRQHandler` 是函数名称，
二者职责不同。另外，USART1 自身 IRQ 还会处理串口事件，不能认为有 DMA IRQ 就不需要 UART IRQ。

## 9. 波特率与缓冲区大小从哪里来

`dev_serial.h` 中的 `struct serial_configure` 包含：

```c
struct serial_configure
{
    rt_uint32_t baud_rate;
    rt_uint32_t data_bits   :4;
    rt_uint32_t stop_bits   :2;
    rt_uint32_t parity      :2;
    rt_uint32_t bit_order   :1;
    rt_uint32_t invert      :1;
    rt_uint32_t bufsz       :16;
    rt_uint32_t flowcontrol :1;
    rt_uint32_t reserved    :5;
};
```

冒号后面是位域宽度。例如 `bufsz :16` 表示该字段占 16 个二进制位，不是缓冲区固定为
16 字节。字段宽度给出的可表示范围，也不代表任意大小都被具体 HAL、内存和驱动支持。

默认宏 `RT_SERIAL_CONFIG_DEFAULT` 给出 115200 波特率、8 数据位、1 停止位、无校验、
LSB 先传、正常模式、`RT_SERIAL_RB_BUFSZ` 接收缓冲大小以及关闭硬件流控等参数。
缓冲大小应核对实际工程宏值，不能把某个 BSP 的数值当成全仓固定值。

### 9.1 通用参数怎样传到 STM32 HAL

`stm32_configure()` 结合硬件资源与通信参数：

```c
uart->handle.Instance = uart->config->Instance;
uart->handle.Init.BaudRate = cfg->baud_rate;
/* 数据位、停止位、校验与流控转换省略 */
HAL_UART_Init(&uart->handle);
```

应用提交配置时，由通用串口框架保存参数；设备已打开时，框架还会调用底层 `configure()`
应用配置，并检查返回值。未打开时的保存并不代表此刻已写入所有硬件寄存器。

数据位与校验位的 HAL WordLength 映射见第 26 节，不能简单把通用数据位数直接等同于
STM32 含校验位的硬件字长。

## 10. 引脚在哪里初始化：板级 MSP 示例

本节仅选择以下板卡作为接线配置例子：

`bsp/stm32/stm32f407-atk-explorer/board/CubeMX_Config/Src/stm32f4xx_hal_msp.c`

其中 `HAL_UART_MspInit()` 对 USART1 配置：

```c
__HAL_RCC_USART1_CLK_ENABLE();
__HAL_RCC_GPIOA_CLK_ENABLE();

GPIO_InitStruct.Pin = GPIO_PIN_9 | GPIO_PIN_10;
GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
GPIO_InitStruct.Pull = GPIO_PULLUP;
GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
```

对应 PA9 为 TX、PA10 为 RX。GPIO 复用模式将引脚交给外设信号控制；它不是应用通过普通
GPIO 写高低电平来模拟串口。该板的 MSP 中还配置 USART1 中断优先级并使能 NVIC。

UART HAL 初始化在相应初始状态及回调配置下进入 MSP 初始化路径。具体工程可能使用
不同的 MSP 文件或回调注册方式，应核对实际参与构建的实现，不能只看到一个同名文件
就认为它一定生效。不同板卡的引脚和复用编号也不保证相同。

## 11. DMA 接收缓冲区由谁申请

以下专门讨论传统框架 `RT_SERIAL_USING_DMA` 下、`bufsz > 0` 的接收路径。

`rt_serial_open()` 在选择 DMA RX 且尚未创建 RX 对象时申请：

```c
rx_fifo = (struct rt_serial_rx_fifo *)rt_malloc(
    sizeof(struct rt_serial_rx_fifo) + serial->config.bufsz);
```

一次内存申请包含两个部分：

```text
低地址
┌──────────────────────────┐
│ rt_serial_rx_fifo 管理信息 │
├──────────────────────────┤
│ bufsz 字节的接收数据区     │
└──────────────────────────┘
高地址
```

数据区地址为：

```c
rx_fifo->buffer = (rt_uint8_t *)(rx_fifo + 1);
```

`rx_fifo` 是结构体指针，`+1` 前进一个完整结构体的大小，而不是一个字节。再转换为字节
指针，就得到紧随管理信息之后的数据区入口。

框架还会清空数据区、将 `put_index/get_index` 置零、将 `is_full` 置假，再保存：

```c
serial->serial_rx = rx_fifo;
```

职责边界是：框架管理缓存和软件索引，STM32 驱动配置 DMA 将数据搬入该缓存。DMA
配置描述符本身不包含这次动态申请的缓冲区。

## 12. 从 open 到真正接收的调用链

仍以上述 `bufsz > 0`、首次建立 RX 对象、打开模式选择 DMA RX 的路径为前提：

```text
rt_device_open(..., RDWR | DMA_RX)
    ↓
rt_serial_open()
    ├─ 检查设备是否声明 DMA RX 能力
    ├─ 申请并初始化 RX FIFO 与数据区
    └─ serial->ops->control(..., CONFIG, DMA_RX)
            ↓
        stm32_control()
            ↓
        stm32_uart_dma_config()
            ├─ 选择 RX 描述符与 DMA 句柄
            ├─ stm32_dma_setup()：配置、关联、使能 DMA IRQ
            └─ HAL_UART_Receive_DMA()：实际启动接收
```

最后的调用提供具体目的地址和长度：

```c
HAL_UART_Receive_DMA(
    &uart->handle,
    rx_fifo->buffer,
    serial->config.bufsz);
```

本例使用字节宽度，长度对应缓冲区字节数。因此正常打开此模式的过程中就会启动 DMA
接收，应用第一次 `read()` 不是启动这条有缓冲路径的必要条件。后续 `read()` 从框架
已经记录的可用数据中读取，仍可能返回少于请求长度的数据。

这里只是调用链说明，不是完整可复制的应用示例；实际应用应检查 find/control/open 的
返回值、安排接收通知与线程同步，并确认设备是否已被其他模块占用。

### 12.1 INT_RX 与 DMA_RX 不要同时乱选

当前 `rt_serial_open()` 创建接收对象时先判断 `INT_RX`，再以 `else if` 判断 `DMA_RX`。
同时传入两个标志并不意味着建立两条并行接收链。应选择明确的接收模式，发送方向则
另行选择匹配设备能力的模式。

### 12.2 bufsz == 0 不是本例的另一种缓冲长度

通用框架在 `bufsz == 0` 时分配 `rt_serial_rx_dma` 管理结构，并在 read 路径通过
`dma_transmit(..., RT_SERIAL_DMA_RX)` 请求直接接收，而非采用上述内部 FIFO 启动方式。

但当前 STM32 `stm32_dma_transmit()` 只实现 TX 方向，RX 方向返回 0。因此不能认为这个
BSP 的 `bufsz == 0` 直接 DMA 接收模型也已可用，更不能将本章有缓冲循环接收的结论套到
该分支。框架具有一种分支，不代表每个底层驱动都完整实现了它。

## 13. 同一个 CONFIG 命令，为什么参数类型不同

应用提交串口参数时：

```c
rt_device_control(dev, RT_DEVICE_CTRL_CONFIG, &cfg);
```

通用串口控制函数把 `args` 解释为 `struct serial_configure *`。

框架配置底层 DMA 时：

```c
serial->ops->control(
    serial, RT_DEVICE_CTRL_CONFIG, (void *)RT_DEVICE_FLAG_DMA_RX);
```

STM32 底层控制函数把参数解释为 DMA RX/TX 标志值。这是经 `void *` 形参传递的数值
约定，不是指向一份 `serial_configure` 的地址，不能按配置结构体解引用。

| 调用入口 | 最终处理层 | CONFIG 的参数约定 |
| --- | --- | --- |
| `rt_device_control()` | 通用串口框架的 control | 通信配置结构体指针 |
| `serial->ops->control()` | STM32 驱动的 `stm32_control()` | DMA 方向标志值 |

阅读 `cmd/args` 接口必须同时确认调用层次、操作表和最终处理函数，不能仅根据相同的
命令宏推断参数类型。

## 14. 为什么缓冲区大小应在打开前设置

框架处理应用配置时有以下检查：

```c
if (pconfig->bufsz != serial->config.bufsz && serial->parent.ref_count)
{
    return -RT_EBUSY;
}
```

设备已打开时，不允许直接改变缓冲区大小，因为已有内存分配、索引和 DMA 长度可能仍
依赖旧值。将配置由 128 改为 512 并不会让原来的内存自动扩容，反而可能导致越界。

推荐的生命周期顺序是：查找设备 → 准备配置 → 配置成功 → 按所需模式打开 → 收发。
若设备已被控制台或其他用户打开，应先协调使用者；一次 `close()` 不保证所有引用都已
释放，也不能在其他线程仍使用时擅自重建缓存。

已经打开时修改通信参数与修改缓冲大小也不是同一件事。框架允许的参数修改不代表
应用可以在活动 DMA 收发中无同步地重配置；还需遵守底层 HAL 状态与业务时序要求。

## 15. 完整时序与排查清单

```text
编译期：
系列选择 → DMA 资源映射 → UART 初始化宏 → 静态描述符

设备注册：
关联配置对象 → 设置操作表和默认通信参数 → 声明设备能力

正常有缓冲 DMA RX 打开：
应用配置 → UART 硬件初始化 → 框架申请 RX 数据区
→ DMA setup → Receive_DMA 启动

接收运行：
DMA 写入缓冲 → IDLE/HT/TC 事件 → 框架更新可读数据
→ 通知应用 → 线程 read 与解析
```

UART 的实际首次初始化可能已经由控制台等使用者触发，以上是理解常规路径的分阶段
视图，不表示每次打开都会重新执行全部步骤。

| 现象 | 优先核对 |
| --- | --- |
| 找不到 uart1 | 实例宏、参与构建的驱动版本、板级注册调用 |
| DMA 模式打开返回 `-RT_EIO` | 注册能力是否包含对应模式，框架与 BSP DMA 选项是否一致 |
| 初始化成功但无数据 | 引脚复用、外设时钟、接线、通信参数、实际 RX 启动结果 |
| 有 DMA 搬运但无通知 | IRQ 别名、NVIC、HAL 回调、UART IDLE 与框架事件链 |
| 接收一段后数据异常 | 软件消费速度、缓冲容量、DMA 回绕、资源冲突及内存访问条件 |
| 配置缓冲大小返回忙 | 设备引用是否已存在，控制台或其他线程是否仍使用设备 |

## 16. 本章小结

1. `drv_config.h` 选择系列，`dma_config.h` 提供 DMA 资源映射，`uart_config.h` 组合配置；
2. 配置宏是编译期初始化内容，不是运行时硬件启动命令；
3. 硬件资源、通信参数和板级引脚是三类不同配置；
4. `static const` 描述符通过指针与 UART 对象关联，运行时再翻译为 HAL 参数；
5. 有缓冲 DMA RX 在打开过程中申请缓存并启动接收，后续 read 取出已接收的数据；
6. 同名 CONFIG 命令在不同接口层可以有不同参数约定；
7. 设备能力、打开模式、缓冲模型和底层实现必须同时匹配，不能只启用一个宏就认为可用。

继续阅读 [第 29 节：PIN 通用框架](29-PIN通用框架-设备注册操作表与跨平台分发.md)，
将设备对象、操作表和 BSP 适配的思想应用到 GPIO，再由第 30 节展开 STM32 GPIO/EXTI 实现。
