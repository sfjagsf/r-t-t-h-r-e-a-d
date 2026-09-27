# 27. 设备框架核心：通用设备 API 的实现

> 对应源码：`components/drivers/core/device.c`。
>
> 上一节的 `include/rtdef.h` 定义了 `struct rt_device` 与其操作接口；
> `include/rtthread.h` 只声明 `rt_device_*()`；本文件实现这些 API，负责设备的
> **注册、查找、生命周期状态维护，以及向具体驱动分发调用**。

## 1. 文件在设备框架中的位置

```text
驱动/BSP：创建具体设备对象，填写 type 和 ops
  │ rt_device_register(dev, "uart1", flags)
  ▼
device.c：将设备纳入内核对象系统
  │
应用：rt_device_find/open/read/write/control
  ▼
device.c：检查通用状态、维护引用计数、调用驱动回调
  ▼
具体驱动：UART、SPI、块设备、ADC 等
```

`device.c` 不直接访问 UART 寄存器、DMA 或 Flash；它只提供所有设备共同遵守的
门面。是否实际支持读、写或控制，仍由具体设备的回调决定。

整个文件被 `#ifdef RT_USING_DEVICE` 包住。关闭此配置时，通用设备对象和本文件
实现的 API 都不会参与构建。

## 2. 兼容两种设备接口布局

文件开头以宏将两种设备接口布局统一起来：

```c
#ifdef RT_USING_DEVICE_OPS
#define device_read     (dev->ops ? dev->ops->read : RT_NULL)
#else
#define device_read     (dev->read)
#endif
```

`init/open/close/read/write/control` 都遵循同一模式。

| 配置 | 回调存放位置 | `device.c` 中取得回调的方式 |
| --- | --- | --- |
| `RT_USING_DEVICE_OPS` | `dev->ops->read` 等接口表成员 | `dev->ops` 不为空才取回调 |
| 未启用 | `dev->read` 等直接成员 | 直接取得成员 |

因此后续代码只使用 `device_read`、`device_open` 等统一名字。宏消除了对象布局的
差异，但不会改变应用使用的 `rt_device_*()` API。

## 3. 注册、查找与注销

### 3.1 `rt_device_register()`

```c
rt_err_t rt_device_register(rt_device_t dev,
                            const char *name,
                            rt_uint16_t flags);
```

注册步骤：

1. `dev` 为 `RT_NULL` 时返回 `-RT_ERROR`；
2. 用 `rt_device_find(name)` 检查重名，重名也返回 `-RT_ERROR`；
3. 调用 `rt_object_init(&dev->parent, RT_Object_Class_Device, name)`，将嵌入的
   `rt_object` 初始化为设备内核对象；
4. 保存 `flag`，并将 `ref_count`、`open_flag` 清零；
5. 开启 `RT_USING_POSIX_DEVIO` 时初始化等待队列；开启 DFS V2 的 devfs 时，调用
   `dfs_devfs_device_add(dev)` 加入 devfs。

设备注册后才能通过名称查找。这里的注册不等于硬件初始化：它不会调用设备的 `init`
回调，硬件常在第一次 `open()` 时按需初始化。

`flags` 描述设备能力和生命周期状态，例如 `RT_DEVICE_FLAG_RDWR`、
`RT_DEVICE_FLAG_INT_RX`、`RT_DEVICE_FLAG_STANDALONE`；它不同于一次 `open()` 所传的
`oflag`，后者描述当前访问方式。

### 3.2 `rt_device_find()`

```c
rt_device_t rt_device_find(const char *name)
{
    return (rt_device_t)rt_object_find(name,
                                       RT_Object_Class_Device);
}
```

设备框架没有独立的设备链表；它复用内核对象系统，通过
`RT_Object_Class_Device` 限定查找类别。找不到时返回 `RT_NULL`。

### 3.3 `rt_device_unregister()`

```c
rt_err_t rt_device_unregister(rt_device_t dev);
```

它断言对象有效、确为设备且为系统对象，再调用：

```c
rt_object_detach(&dev->parent);
```

这只会将对象从内核对象系统摘除，不会释放设备结构体、不替驱动关闭硬件，也不检查
是否仍有使用者打开设备。注销前应由调用者停止新访问、等待使用者关闭，并关闭中断或
DMA 等硬件活动。

## 4. 动态设备：`create` 与 `destroy`

仅在 `RT_USING_HEAP` 下提供：

```c
rt_device_t rt_device_create(int type, int attach_size);
void rt_device_destroy(rt_device_t dev);
```

`rt_device_create()` 分配一块连续内存：对齐后的 `struct rt_device` 加上对齐后的
`attach_size` 私有附加空间。它清零通用对象部分并写入 `device->type`，但不会自动
注册设备、填写回调、设置名称或初始化硬件。

典型生命周期：

```text
create → 填写回调/私有数据 → register → open ... close → unregister → destroy
```

`rt_device_destroy()` 仅适用于动态分配对象。它要求设备已经不再注册，且不是系统对象，
最后调用 `rt_free()`。嵌在全局或静态结构体中的设备绝不能调用它。

## 5. 初始化与打开

### 5.1 `rt_device_init()`

```c
rt_err_t rt_device_init(rt_device_t dev);
```

若存在 `device_init` 回调且 `flag` 尚未包含 `RT_DEVICE_FLAG_ACTIVATED`，框架调用
驱动初始化。回调成功后设置 `ACTIVATED`；失败时记录设备名和错误码，且保持未激活，
以便后续重试。

若没有 `init` 回调，函数仍返回 `RT_EOK`，但不会自动置 `ACTIVATED`。这与 `open()`
的处理有区别。

### 5.2 `rt_device_open()`

```c
rt_err_t rt_device_open(rt_device_t dev, rt_uint16_t oflag);
```

其核心流程：

```text
设备未 ACTIVATED？
  └─ 有 init 回调则先调用；成功后置 ACTIVATED

设备是 STANDALONE 且已打开？
  └─ 返回 -RT_EBUSY

首次打开，或新的读写方式与当前方式不同？
  └─ 调用驱动 open(dev, oflag)

成功（也接受 -RT_ENOSYS）？
  └─ 置 OPEN，ref_count 加一
```

常见打开方式：

| `oflag` | 含义 |
| --- | --- |
| `RT_DEVICE_OFLAG_RDONLY` | 只读 |
| `RT_DEVICE_OFLAG_WRONLY` | 只写 |
| `RT_DEVICE_OFLAG_RDWR` | 可读写 |

`RT_DEVICE_FLAG_STANDALONE` 代表独占设备，已打开时再次打开会失败。非独占设备允许
多次打开，框架以 `ref_count` 记录打开者数量。设备没有 `open` 回调时，框架仍会保存
请求的访问方式。

## 6. 关闭与数据通路

### 6.1 `rt_device_close()`

```c
rt_err_t rt_device_close(rt_device_t dev);
```

若 `ref_count` 为零，函数返回 `-RT_ERROR`。否则它先减一：仍大于零时直接成功返回；
只有最后一个使用者关闭时，才调用具体驱动的 `close` 回调。

当最后一次 `close` 返回 `RT_EOK` 或 `-RT_ENOSYS` 时，框架将 `open_flag` 清为
`RT_DEVICE_OFLAG_CLOSE`。这种设计防止共享设备被其中一个使用者提前关闭硬件。

### 6.2 `rt_device_read()` / `rt_device_write()`

两者的通用逻辑相同：

```c
if (dev->ref_count == 0)
{
    rt_set_errno(-RT_ERROR);
    return 0;
}

if (device_read != RT_NULL)
    return device_read(dev, pos, buffer, size);

rt_set_errno(-RT_ENOSYS);
return 0;
```

因此必须先成功 `open()` 才能通过框架读写。返回值是 `rt_ssize_t`，正常情况表示实际
传输量；框架层失败时返回 `0`，错误原因保存在 errno：`-RT_ERROR` 表示设备未打开，
`-RT_ENOSYS` 表示驱动没有对应回调。

对于块设备，`pos` 和 `size` 的单位是块而非字节；串口、ADC 等字符型设备通常忽略
`pos`，但最终语义由具体驱动定义。

### 6.3 `rt_device_control()`

```c
rt_err_t rt_device_control(rt_device_t dev, int cmd, void *arg);
```

它检查对象有效后，直接调用 `device_control`；没有回调则返回 `-RT_ENOSYS`。

与 `read/write` 的关键区别是：`control()` **不检查 `ref_count`**。因此某些查询、配置、
挂起或恢复命令可以在未打开状态下由驱动自行决定是否执行。

公共命令包括 `RT_DEVICE_CTRL_RESUME`、`SUSPEND`、`CONFIG` 等；具体类别可用
`RT_DEVICE_CTRL_BASE(Type)` 划分专属命令区间，避免冲突。

## 7. 异步通知：`rx_indicate` 与 `tx_complete`

```c
rt_device_set_rx_indicate(dev, rx_ind);
rt_device_set_tx_complete(dev, tx_done);
```

这两个函数只校验对象并保存回调：

```text
dev->rx_indicate = rx_ind
dev->tx_complete = tx_done
```

通知方向如下：

```text
上层安装回调
       │
设备接收数据 / 硬件发送完成
       │
具体驱动主动调用 rx_indicate / tx_complete
       ▼
上层得知可读或发送完成
```

驱动可能在中断或 DMA 完成路径调用它们，回调通常只应做轻量工作，例如释放信号量、
发送事件或唤醒线程，不应阻塞或执行复杂业务。setter 返回 `RT_EOK` 只表示函数指针
已保存，不表示中断、DMA 或硬件通知机制已自动启用。

## 8. `RTM_EXPORT()`

每个公开 API 后面都有类似：

```c
RTM_EXPORT(rt_device_open);
```

它是 RT-Thread 模块机制的符号导出标记，使可加载模块能够解析并调用这些 API。它不
参与设备注册、状态管理或具体驱动分发。

## 9. 一次完整使用的状态变化

```text
驱动：register
  ref_count = 0，open_flag = CLOSE，尚未 ACTIVATED

应用：find → open(RDWR)
  必要时 init → ACTIVATED
  必要时驱动 open → OPEN/RDWR，ref_count = 1

应用：read/write
  ref_count 非零 → 转发到具体驱动回调

第二个使用者：open(RDWR)
  ref_count = 2

第一个使用者：close
  ref_count = 1，不调用驱动 close

最后一个使用者：close
  ref_count = 0 → 调用驱动 close → open_flag = CLOSE
```

## 10. 本文件的边界与结论

1. `device.c` 管理通用对象状态和调用分发，硬件细节始终属于具体驱动；
2. `flag` 表示能力与生命周期，`open_flag` 表示当前打开方式，`ref_count` 表示打开者数量；
3. `open()` 会按需初始化，并负责独占检查和引用计数；
4. `read/write` 必须在打开后调用，`control` 是否需要打开由驱动决定；
5. `unregister` 不释放静态对象；动态设备须在注销后调用 `destroy`；
6. `rx_indicate/tx_complete` 是设备到上层的异步通知，而不是上层调用设备的操作接口。

下一份可继续阅读 `components/drivers/include/rtdevice.h`：它按配置汇集串口、I2C、SPI、
ADC 等专用驱动头文件，说明设备框架如何向各类别扩展。
