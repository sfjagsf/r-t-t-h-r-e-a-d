# 29. PIN 通用框架：设备注册、操作表与跨平台分发

> 对应源码：
>
> - `components/drivers/include/drivers/dev_pin.h`
> - `components/drivers/pin/dev_pin.c`
> - `components/drivers/pin/SConscript`
>
> 对照驱动：
>
> - `bsp/stm32/libraries/HAL_Drivers/drivers/drv_gpio.c`
> - `bsp/nxp/mcx/mcxa/libraries/drivers/drv_pin.c`
> - `bsp/nxp/lpc/lpc55sxx/libraries/drivers/drv_pin.c`
>
> 本章先解释传统 BSP 路径。`RT_USING_DM`、OFW 和 PINCTRL 有扩展实现，只说明边界，
> 不将传统 STM32 的单设备转发和引脚编码推广到全部平台。

承接 [第 28 节](28-STM32串口与DMA配置-资源映射参数与接收启动.md)。前面串口通过
`rt_uart_ops` 对接硬件，本章看 PIN 框架怎样通过 `rt_pin_ops` 对接不同芯片的 GPIO。
当前仓库文件名是 `dev_pin.c`、`dev_pin.h`，不是某些旧资料中的 `pin.c`、`pin.h`。

## 1. 本章先明确的三个结论

1. `rt_pin_write()` 统一的是操作入口，不是所有厂商的引脚编号；
2. 应用参数 `pin` 是当前驱动能够解释的引脚标识，不一定是封装脚号，也不一定是位掩码；
3. 函数指针操作表实现统一入口到不同实现的分发，厂商 SDK 函数不必具有相同名称和参数形式。

例如，当前 STM32 驱动中可以有：

```text
rt_pin_write(21, PIN_HIGH)
    ↓
stm32_pin_write()
    ↓
HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_SET)
```

而当前 MCXA BSP 的对应编码可以是：

```text
rt_pin_write(37, PIN_HIGH)
    ↓
mcx_pin_write()
    ↓
GPIO_PinWrite(GPIO1, 5U, 1U)
```

这里比较的是接口层次，不表示 PB5 与 MCXA 的 GPIO1 第5位是相同物理引脚或相同业务功能。
代码中的 21、37 也不是推荐直接写入应用的跨平台常量。

## 2. 框架在整个调用链中的位置

```text
应用：模式、读写、中断绑定
    │ rt_pin_mode / rt_pin_read / rt_pin_write 等
    ▼
dev_pin.c：PIN 通用包装层
    │ rt_pin_ops
    ▼
BSP GPIO 适配函数
    │ 解析本平台编号，转换参数
    ▼
厂商 SDK / HAL / 寄存器
    ▼
GPIO 硬件
```

框架不直接决定 GPIO 寄存器地址，不掌握所有芯片的端口布局，也不会通过统一接口自动
完成板卡接线适配。相同应用逻辑可以复用，但引脚映射、有效电平和电气条件仍需适配。

构建脚本在启用 `RT_USING_PIN` 时引入 `dev_pin.c`；启用 DM、OFW 等条件时还引入各自的
扩展文件。应用能包含一个头文件，并不等于相应底层驱动已参与构建、完成初始化和注册。

## 3. rt_device_pin：通用设备对象与具体操作表

结构主干为：

```c
struct rt_device_pin
{
    struct rt_device parent;
#ifdef RT_USING_DM
    /* IRQ chip、引脚范围、链表等扩展成员省略 */
#endif
    const struct rt_pin_ops *ops;
};
```

`parent` 位于开头，使 PIN 对象能接入通用设备框架。当前实现可以将指向该首成员的
`rt_device_t` 转回 `struct rt_device_pin *`，但不能对任意其他设备类型随意做同样转换。

`ops` 指向底层实现。注册时通常传入静态操作表，框架保存指针而不是复制整张表，所以
操作表必须在后续使用期间始终有效，不能指向已经返回函数的临时栈对象。

### 3.1 不要混淆两张操作表

| 表达式 | 操作表类型 | 职责 |
| --- | --- | --- |
| `_hw_pin.parent.ops` | `rt_device_ops` | 接入通用设备的 read/write/control 等接口 |
| `_hw_pin.ops` | `rt_pin_ops` | 接入 BSP 的 GPIO 模式、读写、中断等实现 |

只有启用 `RT_USING_DEVICE_OPS` 时，通用层才以 `parent.ops` 形式组织接口；否则设置
`parent.read/write/control` 等成员。这个开关与 `RT_USING_DM` 不是同一个概念。

## 4. rt_pin_ops：哪些接口由底层提供

以下为传统操作表主干，省略 DM/PINCTRL 扩展成员：

```c
struct rt_pin_ops
{
    void (*pin_mode)(struct rt_device *device, rt_base_t pin, rt_uint8_t mode);
    void (*pin_write)(struct rt_device *device, rt_base_t pin, rt_uint8_t value);
    rt_ssize_t (*pin_read)(struct rt_device *device, rt_base_t pin);
    rt_err_t (*pin_attach_irq)(struct rt_device *device, rt_base_t pin,
        rt_uint8_t mode, void (*hdr)(void *args), void *args);
    rt_err_t (*pin_detach_irq)(struct rt_device *device, rt_base_t pin);
    rt_err_t (*pin_irq_enable)(struct rt_device *device, rt_base_t pin,
        rt_uint8_t enabled);
    rt_base_t (*pin_get)(const char *name);
    rt_err_t (*pin_debounce)(struct rt_device *device, rt_base_t pin,
        rt_uint32_t debounce);
};
```

| 功能 | 操作成员 | 包装层的处理 |
| --- | --- | --- |
| 设置模式、写电平、读电平 | `pin_mode/write/read` | 检查操作表非空后直接调用相应成员 |
| 中断管理 | `pin_attach_irq/detach_irq/irq_enable` | 成员为空时返回 `-RT_ENOSYS` |
| 名称转编号 | `pin_get` | 成员为空时返回 `-RT_ENOSYS` |
| 消抖配置 | `pin_debounce` | 成员为空时返回 `-RT_ENOSYS` |

基础三个接口没有逐一判断对应函数指针是否为空，底层必须正确提供所需实现。断言也不等于
对全部调用参数的运行期校验，不能依赖关闭断言后的错误指针调用行为。

## 5. _hw_pin 与注册流程

文件保存一个静态入口对象：

```c
static struct rt_device_pin _hw_pin;
```

传统路径中，一个 PIN 设备管理多根引脚，应用通过 `pin` 选择具体引脚，而非为 PA0、PA1、
PB0 分别创建一个 `_hw_pin`。不能反复调用这个传统注册函数，期望它自动分配多个独立设备。

注册函数的关键逻辑为：

```c
_hw_pin.parent.type = RT_Device_Class_Pin;
/* 设置通用设备接口、清空通知指针等操作省略 */
_hw_pin.ops = ops;
_hw_pin.parent.user_data = user_data;
rt_device_register(&_hw_pin.parent, name, RT_DEVICE_FLAG_RDWR);
return 0;
```

STM32 驱动调用：

```c
rt_device_pin_register("pin", &_stm32_pin_ops, RT_NULL);
```

从而将入口对象与 STM32 操作表关联，并按名称注册为 Pin 类设备。

源码细节：当前 `rt_device_pin_register()` 未将 `rt_device_register()` 的返回值传回，
而是直接返回 0。因此不能仅凭这个包装函数返回 0，就断言底层设备注册一定成功。

## 6. rt_pin_write：统一入口怎样找到实际实现

原实现为：

```c
void rt_pin_write(rt_base_t pin, rt_ssize_t value)
{
    RT_ASSERT(_hw_pin.ops != RT_NULL);
    _hw_pin.ops->pin_write(&_hw_pin.parent, pin, value);
}
```

这行代码没有判断“是否 STM32”，而是读取已注册的函数指针。其关系可写成：

```text
初始化：BSP 将操作表地址保存到 _hw_pin.ops

调用：rt_pin_write(pin, value)
                 ↓
       _hw_pin.ops->pin_write(...)
                 ↓
       已注册的 BSP 适配函数
```

`rt_pin_mode()`、`rt_pin_read()` 采用相同转发思路。专用接口没有先执行 find/open，也不
通过通用设备的 `parent.ops->write`，所以通常不需要应用先打开名为 `pin` 的设备，但
必须保证底层已正确初始化并注册。专用调用也不能被理解为自动取得引脚独占使用权。

`rt_pin_write()` 的公开 value 形参类型与底层操作表的 `rt_uint8_t` 不同，调用时发生参数
转换。应用应使用 `PIN_LOW`、`PIN_HIGH`，不要把任意整数当作可靠的电平输入。

## 7. 这是不是 C 语言实现的多态

是，可以将操作表方式称为使用函数指针实现的多态或间接分派。其核心是：调用位置和接口
约定不变，实际执行哪个适配函数由操作表决定。

下面是成员关联示意，不是替换源码中其他表项的完整操作表：

```c
/* STM32 操作表中的对应成员 */
.pin_write = stm32_pin_write

/* MCXA 操作表中的对应成员 */
.pin_write = mcx_pin_write
```

两者都符合 `rt_pin_ops.pin_write` 的函数指针类型，但函数内部可以调用不同厂商接口：

```text
stm32_pin_write → HAL_GPIO_WritePin(端口, 位掩码, 电平)
mcx_pin_write   → GPIO_PinWrite(GPIO实例, 位号, 电平)
```

不同平台通常编译各自驱动，并在初始化时确定操作表；这里的间接调用机制不表示固件会
自动探测任意芯片、每次重新判断平台，或在 STM32 与 NXP 之间运行时热切换。

### 7.1 不等于同名函数重载

某些 NXP SDK 提供三个参数的 `GPIO_PinWrite()`，LPC55Sxx 对应接口则有四个参数。
这是不同 SDK 接口定义，不是 C 根据参数数量在同一工程中自动选择重载。C 普通函数不
支持这种重载；工程必须使用与当前实现匹配的声明。

也不能把参数类型不匹配的厂商函数直接赋给操作表，再用强制类型转换掩盖差异。中间
适配函数负责解析 BSP 编号、调整参数，然后按厂商 API 的真实类型调用。

## 8. 模式、电平与名称转换

| 分类 | 常见常量 | 含义 |
| --- | --- | --- |
| 电平 | `PIN_LOW`、`PIN_HIGH` | 低、高逻辑电平 |
| 普通模式 | `PIN_MODE_OUTPUT`、`PIN_MODE_INPUT` | 输出、输入 |
| 输入偏置 | `PIN_MODE_INPUT_PULLUP/PULLDOWN` | 带上拉、下拉输入 |
| 输出类型 | `PIN_MODE_OUTPUT_OD` | 开漏输出 |

`PIN_MODE_OUTPUT` 和 `PIN_LOW` 都编码为 0，并不代表二者语义相同，它们属于不同参数域。
开漏写高通常是释放输出，由上拉等外部条件形成高电平；高电平也不一定代表板载 LED 点亮。

`rt_pin_get()` 将字符串解析交给底层，STM32 的常见调用为：

```c
rt_base_t pin = rt_pin_get("PB.5");
if (pin < 0)
{
    /* 处理不支持或解析失败；不要继续操作这个负数 */
    return;
}
```

这只是名称转换片段，不是完整应用。字符串形式并非所有平台统一，名称转成功也不保证
该引脚已在当前封装引出、未被其他外设占用。STM32 解析器的校验边界见下一节。

## 9. 通用设备 API 是另一条入口

框架还提供 `_pin_read()`、`_pin_write()` 和 `_pin_control()`，用于适配通用设备接口：

```text
rt_device_write() → _pin_write() → BSP pin_write
rt_pin_write()   ───────────────→ BSP pin_write
```

通用读写使用的载荷是：

```c
struct rt_device_pin_value
{
    rt_base_t pin;
    rt_uint8_t value;
};
```

写入时，结构体同时指定引脚和电平；读取时，调用者先填写 pin，框架再写入 value。
当前包装函数要求 buffer 非空且 `size == sizeof(struct rt_device_pin_value)`，不满足
就返回 0。使用 `sizeof`，不要因为字段看似简单就手算结构体大小，编译器可能插入填充。

read/write 返回的 size 是所处理载荷长度，不是电平值，也不是按串口方式传输的任意
字节流；这两个函数的 pos 参数在当前实现中未用于选择引脚。

还需注意，`_pin_read()` 将底层结果赋给 8 位 value 后返回 size，没有完整传播底层负
错误码。因此不能认为通用读接口返回结构体长度就必然代表引脚合法、读取成功。

`_pin_control()` 把 args 解释为以下结构体并设置模式：

```c
struct rt_device_pin_mode
{
    rt_base_t pin;
    rt_uint8_t mode;
};
```

当前函数没有按 cmd 做分支，不能把其他设备如串口的 control 命令及参数约定直接套过来。

## 10. 中断接口：登记、启用、禁用、解除

| 操作 | API | 主要语义 |
| --- | --- | --- |
| 登记 | `rt_pin_attach_irq()` | 关联 pin、触发方式、回调和参数 |
| 启用 | `rt_pin_irq_enable(pin, PIN_IRQ_ENABLE)` | 请求底层启用中断 |
| 禁用 | `rt_pin_irq_enable(pin, PIN_IRQ_DISABLE)` | 请求底层停用中断 |
| 解除 | `rt_pin_detach_irq()` | 解除软件回调登记 |

在当前 STM32 路径中，attach 主要登记软件表项，enable 才进一步配置 GPIO/EXTI/NVIC。
正常顺序是准备输入及同步对象 → attach 并检查结果 → enable 并检查结果。停止时先
disable，再 detach，并协调可能正在执行或已经通知给线程的工作。

回调类型为：

```c
void callback(void *args);
```

args 是调用者提供的上下文，框架不自动复制它指向的数据。它必须在全部潜在回调结束前
有效，不能传入即将退出函数的临时局部对象地址，再把它当作长期上下文。

STM32 EXTI 回调在中断路径执行，不会自动变成线程。通常只做短小处理或释放信号量、
发送事件，在线程中处理按键业务。不要在 ISR 中执行线程延时、阻塞等待或耗时长循环。

头文件声明某种触发模式，不代表全部底层驱动都支持；STM32 当前边沿模式的具体实现
及共享 EXTI 线约束见第 30 节。

## 11. 消抖和 Shell 的边界

`rt_pin_debounce()` 只在底层实现该成员时转发，否则返回 `-RT_ENOSYS`。当前 STM32
操作表的对应成员是 `RT_NULL`，所以按键不会仅因调用该接口就自动完成消抖。通用包装层
没有规定这里的时间单位，支持它的具体实现应提供约定。

启用 `RT_USING_FINSH` 时，文件导出 `pin` 命令，支持 num/mode/read/write 等子命令。
STM32 下的命令形式可以是：

```text
pin num PB.5
pin mode PB.5 output
pin read PB.5
pin write PB.5 high
```

这些只是接口示例，不建议在未知用途的板卡上直接执行。命令仍调用同一组 PIN API，
不会绕过硬件资源冲突；将 UART/SPI/调试引脚改为普通 GPIO 可能中断正在使用的功能。
命令中的 high/on 表示电平，不保证板上某个负载必然开启。

## 12. 跨平台移植应该改哪里

应用最好使用板级功能名，如 `APP_STATUS_LED_PIN`，其取值由对应板卡提供，而不是业务
代码四处硬编码 21、37 或封装脚号。换板时除了引脚标识，还应核对有效电平、复用、
上下拉、驱动能力和外设占用。`GET_PIN()` 不是所有 BSP 都统一提供、参数格式完全相同的 API。

```text
应用业务：操作状态灯
    ↓
板级定义：状态灯接哪个 GPIO、什么电平有效
    ↓
PIN API：按当前 BSP 的标识发起请求
    ↓
底层适配：调用当前厂商 SDK
```

## 13. 本阶段没有展开的扩展

启用 `RT_USING_DM` 后，`dev_pin_dm.c` 通过控制器列表、`pin_start/pin_nr` 查找所属
控制器，并将全局标识换算为局部引脚再分发。因此不能从 `_hw_pin` 的存在推断整个系统
只能有一个 GPIO 控制器。

OFW 引脚描述解析和 PINCTRL 复用/电气配置属于后续专题。本章不将这些分支的对象管理、
分配方式或所有权规则与传统 STM32 路径混为一谈。

## 14. 本章小结

1. `rt_device_pin` 通过 parent 接入设备框架，通过 `rt_pin_ops` 接入芯片驱动；
2. 传统专用 API 直接经操作表分发，不要求应用先走通用设备 open 路径；
3. 统一调用方式不代表统一引脚编码，也不代表所有厂商 SDK 的参数一致；
4. 操作表体现 C 函数指针多态，同名厂商函数的接口差异不等于 C 函数重载；
5. 模式、电平、位号和位掩码必须区分；中断登记与启用也必须区分；
6. 可选接口、错误传播、注册返回值等，应按当前源码实现理解，不能仅凭接口名字推断。

继续阅读 [第 30 节：STM32 GPIO 驱动](30-STM32-GPIO驱动-引脚标识HAL适配与EXTI中断.md)，
重点对照封装脚号、PB5、BSP 编号 21、HAL 掩码 0x0020 以及 EXTI 线号 5 的区别。
