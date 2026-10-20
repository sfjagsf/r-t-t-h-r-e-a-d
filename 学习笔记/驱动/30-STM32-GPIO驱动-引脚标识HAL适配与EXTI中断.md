# 30. STM32 GPIO 驱动：引脚标识、HAL 适配与 EXTI 中断

> 对应源码：
>
> - `bsp/stm32/libraries/HAL_Drivers/drivers/drv_gpio.h`
> - `bsp/stm32/libraries/HAL_Drivers/drivers/drv_gpio.c`
> - `bsp/stm32/libraries/HAL_Drivers/drv_common.c`
>
> 对照资料与工程：
>
> - `components/drivers/include/drivers/dev_pin.h`
> - `bsp/nxp/mcx/mcxa/libraries/drivers/drv_pin.h`、`drv_pin.c`
> - `bsp/nxp/lpc/lpc55sxx/libraries/drivers/drv_pin.h`、`drv_pin.c`
> - `bsp/nxp/imx/imxrt/libraries/drivers/drv_gpio.h`
> - `bsp/nxp/imx/imxrt/imxrt1052-fire-pro/board/MCUX_Config/pin_mux.c`
>
> 本章主要以普通 STM32F4 的传统 BSP 路径举例，MP1 等特殊分支另外说明。
> “PB5 编码为 21”仅属于这里的驱动约定，不是 STM32 封装规定、CubeMX 全局编号，
> 也不是 RT-Thread 为所有芯片统一规定的引脚编号。

承接 [第 29 节](29-PIN通用框架-设备注册操作表与跨平台分发.md)。通用层调用
`rt_pin_ops`，本章继续展开 STM32 的适配函数，解释 GPIO 读写和 EXTI 回调分发。

## 1. 先区分同一个引脚的五种表示

学习讨论中的 CubeMX 截图显示 `(57)-PB5`，其中 57 是该截图所选芯片与封装的脚号，
PB5 是 GPIOB 第5位。不能因此推断全部 STM32 的 PB5 都位于第57脚；没有具体型号、
封装和原理图时，不对其他芯片作相同映射结论。

| 层次 | PB5 示例 | 用途 |
| --- | --- | --- |
| 封装位置 | 截图中的57 | 查封装、原理图、PCB 连接 |
| GPIO 硬件名称 | PB5 | GPIOB 的第5位 |
| 当前 STM32 BSP 的引脚标识 | 21，即0x15 | 传给当前驱动的 `rt_pin_*` API |
| 端口内位号 / EXTI 线号 | 5 | 第几位、对应哪条 GPIO EXTI 线 |
| HAL 位掩码 | `GPIO_PIN_5`，即0x0020 | 在一个端口中选中第5位 |

这些值不能互换。尤其是：传给 `rt_pin_write()` 的21不是传给
`HAL_GPIO_WritePin()` 的 GPIO 位掩码，封装脚号也不直接参与这个驱动的端口解码。

## 2. 原生 CubeMX/HAL 工程有没有21这个编号

普通 CubeMX/HAL 工程通常直接使用两个部分表示 PB5：

```c
HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_SET);
```

分别是端口、位掩码和电平，不要求先合成一个21。如果 CubeMX 给该引脚设置 LED 标签，
生成代码可能使用如下形式的别名：

```c
#define LED_Pin       GPIO_PIN_5
#define LED_GPIO_Port GPIOB
```

调用仍是：

```c
HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);
```

这是端口和掩码的业务别名，不是额外产生一套全局整数编码。用户工程或其他框架可以
自行添加这种编码，但不能认为它是 CubeMX 默认必须生成的内容。

### 2.1 GPIO_TypeDef 是寄存器布局，不是引脚表

设备头文件中的结构体包含 MODER、IDR、ODR、BSRR 等成员，描述一个 GPIO 端口的
寄存器排列。例如常见布局中 ODR 偏移0x14、BSRR 偏移0x18；具体寄存器及布局以对应
芯片头文件为准，不同系列可能增加、减少或改变成员。

GPIOB 的定义通常具有这样的形式：

```c
#define GPIOB ((GPIO_TypeDef *)GPIOB_BASE)
```

它把寄存器基地址解释为结构体指针，并没有为该结构体另行申请 RAM。`GPIOB->ODR`
表示访问 GPIOB 的输出寄存器，绝不是访问一个名为“引脚20”的对象。0x14虽然等于
十进制20，但在这里表示字节地址偏移。

PB5 的“5”用于选择端口寄存器中的相关位或位字段，不能把它理解成结构体第5个成员。
例如 ODR 的第5位与 MODER 中用于第5脚的双比特字段，位置和含义不同。

在原生工程中可以沿 `MX_GPIO_Init` → `GPIOB` / `GPIO_PIN_5` → `GPIOB_BASE` /
`HAL_GPIO_WritePin` 阅读，不需要先寻找 PB5=21。

参考：[ST GPIO HAL 定义](https://github.com/STMicroelectronics/stm32h5xx-hal-driver/blob/main/Inc/stm32h5xx_hal_gpio.h)。
该参考用于对照接口和掩码，不据此认定截图中具体芯片属于 H5。

## 3. 当前 BSP 怎样将 PB5 编码为21

`drv_gpio.c` 定义：

```c
#define PIN_NUM(port, no) (((((port) & 0xFu) << 4) | ((no) & 0xFu)))
#define PIN_PORT(pin)    ((uint8_t)(((pin) >> 4) & 0xFu))
#define PIN_NO(pin)      ((uint8_t)((pin) & 0xFu))
```

正常端口从 A=0、B=1、C=2 编码，端口内位号为0～15。对于 PB5：

```text
端口B：1 << 4 = 0001 0000
位号5：         0000 0101
按位或：        0001 0101 = 0x15 = 21
```

反过来：

```text
PIN_PORT(21) → (21 >> 4) & 0xF → 1 → 端口B
PIN_NO(21)   → 21 & 0xF        → 5
```

| 引脚 | 端口序号 | 位号 | 这里的BSP标识 |
| --- | --- | --- | --- |
| PA0 | 0 | 0 | 0 |
| PA5 | 0 | 5 | 5 |
| PB0 | 1 | 0 | 16 |
| PB5 | 1 | 5 | 21 |
| PC13 | 2 | 13 | 45 |

这是软件为了用一个整数传递两个信息而选择的编码，不是“PB5天生等于21”。
`& 0xF` 是截取低4位，不是完整范围校验；非法端口号或位号可能被截断成另一个值。

如果误把封装脚号57传给这里的 PIN API，按该规则会拆成端口序号3、位号9，即 PD9，
而不是 PB5。该芯片是否实际存在并引出了 PD9，还要另行检查。

## 4. GET_PIN 与 rt_pin_get 的区别

### 4.1 GET_PIN：用端口地址布局计算标识

`drv_gpio.h` 的普通非 MP1 分支按以下思路计算：

```text
目标端口基地址 - GPIOA_BASE
    ↓ 除以端口地址间隔0x400
端口序号
    ↓ 乘16，再加位号
BSP标识
```

正常对应布局下，`GET_PIN(B, 5)` 得到21。B 是宏标记，不是字符 `'B'`；宏使用
`GPIO##port##_BASE` 等记号拼接生成设备头文件符号。常量参数可以用于编译期计算，
不要给这类宏传入带副作用的复杂表达式。

MP1 使用0x1000间隔，并对 GPIOZ 作特殊处理；该头文件的 GET_PIN 位于非 DM 分支中。
不能将这个宏无条件视为每个平台都存在的通用接口。

### 4.2 rt_pin_get：运行时解析字符串

当前 `stm32_pin_get()` 接受类似 `PB.5`、`PC.13` 的形式，检查部分格式后将端口字母
换算为序号，将数字部分累积，再调用 PIN_NUM。

这是运行时解析，不是 GET_PIN 的同名替代。当前解析器没有完整检查所有字符是否为
十进制数字、位号是否确实在0～15，以及端口是否在目标芯片和封装上可用。因此“返回非负”
不能被理解为完整的硬件合法性认证。应用应使用已核实的板级映射，不接收未经校验的
任意字符串直接控制硬件。

## 5. 从标识到 GPIO 端口与位掩码

普通系列分支使用：

```c
#define PIN_STPORT(pin) \
    ((GPIO_TypeDef *)(GPIOA_BASE + (0x400u * PIN_PORT(pin))))

#define PIN_STPIN(pin) \
    ((uint16_t)(1u << PIN_NO(pin)))
```

对于21：

```text
PIN_STPORT(21) → GPIOA_BASE + 0x400 × 1 → GPIOB
PIN_STPIN(21)  → 1 << 5 → 0x0020 → GPIO_PIN_5
```

位号5不等于掩码5。十进制5的二进制为0101，若当掩码表示的是第0位和第2位，不是第5位。
第5位的单比特掩码应为0010 0000，即0x20。

`PIN_STPORT_MAX` 主要由设备头文件可见的端口宏确定。它不证明所有端口都在所用封装
引出，也不是对每个输入整数及端口地址空洞的完整检测，调用者仍需遵守驱动适用范围。

## 6. stm32_pin_write/read：落实为 HAL 调用

写函数检查端口范围后执行：

```c
gpio_port = PIN_STPORT(pin);
gpio_pin = PIN_STPIN(pin);
HAL_GPIO_WritePin(gpio_port, gpio_pin, (GPIO_PinState)value);
```

于是 `rt_pin_write(GET_PIN(B, 5), PIN_HIGH)` 经操作表进入这个函数，最终成为
`HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_SET)`。HAL 接收的是端口和掩码，
不是21，也不是封装脚号57。

读函数调用 `HAL_GPIO_ReadPin()`，再转换为 PIN_LOW / PIN_HIGH。当前实现端口检查
失败时返回 `-RT_EINVAL`；应用应保留有符号返回值并区分错误，不能用“非零就是高电平”
处理全部结果。写入和模式接口返回 void，也不能仅凭函数返回就断言硬件操作成功。

## 7. stm32_pin_mode：通用模式翻译成 HAL 参数

函数设置引脚掩码和 GPIO 输出速度，再按模式填写：

| RT-Thread 模式 | HAL Mode | HAL Pull |
| --- | --- | --- |
| `PIN_MODE_OUTPUT` | `GPIO_MODE_OUTPUT_PP` | `GPIO_NOPULL` |
| `PIN_MODE_INPUT` | `GPIO_MODE_INPUT` | `GPIO_NOPULL` |
| `PIN_MODE_INPUT_PULLUP` | `GPIO_MODE_INPUT` | `GPIO_PULLUP` |
| `PIN_MODE_INPUT_PULLDOWN` | `GPIO_MODE_INPUT` | `GPIO_PULLDOWN` |
| `PIN_MODE_OUTPUT_OD` | `GPIO_MODE_OUTPUT_OD` | `GPIO_NOPULL` |

最后调用 `HAL_GPIO_Init(PIN_STPORT(pin), &GPIO_InitStruct)`。速度字段是 GPIO 输出
电气速度相关配置，不是让引脚自动按某个时钟频率翻转。

当前开漏模式未自动开启内部上拉，应核对实际电路。模式分支没有完整的未知值错误处理，
因此只应传入本实现明确支持的模式，不能因为 void 接口没有返回错误就认为任意值都合法。

## 8. 三个中断数据结构分别存什么

| 对象 | 含义 |
| --- | --- |
| `pin_irq_map[]` | 某条 GPIO EXTI 线对应的位掩码和 NVIC IRQ 编号 |
| `pin_irq_hdr_tab[]` | 每条线已登记的完整引脚标识、触发模式、回调、参数 |
| `pin_irq_enable_mask` | 哪些 EXTI 线当前被记录为启用，供共享 IRQ 禁用判断使用 |

`pin_irq_hdr_tab` 有16项，按 GPIO EXTI 线号0～15组织，不是给每个端口各建16项，
也不表示芯片的整个 EXTI 控制器只有16种中断源。

## 9. bit2bitno：把单比特掩码变回位号

代码主干为：

```c
for (i = 0; i < 32; i++)
{
    if (((rt_uint32_t)0x01 << i) == bit)
    {
        return i;
    }
}
return -1;
```

输入0x20时，依次与1、2、4、8、16、32比较，匹配 `1 << 5`，返回5。

对照前面学过的 `__rt_ffs()`，应明确：

- 它不是统计有几个1；
- 它要求整个输入恰好等于一个单比特掩码；
- 返回的位号从0开始，`__rt_ffs()` 返回位置从1开始；
- 输入0或多比特掩码如0x30时，这个函数返回-1，不会选择其中最低的一个1。

因此 `bit2bitno(PIN_STPIN(pin))` 可以将选中的 GPIO 位转换为回调表索引。

## 10. 同位号不同端口，为何可能争用中断

以普通 STM32F4 的 GPIO EXTI 路径为例：

```text
PA5 ─┐
PB5 ─┼─ 选择一个来源 → EXTI5 → 回调表第5项
PC5 ─┘
```

这些引脚可同时作为普通 GPIO 使用，但不能在这里各自独立占用同一条 EXTI5。回调
表项保存完整 pin，负责记录当前是哪一个端口占用了该线。

`stm32_pin_attach_irq()` 先算索引，再检查：若 pin、回调、模式、args 全部相同，重复
登记返回成功；若表项已被其他登记占用，则返回 `-RT_EBUSY`；否则保存四项信息。
当前 attach 并不会自动替换已有回调，也没有在此一步配置整个 GPIO/EXTI 硬件。

表项更新在关中断保护内进行，随后恢复原中断状态，不是无条件打开中断。它保护更新
过程，不代表驱动提供了完备的多使用者引脚所有权管理。

## 11. irq_enable：配置触发模式和 NVIC

启用前要求对应回调表项已登记，然后按触发方式设置：

| 触发模式 | GPIO Mode | 上下拉 |
| --- | --- | --- |
| 上升沿 | `GPIO_MODE_IT_RISING` | `GPIO_PULLDOWN` |
| 下降沿 | `GPIO_MODE_IT_FALLING` | `GPIO_PULLUP` |
| 双边沿 | `GPIO_MODE_IT_RISING_FALLING` | `GPIO_NOPULL` |

之后执行 GPIO 初始化，设置 NVIC 优先级5、0，使能对应 IRQ，并置位 enable_mask。

两个重点：其一，这会重新设置上下拉，不能假定此前 `rt_pin_mode()` 的偏置始终保留；
其二，5、0是 NVIC 参数，不是线程优先级，其实际含义还取决于优先级分组等配置。

当前 switch 只明确处理上述三种边沿模式，没有完整的非法模式拒绝分支。虽然通用
头文件还声明高、低电平触发常量，也不能因此认为这个实现正确支持了它们。

## 12. 不同 EXTI 线共用 NVIC，不等于同一条 EXTI 线

普通 F4 分支的映射为：

| EXTI 线 | NVIC 入口 |
| --- | --- |
| 0～4 | 各自独立入口 |
| 5～9 | `EXTI9_5_IRQn` |
| 10～15 | `EXTI15_10_IRQn` |

例如 PA5 与 PB6 使用不同线，但共享一个 CPU 中断入口：

```text
PA5 → EXTI5 ─┐
             ├→ EXTI9_5_IRQn
PB6 → EXTI6 ─┘
```

这与 PA5/PB5 争用同一 EXTI5 不同。enable_mask 解决的是共享 NVIC 的关闭问题：
禁用 EXTI5 时清除其位，如果 EXTI6 仍启用，就保留公共 IRQ；同组都未使用时才关闭。

这是一份启用位图，不是给每个调用者加减的引用计数，也不能自动解决多人操作同一线。
其他系列有独立或不同分组映射，应核对对应编译分支；不能把本节的 F4 分组当成所有
STM32 的共同布局，映射表与禁用分组逻辑也应一起检查。

## 13. 中断入口怎样找到用户回调

普通 F4 共享入口会依次调用：

```c
rt_interrupt_enter();
HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_5);
HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_6);
HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_7);
HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_8);
HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_9);
rt_interrupt_leave();
```

各次交给 HAL 检查、处理对应挂起状态，不是无条件调用五个用户回调。以5号线为例：

```text
PB5 边沿变化
    ↓
EXTI5 产生中断
    ↓
EXTI9_5_IRQHandler()
    ↓
HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_5)
    ↓
HAL_GPIO_EXTI_Callback(GPIO_PIN_5)
    ↓
bit2bitno(0x20) → 5
    ↓
pin_irq_hdr_tab[5].hdr(pin_irq_hdr_tab[5].args)
```

部分系列走分别命名的 Rising/Falling 回调，最终仍转到表项。实际匹配应同时核对当前
HAL 版本和编译分支，不能只从通用函数名字推断全部系列都走同一回调。

用户回调仍在中断上下文内。常见安排是 ISR 通知线程、线程做消抖与业务处理；本驱动
操作表的 pin_debounce 为 NULL，没有自动替全部按键完成消抖。

## 14. disable 与 detach：清理范围不同

禁用路径先找到 IRQ 映射，在关中断保护内执行 `HAL_GPIO_DeInit()`，清除 enable_mask
中的对应位，并按同组使用情况决定是否关闭 NVIC。

因此它不只是屏蔽 CPU 中断，还会反初始化引脚配置。若禁用后要改为普通输入或输出，
需重新设置所需模式，不应假定原配置仍在。

`stm32_pin_dettach_irq()` 则清空回调表项的 pin、hdr、mode、args，没有替代前面的
硬件禁用流程。正常顺序是先 disable 再 detach，并协调已发生事件和上下文的生命周期。

当前部分管理路径按位号索引处理，未完整校验调用者是否拥有已登记的那个端口引脚。
不能在 PA5 已登记时误传 PB5 去解除、启用或禁用，并期待驱动自动阻止所有误操作。

## 15. 初始化：开时钟并注册操作表

`rt_hw_pin_init()` 根据可见的 HAL 时钟宏开启相应 GPIO 时钟；L4 的 GPIOG 路径还有
启用 VddIO2 的特殊处理。然后调用：

```c
rt_device_pin_register("pin", &_stm32_pin_ops, RT_NULL);
```

操作表把模式、读写和中断接口关联到上述 STM32 函数，pin_get 对应名称解析器，消抖项
为空。普通 mode/write 路径依赖初始化准备，不是每次调用都独立完成整个时钟流程。

当前 `drv_common.c` 在 `RT_USING_PIN && !RT_USING_DM` 条件下由板级初始化调用
`rt_hw_pin_init()`，本文件主体还受 `BSP_USING_GPIO` 约束。不能仅以文件末尾有没有
自动初始化导出宏判断驱动是否初始化，更不能将传统路径当成 DM 平台的唯一入口。

## 16. NXP 原生 SDK 与 BSP 编码对照

跨厂商理解时，先分开“芯片 GPIO 身份”“SDK 参数”和“RT-Thread BSP 标识”。当前
仓库可见的规则如下，不代表厂商全部芯片的永久统一约定：

| BSP | 宏规则 | 示例 |
| --- | --- | --- |
| MCXA | `32 * port + pin` | 端口1、第5位 → 37 |
| LPC55Sxx | `32 * port + pin + 1` | PIO1_5 → 38 |
| i.MX RT | `32 * (port - 1) + (pin & 31)` | GPIO2_IO05 → 37 |

MCXA 的适配函数用除32、模32拆出端口与位号；LPC55Sxx 使用从1开始的映射，解码前
先减1。因此同一厂商也可能有不同 BSP 约定，不同厂商也可能恰好采用相同公式。

### 16.1 原生 SDK 通常不要求先合并成37或38

接口示意：

```c
/* 当前 MCXA 对应 SDK：GPIO 实例 + 位号 + 电平 */
GPIO_PinWrite(GPIO1, 5U, 1U);

/* 当前 LPC55Sxx 对应 SDK：GPIO 寄存器入口 + 端口号 + 位号 + 电平 */
GPIO_PinWrite(GPIO, 1U, 5U, 1U);
```

这是不同目标 SDK 的调用形式，不能把两个不同原型当作同一工程里的 C 函数重载。
本例中第二种接口需要框架的38先拆成端口1、位号5，SDK 本身并不要求输入38。

### 16.2 pin 参数与 mask 参数不能混用

| 接口形式 | 选择第5位的参数 |
| --- | --- |
| STM32 `HAL_GPIO_WritePin()` | `GPIO_PIN_5`，掩码0x20 |
| 上述 NXP `GPIO_PinWrite()` | 位号5 |
| NXP 对应 `GPIO_PortSet()` 一类批量接口 | 掩码 `1UL << 5` |

同一 SDK 的不同函数也可能分别要求位号和掩码。不能简单地把所有函数名里的 Pin 理解为
同一种数据，也不能迁移时只替换函数名却保留旧参数形式。

参考：[NXP GPIO 接口](https://mcuxpresso.nxp.com/api_doc/dev/1241/group__gpio__driver.html)、
[NXP LPC GPIO 接口](https://mcuxpresso.nxp.com/api_doc/dev/779/group__lpc__gpio.html)。

### 16.3 i.MX RT 的 PAD 名与 GPIO 功能名

RT1052 的本地配置代码可见：

```c
IOMUXC_SetPinMux(IOMUXC_GPIO_AD_B0_03_GPIO1_IO03, 0U);
```

表示将名为 GPIO_AD_B0_03 的 PAD 复用到 GPIO1_IO03 功能。封装脚号/球位、PAD 名称、
GPIO 功能名与 BSP 整数编码是不同层次。这里两段名字恰好都含03，不代表每个 PAD
都能通过名称尾数直接推算 GPIO 位号，应查具体芯片的复用映射。

## 17. 常见误解与实际排查

| 误解或现象 | 应核对的事实 |
| --- | --- |
| CubeMX 图上57，所以 PIN API 传57 | 57是当前封装脚号；本驱动会把57解码成D9 |
| 原生 HAL 工程找不到PB5=21 | 正常，21属于本BSP额外编码，HAL直接用端口和掩码 |
| 所有芯片都按端口乘16 | 不成立，具体 BSP 可用其他公式、查表或DM范围映射 |
| 设置 GPIO 模式就能知道 LED 亮灭 | 还要核对接线、有效电平和电气条件 |
| PA5与PB5能各自登记独立EXTI | 这里争用同一条线，后登记通常返回忙 |
| PA5与PB6共享IRQ，所以也不能共用 | 它们是不同EXTI线，共享的是CPU入口，可以分别处理 |
| 禁用IRQ后可继续按原GPIO模式使用 | 当前禁用路径调用DeInit，可能需要重新配置 |
| attach成功就能触发回调 | 还需enable、有效边沿、正确时钟/接线/IRQ/HAL回调链 |
| pin_get返回非负就证明引脚存在 | 当前解析与端口检查并非完整的封装/输入合法性验证 |

调试前应先确认没有占用 UART、SPI、SWD 或其他关键引脚，再根据模式、时钟、EXTI
选择、NVIC 使能、回调表项和应用同步逐层检查。不要在未知接线下直接执行写电平命令。

## 18. 本章小结

1. 封装脚号、GPIO名称、BSP标识、位号和掩码是五个不同层次；
2. 当前普通 STM32 BSP 用21表示PB5，原生 HAL 仍使用 GPIOB 和 GPIO_PIN_5；
3. `PIN_STPORT/PIN_STPIN` 完成编号到端口地址、位掩码的转换；
4. GPIO模式、读写接口将通用请求翻译为 HAL 调用，不自动管理所有使用者的所有权；
5. EXTI回调表按线号组织，同位号不同端口可能冲突，不同线又可能共享一个NVIC入口；
6. attach登记、enable配置启用、disable硬件清理、detach软件解除，职责不能混用；
7. 上层统一入口经函数指针分发，厂商 SDK 的函数原型和 BSP 编码不要求统一。

至此，传统 PIN 框架和 STM32 GPIO/EXTI 路径形成闭环。DM 多控制器、OFW 和 PINCTRL
仍是后续专题，本章不将其标记为已经展开学习的内容。
