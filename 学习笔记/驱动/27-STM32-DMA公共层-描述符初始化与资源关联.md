# 27. STM32 DMA 公共层：描述符、初始化与资源关联

> 对应源码：
>
> - `bsp/stm32/libraries/HAL_Drivers/drivers/drv_dma.h`
> - `bsp/stm32/libraries/HAL_Drivers/drivers/drv_dma.c`
> - `bsp/stm32/libraries/HAL_Drivers/drivers/drv_usart.c`
>
> 本章以当前仓库源码为准，解释 STM32 外设驱动共用的 DMA 辅助层。系列专属配置、
> HAL 字段及 DMA 控制器能力以相应编译条件为准，不能直接在所有 STM32 系列间套用。

承接 [第 26 节](26-STM32传统UART驱动-RTThread框架与HAL对接.md)。上一节 UART 驱动通过
`stm32_dma_setup()` 配置 DMA，本章继续展开这个调用，重点区分静态描述符、HAL 运行期句柄、
外设关联和真正的传输启动。

## 1. 本文件负责什么，不负责什么

UART、SPI 等驱动使用 DMA 时，有一些相同的工作：开启 DMA 时钟、填写 HAL DMA 初始化
参数、初始化 DMA、关联父外设以及管理 DMA 中断。公共层将这些工作集中起来。

```text
UART / SPI 等外设驱动
    │ 提供 DMA 句柄、配置描述符、父外设
    ▼
drv_dma.c
    │ 时钟、HAL 初始化、双向关联、NVIC 管理
    ▼
STM32 HAL 与硬件
```

公共层不负责为应用申请接收缓冲区，不解释通信协议，也不是动态 DMA 通道分配器。
它的 `setup()` 不接收缓冲区地址和传输长度，所以不能单独启动一次具体的数据搬运。

`drv_dma.h` 和 `drv_dma.c` 的主体受 `HAL_DMA_MODULE_ENABLED` 约束。仅在应用层请求
DMA 模式，并不能自动补齐 HAL、框架与 BSP 的所有编译条件。

## 2. 两类对象：描述符与 HAL 句柄

| 对象 | 作用 | 典型内容 |
| --- | --- | --- |
| `struct stm32_dma_config` | 描述“应当怎样配置” | 硬件实例、方向、模式、中断、优先级 |
| `DMA_HandleTypeDef` | 保存 HAL 使用的运行期状态 | `Instance`、`Init`、`Parent`、状态和回调等 |

描述符通常定义为 `static const`，由板级或系列宏初始化；HAL 句柄由 UART 等外设驱动持有，
会随着初始化、传输和关闭而改变状态。二者不是同一个对象，也不能相互替代。

以 UART RX 为例：

```text
uart->config->dma_rx     → 静态 DMA 配置描述符
&uart->dma_rx.handle     → 接收方向的 HAL DMA 句柄
&uart->handle           → HAL UART 句柄
```

## 3. 为什么描述符分为 common 和 union

结构如下：

```c
struct stm32_dma_config
{
    struct stm32_dma_config_common common;
    union
    {
        struct stm32_dma_classic_config classic;
#if defined(STM32_DMA_USES_GPDMA)
        struct stm32_gpdma_config gpdma;
#endif
    } config;
};
```

`common` 保存共有信息，`config` 保存控制器特有的配置。

### 3.1 common：共同需要的信息

| 字段 | 含义 |
| --- | --- |
| `Instance` | DMA 通道或 Stream 等硬件实例地址 |
| `type` | 普通 DMA、BDMA 或 GPDMA 类型 |
| `dma_rcc` | 控制器对应的 RCC 时钟使能位 |
| `dma_irq` | DMA 中断编号 |
| `priority` | DMA 硬件搬运仲裁优先级 |
| `preempt_priority`、`sub_priority` | NVIC 中断优先级参数 |
| `request` | 请求选择值；只有相应系列条件下才存在 |
| `direction` | 外设到内存、内存到外设等方向 |
| `mode` | 普通、循环等工作模式 |

DMA 仲裁优先级、NVIC 中断优先级和 RT-Thread 线程优先级是三套不同的概念。

### 3.2 classic：传统 DMA / BDMA 配置

主要包括外设与内存地址是否递增、两端的数据访问宽度，以及可选的 Channel、FIFO、Burst
配置。软件描述符中的 Channel/FIFO 等成员始终存在，但初始化宏及应用函数会按当前
系列和控制器类型决定是否填写、复制它们。

### 3.3 gpdma：使用源和目标的表达方式

GPDMA 分支使用 `src_inc`、`dest_inc`、`src_data_width`、`dest_data_width` 等字段，
另外描述突发长度、端口和传输事件模式。不能将传统 DMA 的所有 HAL 字段机械套过来。

`union` 的各成员共享存储空间，只能按正确的 `common.type` 解释对应成员。运行时的类型
分支与编译期的系列宏一起决定实际使用的字段；这不是让任意固件都能运行全部控制器类型。

## 4. stm32_dma_enable_clock：为什么先开时钟

初始化寄存器前，需要先让对应 DMA 控制器获得时钟。公共层根据系列选择 RCC 寄存器，
例如 `AHBENR`、`AHB1ENR` 或 `MP_AHB2ENSETR`。

STM32H7 的 BDMA 有专门分支，使用 `AHB4ENR`，随后直接返回，避免错误进入普通 DMA
时钟路径。对于使用请求选择的系列，代码还尝试启用相应 DMAMUX 时钟；实际展开取决于
芯片头文件提供的 DMAMUX 实例和 HAL 宏。

典型代码形式是：

```c
SET_BIT(RCC->AHB1ENR, dma_rcc);
tmpreg = READ_BIT(RCC->AHB1ENR, dma_rcc);
UNUSED(tmpreg);
```

设置时钟位后回读寄存器，遵循时钟使能后的访问时序；`UNUSED` 用于避免未使用变量警告。
这里不是等待 DMA 搬运完成，也没有启动数据传输。

## 5. apply_config：将描述符翻译成 HAL 配置

公共字段先由 `stm32_dma_apply_common_config()` 写入句柄，例如：

```c
dma_handle->Instance = common->Instance;
dma_handle->Init.Direction = common->direction;
dma_handle->Init.Mode = common->mode;
dma_handle->Init.Priority = common->priority;
```

随后 `stm32_dma_apply_config()` 按 `common.type` 分派：

- 普通 DMA / BDMA：复制地址递增和数据对齐字段；普通 DMA 再按系列能力复制 Channel、FIFO 等字段。
- GPDMA：在对应编译条件下，复制源/目标递增、数据宽度、Burst、端口及事件模式。

这个阶段主要是填写 C 对象中的 `Instance`、`Init`。不能把“填好 HAL 参数”直接理解成
“硬件已经完成配置”，更不能理解成“正在接收数据”。

## 6. stm32_dma_init：建立 DMA 初始状态

函数原型：

```c
rt_err_t stm32_dma_init(
    DMA_HandleTypeDef *dma_handle,
    const struct stm32_dma_config *dma_config);
```

实际顺序为：

```text
检查句柄和描述符非空
    ↓
开启 DMA 控制器时钟
    ↓
将描述符写入 HAL 句柄
    ↓
HAL_DMA_DeInit()
    ↓
HAL_DMA_Init()
    ↓
返回 RT_EOK
```

先反初始化、再初始化，是为了按所需参数重新建立初始状态。任一步 HAL DeInit/Init 失败，
代码记录日志并返回 `-RT_ERROR`；不会继续执行后面的正常初始化步骤。

这不意味着可以在其他外设仍使用同一 DMA Stream 时随意初始化。资源使用权应由 BSP
配置和驱动生命周期保证，本函数没有检测所有潜在占用者。

## 7. stm32_dma_setup：为什么需要二级指针

函数原型：

```c
rt_err_t stm32_dma_setup(
    DMA_HandleTypeDef *dma_handle,
    void *parent_handle,
    DMA_HandleTypeDef **dma_slot,
    const struct stm32_dma_config *dma_config);
```

UART 接收方向的调用是：

```c
stm32_dma_setup(
    &uart->dma_rx.handle,
    &uart->handle,
    &uart->handle.hdmarx,
    dma_config);
```

参数对应关系：

| 参数 | UART RX 示例 | 用途 |
| --- | --- | --- |
| `dma_handle` | `&uart->dma_rx.handle` | 要初始化的 DMA 句柄 |
| `parent_handle` | `&uart->handle` | 所属 UART 的 HAL 句柄 |
| `dma_slot` | `&uart->handle.hdmarx` | UART 内部保存 DMA 指针的位置 |
| `dma_config` | `uart->config->dma_rx` | 静态配置描述符 |

注意下面两个表达式不同：

```c
uart->handle.hdmarx     /* 一个 DMA_HandleTypeDef * 指针 */
&uart->handle.hdmarx    /* 这个指针变量的地址，类型为二级指针 */
```

函数需要修改父句柄里的指针，所以接收的是指针变量的地址。初始化成功且父句柄、槽地址
都非空时，它执行：

```c
*dma_slot = dma_handle;
dma_handle->Parent = parent_handle;
```

等价的 UART 关系为：

```text
UART HAL 句柄 --hdmarx--> DMA HAL 句柄
UART HAL 句柄 <--Parent-- DMA HAL 句柄
```

前向指针让 UART HAL 找到自己的接收 DMA；反向指针使 DMA 回调能找到所属 UART。
`parent_handle` 使用 `void *`，是为了复用到 UART、SPI 等不同父句柄类型，并非新分配对象。

`setup()` 的完整顺序是：调用 `init()` → 建立上述关联 → 设置并使能 DMA IRQ。
如果 `init()` 失败，则直接返回，不建立正常关联，也不执行后续 IRQ 使能。

## 8. setup 完成后，为何还要 Receive_DMA

UART 驱动完成 DMA setup 后，继续调用：

```c
HAL_UART_Receive_DMA(
    &uart->handle,
    rx_fifo->buffer,
    serial->config.bufsz);
```

这一层才提供实际目标缓冲区和接收长度，并启动接收。两步的职责分别是：

```text
stm32_dma_setup()         → 配好搬运工具并关联父外设
HAL_UART_Receive_DMA()    → 指定本次目的地、数量，启动接收
```

发送方向同样需要后续的 `HAL_UART_Transmit_DMA()`，不能仅通过设置 `DMA_MEMORY_TO_PERIPH`
就自动发送数据。

## 9. IRQ get/put：为什么有引用计数

一些芯片的多个 DMA 通道共用同一条 NVIC 中断线。假设 A、B 两个使用者都需要该中断：

```text
A 启用：引用数 0 → 1，配置并使能 IRQ
B 启用：引用数 1 → 2，保持 IRQ
A 退出：引用数 2 → 1，不能关闭 IRQ
B 退出：引用数 1 → 0，才关闭 IRQ
```

当前 `.c` 实现对特定编译条件下的已知共享 IRQ 使用引用计数，涉及 F1、L0、G0 以及 F0
的相应分支。这里以实现为准，不能仅依据头文件注释中的示例清单判断全部覆盖范围。
其他 IRQ 仍直接设置优先级、使能或禁用。

计数修改前关中断、修改后恢复原状态，保护更新过程。共享 IRQ 第一次启用时设置优先级，
后来的使用者增加引用但不重新设置，因此共享该线的驱动仍需要协调优先级要求。

它只解决“某个使用者退出时，不要关闭别人仍需要的中断线”，并不负责：

- 为多个外设分配同一 DMA 硬件通道；
- 自动编写共享 IRQ 内部的所有中断分发逻辑；
- 防止同一使用者重复 setup 而未配对释放。

共享 IRQ 和共享同一个 DMA Stream，是两类不同的问题。

## 10. stm32_dma_deinit：释放到什么程度

关键顺序为：

```text
释放 IRQ 使用权（必要时关闭 IRQ）
    ↓
abort_first 为真时调用 HAL_DMA_Abort()
    ↓
调用 HAL_DMA_DeInit()
```

`abort_first` 允许调用者指定是否先尝试中止当前搬运。按当前实现，Abort 失败只记录警告，
随后仍继续 DeInit；最终若 DeInit 成功，函数仍返回 `RT_EOK`。不能把头文件的返回值
说明理解为“Abort 一失败就一定返回错误”。

该函数没有自动完成下列工作：

| 工作 | 负责方或注意事项 |
| --- | --- |
| 释放串口接收缓冲区 | 串口框架的关闭流程 |
| 清空 UART 的 `hdmarx/hdmatx` | 需要相应调用路径自行处理 |
| 清空 DMA 的 `Parent` | 需要相应调用路径自行处理 |
| 关闭整个 DMA 控制器时钟 | 本函数未实现；同一控制器可能服务其他外设 |
| 停止应用线程对设备的访问 | 应用的生命周期与同步逻辑 |

因此 `deinit()` 是底层清理的一部分，不是设备所有资源的“一键销毁”。

## 11. 错误处理与调试顺序

UART RX 如果 setup 成功而 `HAL_UART_Receive_DMA()` 启动失败，当前 UART 驱动会尝试
调用公共 `deinit()` 回滚，再清空 `hdmarx` 和 DMA `Parent`。这个额外清理来自调用者，
不能误认为公共 `deinit()` 本身会自动断开全部指针。

排查 DMA 问题时，可以依次核对：

1. 描述符的实例、类型、请求源、方向和数据宽度是否匹配硬件；
2. DMA 及所需 DMAMUX 时钟是否启用；
3. HAL DeInit/Init 的返回值和日志；
4. 父句柄 DMA 槽与 DMA `Parent` 是否互相对应；
5. 是否实际调用了传输启动函数，缓冲区和长度是否正确；
6. IRQ 入口、NVIC 配置、HAL 回调及框架事件是否串通；
7. 缓冲区所在内存是否可由该 DMA 访问；带 Cache 的平台是否满足相应一致性要求。

最后一项属于具体芯片和调用者的内存管理要求，本公共层并未替所有传输完成缓存维护。

## 12. 本章小结

1. 描述符给出配置，HAL 句柄保存运行状态；
2. `init()` 负责时钟、参数转换和 HAL 初始状态；
3. `setup()` 在初始化基础上建立双向关联并获取 IRQ 使用权；
4. 真正的数据传输还需要外设 HAL 的启动函数、缓冲区与长度；
5. 共享 IRQ 引用计数不是 DMA 通道分配器；
6. `deinit()` 不负责所有内存、父指针和设备生命周期清理。

继续阅读 [第 28 节：STM32 串口与 DMA 配置](28-STM32串口与DMA配置-资源映射参数与接收启动.md)，
追踪配置宏如何变成描述符，以及通信参数、引脚和接收缓冲区分别在哪里准备。
