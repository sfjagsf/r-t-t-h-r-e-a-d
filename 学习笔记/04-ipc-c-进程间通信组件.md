# `ipc.c`：RT-Thread IPC 组件学习笔记

> 本笔记对应 `src/ipc.c`。IPC（Inter-Process Communication）在 RT-Thread 的语境中更常表示**线程间通信与同步**：信号量、互斥锁、事件、邮箱、消息队列。

## 1. 总体对象关系

这些 IPC 组件均采用“结构体嵌入”模拟继承：

```text
rt_semaphore / rt_mutex / rt_event / rt_mailbox / rt_messagequeue
    └─ rt_ipc_object parent
         └─ rt_object parent
```

因此每个 IPC 对象都具备 `rt_object` 的名称、类型、静态/动态标记和对象链表节点，并能被统一对象系统管理。

`rt_ipc_object` 还提供：

```text
suspend_thread：等待“获取该 IPC 对象”的线程链表
```

`_ipc_object_init()` 做的核心事情就是初始化这个链表头为空双向循环链表。所有**链表头**使用前都必须初始化；结构体里的 `list` 节点本身不是独立链表头。

## 2. 所有 IPC 组件共用的等待模式

```text
资源/数据暂不可用
    ↓
当前线程写入自己的等待信息
    ↓
rt_thread_suspend_to_list(...)
    ↓
线程加入相应等待链表，状态变为挂起
    ↓
若 timeout > 0，启动该线程自己的 thread_timer
    ↓
rt_schedule() 切换到其他就绪线程
    ↓
释放者/发送者/超时处理唤醒该线程
```

等待链表可按对象创建时的 `flag` 排队：

```text
RT_IPC_FLAG_FIFO：先等待者先被唤醒
RT_IPC_FLAG_PRIO：优先级数值更小（优先级更高）的线程先被唤醒
```

`rt_susp_list_enqueue()` 负责入队；`rt_susp_list_dequeue()` 从队头取一个线程、停止其超时定时器、把它放回就绪队列；`rt_susp_list_resume_all()` 取消等待并唤醒全部等待者。

## 3. 信号量（Semaphore）

信号量保存一个计数：

```text
value > 0：可获取的资源数量
value == 0：获取者需要等待
```

```text
rt_sem_take()：value > 0 时减一；否则等待/超时
rt_sem_release()：若已有等待线程，直接唤醒一个；否则 value 加一
```

释放时优先唤醒等待者、而非先增加 `value`，避免资源先变为“公开可抢”、又被非等待线程抢走的竞争。

但这**不是优先级反转的解决方案**。`RT_IPC_FLAG_PRIO` 只决定“哪个等待线程优先被唤醒”；优先级反转需要互斥锁的优先级继承机制处理。

`rt_sem_control(..., RT_IPC_CMD_RESET, ...)` 会唤醒全部等待者并返回错误，再重置信号量状态。

## 4. 互斥锁（Mutex）

互斥锁用于保护临界资源，关键状态包括：

```text
owner       当前持有锁的线程
hold        递归持有次数
priority    等待此锁的最高优先级（数值最小）
taken_list 该锁被挂入 owner 的“已持有锁链表”所用节点
```

特点：

```text
同一 owner 可重复 take（hold++）
只有 hold 减到 0 才是真正释放
创建/初始化时强制使用优先级等待队列
```

### 优先级继承（PI）

```text
低优先级线程 L 持有 mutex
高优先级线程 H 等待 mutex
    ↓
L 临时继承 H 的更高优先级
    ↓
L 尽快运行并释放 mutex
    ↓
L 根据自己仍持有的其他 mutex 和基础优先级恢复/重新计算优先级
```

RT-Thread 会处理嵌套锁等待场景：若锁的 owner 又阻塞在另一把 mutex 上，优先级变化可继续沿 owner 链传播。

`rt_mutex_setprioceiling()` 设置每把锁的优先级上限；它与“等待者触发的优先级继承”是两个概念。

## 5. 事件（Event）

事件对象的核心状态：

```text
event->set：32 位事件位集合，记录已经发生但可能尚未被消费的事件
thread->event_set：某个等待线程期望/实际得到的事件位
thread->event_info：AND / OR / CLEAR 接收规则
```

### 发送：`rt_event_send(event, set)`

```c
event->set |= set;
```

事件位是累积状态，不是一条消息。发送后会检查所有等待线程：

```text
RT_EVENT_FLAG_AND：请求的全部位均出现才成功
RT_EVENT_FLAG_OR ：请求的位中任意一位出现就成功
```

一次事件发送可能唤醒多个满足条件的线程。

若线程请求 `RT_EVENT_FLAG_CLEAR`，源码会先遍历全部等待者、汇总应清除的位，最后统一清除。这样可保证同一次 `send` 中其他同样满足条件的等待线程不会因为“前一个线程过早清位”而错过事件。

### 接收：`rt_event_recv()`

```text
事件条件已满足：立即成功，recved 返回实际匹配位
条件不满足且 timeout == 0：返回 -RT_ETIMEOUT
条件不满足且可等待：当前线程进入 event->parent.suspend_thread
```

若指定 `RT_EVENT_FLAG_CLEAR`，成功接收后对应事件位会被清除。

三个接收接口的差异只在等待时的挂起属性：

```text
rt_event_recv()                不可中断等待
rt_event_recv_interruptible()  可中断等待
rt_event_recv_killable()       可终止等待
```

`rt_event_control(..., RT_IPC_CMD_RESET, ...)` 清除事件位，并以错误结果唤醒所有等待者。

## 6. 邮箱（Mailbox）

邮箱保存的是 `rt_ubase_t`：通常为一个机器字（32 位 MCU 上常为 4 字节）。适合传递：

```text
整数、事件值、枚举、指针
```

它不复制指针指向的数据；例如传结构体地址时，结构体本身的生命周期和并发保护仍由应用负责。

### 环形缓冲区状态

```text
msg_pool     消息数组
size         容量
entry        当前消息数量
in_offset    下一个写入位置
out_offset   下一个读取位置
```

```text
普通 send：在 in_offset 写入，in_offset 前进
recv：从 out_offset 读取，out_offset 前进
到数组末尾则回绕到 0
```

邮箱有两条等待链表：

```text
mb->parent.suspend_thread：邮箱空，等待接收消息的线程
mb->suspend_sender_thread：邮箱满，等待发送空位的线程
```

### 常用操作

```text
rt_mb_send()：非阻塞发送；邮箱满返回 -RT_EFULL
rt_mb_send_wait()：邮箱满时可等待
rt_mb_urgent()：非阻塞紧急发送，将消息放到队头
rt_mb_recv()：接收消息；邮箱空时可等待
```

普通消息按 FIFO 进入队尾；`rt_mb_urgent()` 将一条消息插入队头。它仅提供“普通/紧急”两档，不是完整多等级消息优先级队列。

`RT_IPC_FLAG_FIFO/PRIO` 排的是**等待线程**，不是邮箱里消息的优先级。

接收成功会腾出一个槽位，因此唤醒一个等待发送者；发送成功增加一条消息，因此唤醒一个等待接收者。

## 7. 消息队列（MessageQueue）

消息队列传递的是固定最大长度的数据**副本**：

```c
rt_mq_send(mq, &data, sizeof(data));
```

适合在线程间传递结构体内容。它与邮箱对比：

```text
邮箱：传一个机器字；常用于整数或指针
消息队列：复制 size 字节；常用于结构体和数据块
```

### 节点池与两条消息链表

用户/内核提供的消息池被预先分割成固定节点：

```text
[struct rt_mq_message 节点头 | 对齐后的用户数据区]
```

重要成员：

```text
msg_queue_free：空闲节点链表
msg_queue_head：已发送、等待接收的消息链表头
msg_queue_tail：已发送消息链表尾
entry：当前消息数量
```

发送时：

```text
从 msg_queue_free 取节点
→ memcpy 用户数据到节点数据区
→ 节点加入已发送消息链表
```

接收时：

```text
从 msg_queue_head 取节点
→ memcpy 节点数据给用户 buffer
→ 节点归还 msg_queue_free
```

消息队列也有接收、发送两条线程等待链表：

```text
mq->parent.suspend_thread：队列空，等待接收
mq->suspend_sender_thread：空闲节点用尽，等待发送
```

### 发送与接收

```text
rt_mq_send()：非阻塞；队列满返回 -RT_EFULL
rt_mq_send_wait()：队列满时可等待
rt_mq_urgent()：非阻塞紧急发送，节点头插
rt_mq_recv()：接收消息，成功返回实际复制的字节数
```

普通发送尾插，因此是 FIFO；紧急消息头插。大量紧急消息会让普通消息长期排后，可能造成饥饿。

接收缓冲区若小于消息实际长度，`rt_mq_recv()` 只复制缓冲区容纳的长度，剩余数据会被丢弃；因此通常让 `buffer` 至少等于消息队列的 `msg_size`。

若编译开启 `RT_USING_MESSAGEQUEUE_PRIORITY`，消息本身还可按 `prio` 排序（数值更大优先级更高）。这与 `RT_IPC_FLAG_PRIO` 的“等待线程排序”不同。

### Reset / delete / detach

```text
control RESET：取消全部等待者、丢弃所有已发送消息，但节点归还空闲池；队列仍可用
detach：静态对象从对象系统移除，不释放用户提供的内存
delete：动态对象，先取消等待者，再释放消息池与对象本体
```

## 8. 组件选择速记

```text
只需要通知/计数资源数量        → 信号量
需要独占访问共享资源            → 互斥锁
需要等待多个布尔条件组合        → 事件
传整数、状态值或指针            → 邮箱
传结构体或固定长度数据副本      → 消息队列
```

## 9. 贯穿 `ipc.c` 的设计模式

```text
对象：每种 IPC 都嵌入 rt_object，可统一注册、查找、监控
接口：init/create、detach/delete、take/send/recv、control 等 API
组件：信号量、互斥锁、事件、邮箱、消息队列共享等待、超时、唤醒框架
```

这就是 RT-Thread 用 C 组织“对象 + 接口 + 组件”的典型例子：公共机制复用，组件只保存各自独特的数据结构与业务规则。
