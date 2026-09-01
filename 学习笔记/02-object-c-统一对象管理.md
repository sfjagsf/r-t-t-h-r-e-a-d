# RT-Thread `object.c` 学习笔记

## 文件定位

`src/object.c` 实现 RT-Thread 的统一内核对象管理。它不实现线程调度、信号量计数或设备读写；它负责对象的分类登记、创建、查找、遍历与销毁。

```text
已有内存的静态对象：init → 登记 → detach
堆创建的动态对象：allocate → 登记 → delete
```

公开 API 声明集中在 `include/rtthread.h`，基础结构定义在 `include/rtdef.h`；本文件没有单独的 `object.h`。

## 一、`rt_custom_object`：对象模型可扩展

```c
struct rt_custom_object
{
    struct rt_object parent;
    rt_err_t (*destroy)(void *);
    void *data;
};
```

这只是定义新**类型**，不会立刻创建变量或占 RAM。实际调用 `rt_custom_object_create()` 才会动态申请对象。

`parent` 是完整的 `struct rt_object` 成员，不是指针：

```text
rt_custom_object
├─ [完整 rt_object 内存]
├─ destroy
└─ data
```

不能写成 `rt_object_t parent`，因为：

```c
typedef struct rt_object *rt_object_t;
```

`rt_object_t parent` 会是一个指针成员，指向另一块独立的对象内存，破坏“首成员嵌入”的继承式布局。

## 二、对象类别登记表 `_object_container[]`

```c
static struct rt_object_information
_object_container[RT_Object_Info_Unknown] =
{
    {RT_Object_Class_Thread, ..., sizeof(struct rt_thread), ...},
    {RT_Object_Class_Semaphore, ..., sizeof(struct rt_semaphore), ...},
    ...
};
```

它不是在创建具体线程或具体信号量，而是在为每个已启用的**对象类别**建立管理容器：

```text
Semaphore 类容器
├─ 类别：Semaphore
├─ object_list：以后所有信号量对象会挂在这里
├─ object_size：sizeof(struct rt_semaphore)
└─ spinlock：保护该信号量总链表
```

当启用 `RT_USING_SEMAPHORE` 时，信号量这一类容器会被编译进数组；真正调用 `rt_sem_init()` 或 `rt_sem_create()` 后，具体 `rt_sem` 才会进入其 `object_list`。

### 两组枚举的区别

```text
RT_Object_Class_xxx
  公开对象类别，存入 object->type，数值稳定。

RT_Object_Info_xxx
  object.c 内部数组下标；受配置宏影响，会变化。
```

`RT_Object_Info_Unknown` 放在内部枚举最后，作为 `_object_container[]` 的长度上界，不代表有效登记册。

### 每类对象自己的总链表

```text
Thread.object_list      → 所有线程
Semaphore.object_list   → 所有信号量
Mutex.object_list       → 所有互斥锁
Event.object_list       → 所有事件
Device.object_list      → 所有设备
Timer.object_list       → 所有定时器
```

访问数组项先得到的是“该类别对象总链表的入口”，而不是某个具体对象。遍历节点后再用：

```c
rt_list_entry(node, struct rt_object, list)
```

从对象内嵌的 `list` 节点反推出完整对象地址。

## 三、`rt_object_get_information()`

```c
struct rt_object_information *
rt_object_get_information(enum rt_object_class_type type)
```

输入对象类别，返回对应类别管理容器的指针。

函数先执行：

```c
type &= ~RT_Object_Class_Static;
```

例如静态线程的 `type` 是 `Thread | Static = 0x81`，去掉 `0x80` 后恢复为 `Thread = 0x01`，即可找到线程类别登记册。

不能直接写 `_object_container[type]`，因为对象类别编号不连续，且内部数组下标会随组件开关变化；函数必须遍历并比较每项的 `.type`。

## 四、对象查询

### `rt_object_get_length(type)`

找到类别容器后，锁住其 `spinlock` 并遍历 `object_list` 统计节点数量。它回答“当前有多少个该类别对象”。

### `rt_object_get_pointers(type, pointers, maxlen)`

将目标类别中至多 `maxlen` 个对象地址复制到调用者数组：

```c
rt_object_t objects[8];
int n = rt_object_get_pointers(RT_Object_Class_Thread, objects, 8);
```

遍历时加锁，因此复制过程安全；函数返回后锁释放，动态对象仍可能被其他线程删除，返回的指针适合立即检查，不应长期无保护保存。

### `rt_object_for_each(type, iter, data)`

对类别链表中每个对象调用：

```c
rt_err_t iter(rt_object_t object, void *data);
```

返回 `RT_EOK` 继续；返回正数正常提前停止；返回负数按错误结束。回调执行时链表锁仍被持有，回调应短小且避免修改同一类别链表。

### `rt_object_find(name, type)`

内部基于 `rt_object_for_each()` 线性比较名称，并返回第一个匹配对象。名称规范要求同类对象唯一，但该函数本身不是强制的重名检测机制。

## 五、静态对象生命周期

### `rt_object_init(object, type, name)`

适用于调用者提供内存的对象，如：

```c
static struct rt_thread worker;
```

主要步骤：

```text
找到类别容器
→ object->type = type | Static
→ 写入对象名称
→ 调用 attach Hook（若启用）
→ 加锁，将 object->list 插入类别 object_list
```

名称超过 `RT_NAME_MAX - 1` 时会记录日志并截断，最后强制补 `\0`。

### `rt_object_detach(object)`

静态对象的反向操作：

```text
调用 detach Hook
→ 找到对象所属类别容器
→ 加锁，从 object_list 摘除 object->list
→ object->type = Null
→ 不释放内存
```

通常应使用外层 API（如 `rt_timer_detach()`），让具体组件先清理自己的资源，再调用通用 `rt_object_detach()`。

## 六、动态对象生命周期

### `rt_object_allocate(type, name)`

仅在 `RT_USING_HEAP` 时存在，且不能在中断上下文调用。

主要步骤：

```text
找到类别容器
→ RT_KERNEL_MALLOC(information->object_size)
→ 将完整对象内存清零
→ object->type = type（不带 Static 位）
→ object->flag = 0，写入名称
→ 调用 attach Hook
→ 加锁，挂入类别 object_list
→ 返回对象地址
```

例如传入 `Thread` 时，实际申请 `sizeof(struct rt_thread)`；传入 `Semaphore` 时申请 `sizeof(struct rt_semaphore)`。

### `rt_object_delete(object)`

动态对象的反向操作：

```text
断言对象不带 Static 位
→ 调用 detach Hook
→ 从类别 object_list 摘除
→ object->type = Null
→ RT_KERNEL_FREE(object)
```

配对规则：

```text
静态：rt_xxx_init()   ↔ rt_xxx_detach()
动态：rt_xxx_create() ↔ rt_xxx_delete()
```

## 七、Hook：注册与真正触发

`rt_object_xxx_sethook()` 只做函数指针赋值：

```c
rt_object_attach_hook = hook;
```

真正触发发生在源码预埋的：

```c
RT_OBJECT_HOOK_CALL(rt_object_xxx_hook, (object));
```

启用函数指针 Hook 时可概念性展开为：

```c
if (rt_object_xxx_hook != RT_NULL)
    rt_object_xxx_hook(object);
```

五类事件：

```text
attach：对象即将加入对象系统
detach：对象即将离开对象系统
trytake：尝试从 IPC 对象取得资源
take：成功取得资源；定时器中表示启动
put：向 IPC 对象放入/归还资源
```

`trytake` 不代表成功；`take` 才表示成功。单函数指针 Hook 每个事件点只能注册一个回调，后注册会覆盖先注册；若需多个回调，应使用 Hook List 机制。

## 八、`RTM_EXPORT()`

`RTM_EXPORT` 不是 C 关键字，而是 RT-Thread 宏。启用 `RT_USING_MODULE` 时：

```c
RTM_EXPORT(rt_object_get_pointers);
```

为符号建立“名称 → 地址”表项并放入 `RTMSymTab` 链接段：

```text
"rt_object_get_pointers" → &rt_object_get_pointers
```

函数机器码仍在普通代码段，不会移动到所谓 `RTM_EXPORT` 地址段。动态模块加载时通过名字查询 `RTMSymTab`，得到函数或全局变量的实际地址并完成重定位。

未启用动态模块时，`RTM_EXPORT(symbol)` 展开为空。

## 建议复习问题

1. `_object_container[]` 管理的是具体对象，还是对象类别？
2. 为什么静态对象的 `type` 要带 `Static = 0x80`？
3. 为什么静态对象用 `detach`，动态对象用 `delete`？
4. `rt_object_get_pointers()` 为什么只能保证复制过程安全，不能保证返回指针永久有效？
5. `RTM_EXPORT` 与 `INIT_xxx_EXPORT`、`MSH_CMD_EXPORT` 的“注册目的”有什么区别？
