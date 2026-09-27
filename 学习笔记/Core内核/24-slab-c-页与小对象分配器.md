# `slab.c`：页分配与小对象 Slab 分配器

> 对应 `src/slab.c`，仅在 `RT_USING_SLAB` 开启时参与编译。Slab 在底层“连续页分配器”之上，为常见小尺寸请求建立固定 chunk 的 zone，减少频繁小对象申请的搜索成本与外部碎片。

## 1. 与前三种内存管理器的区别

|组件|主要策略|典型优势|
|---|---|---|
|`small mem`|可变块物理链，首次适配|实现紧凑，适合作为默认小堆|
|`memheap`|每 RAM 区一个可变块堆，物理链 + 空闲链|支持多段独立内存区|
|`mempool`|预切固定 block|时间稳定，适合固定大小对象|
|`slab`|页 → zone → 固定 chunk|频繁小对象申请快，按尺寸类别复用|

```text
slab heap
├─ page_list：底层连续空闲页段
├─ memusage[]：每页的归属地图
├─ zone_array[]：各 chunk 尺寸类别中“仍有空闲块”的 zone
└─ zone_free：已完全空闲、暂时缓存的 zone
```

## 2. 三层对象关系

```text
page：通常是 RT_MM_PAGE_SIZE（常见 4 KB）
  ↓
zone：若干页组成，专门服务一种 chunk 大小
  ↓
chunk：最终返回给用户的小块
```

```text
小请求（size < zone_limit）
→ zoneindex() 向上归入某个 chunk 尺寸
→ 从该尺寸的 zone 取得 chunk

大请求（size >= zone_limit）
→ 直接按 page 对齐、申请连续页
```

`zone_limit` 不是固定 16 KB：

```c
slab->zone_limit = slab->zone_size / 4;
if (slab->zone_limit > ZALLOC_ZONE_LIMIT)
    slab->zone_limit = ZALLOC_ZONE_LIMIT;
```

即真实阈值为：

```text
min(zone_size / 4, 16 KB)
```

## 3. 底层连续页分配器

空闲页链的节点直接放在空闲页区域开头：

```c
struct rt_slab_page
{
    struct rt_slab_page *next;
    rt_size_t page;
};
```

```text
page_list
  ↓
[起始页 P0，连续 8 页] → [起始页 P20，连续 3 页] → NULL
```

### `rt_slab_page_alloc()`：切分或整段摘除

```c
if (b->page > npages)
{
    n = b + npages;
    n->next = b->next;
    n->page = b->page - npages;
    *prev = n;
    break;
}

if (b->page == npages)
{
    *prev = b->next;
    break;
}
```

例如 `[P0~P7]` 共 8 页，申请 3 页：返回 `[P0~P2]`，空闲链保留 `[P3~P7]`。

### `rt_slab_page_free()`：按物理地址合并

```c
if (b + b->page == n)
{
    b->page += npages;
    ...
}

if (b == n + npages)
{
    n->page = b->page + npages;
    n->next = b->next;
    *prev = n;
}
```

第一种是新释放页段接在已有空闲段右边；第二种是新释放页段接在已有空闲段左边。两者都把物理连续页合并，降低页级碎片。

## 4. `rt_slab_init()`：建立 slab heap

```c
rt_slab_init("heap", begin_addr, size);
```

流程：

```text
1. 在给定 RAM 前部放置 struct rt_slab。
2. 将后续可用区域按 RT_MM_PAGE_SIZE 对齐。
3. 注册为 RT_Object_Class_Memory，algorithm = "slab"。
4. 用全部可用页初始化 page_list。
5. 计算 zone_size、zone_limit、zone_page_cnt。
6. 从 page_list 再申请页，存放 memusage[]。
```

### zone 大小如何随 heap 调整

```c
slab->zone_size = ZALLOC_MIN_ZONE_SIZE;
while (slab->zone_size < ZALLOC_MAX_ZONE_SIZE &&
       (slab->zone_size << 1) < (limsize / 1024))
{
    slab->zone_size <<= 1;
}
```

zone 在 32 KB 到 128 KB 间按 2 倍增长；heap 较小时不会固定占用巨大的 128 KB zone。

若页为 4 KB、`zone_size = 32 KB`：

```text
zone_page_cnt = 8
zone_limit = 8 KB
```

## 5. `memusage[]`：由用户地址反查所属页/zone

```c
struct rt_slab_memusage
{
    rt_uint32_t type : 2;
    rt_uint32_t size : 30;
};

#define btokup(addr) \
    (&slab->memusage[((rt_uintptr_t)(addr) - slab->heap_start) >>
                      RT_MM_PAGE_BITS])
```

`btokup()` 将地址减去 heap 起点、右移页位数，得到页号，再索引 `memusage[]`。

|`type`|`size` 含义|
|---|---|
|`PAGE_TYPE_FREE`|空闲页，`size` 为 0|
|`PAGE_TYPE_LARGE`|大块分配占用的页数|
|`PAGE_TYPE_SMALL`|当前页距所属 zone 起始页的偏移页数|

`memusage[]` 本身也会从 page_list 中占用若干页；它是 slab 的页归属地图，不是用户内存。

## 6. `zoneindex()`：请求大小映射为 chunk 尺寸

`zoneindex(&size)` 会修改 `size` 为实际 chunk 大小，同时返回 `zone_array[]` 下标。

|原请求范围|实际 chunk 粒度|
|---|---:|
|0–127 B|8 B|
|128–255 B|16 B|
|256–511 B|32 B|
|512–1023 B|64 B|
|1024–2047 B|128 B|
|2048–4095 B|256 B|
|4096–8191 B|512 B|
|8192–16383 B|1024 B|

例：

```text
申请 20 B
→ `(20 + 7) & ~7`
→ 实际 chunk = 24 B
→ 取得 24 B 对应的 zone index
```

`RT_SLAB_NZONES = 72` 表示尺寸类别数；一个类别可有多个 zone：

```text
zone_array[24B 类别] → zone1 → zone2 → ...
```

它只连接仍有空闲 chunk 的 zone。

## 7. 小块申请：`rt_slab_alloc()`

### 现有 zone 有空闲 chunk

```c
if ((z = slab->zone_array[zi]) != RT_NULL)
{
    if (--z->z_nfree == 0)
    {
        slab->zone_array[zi] = z->z_next;
        z->z_next = RT_NULL;
    }

    if (z->z_uindex + 1 != z->z_nmax)
    {
        z->z_uindex = z->z_uindex + 1;
        chunk = (struct rt_slab_chunk *)
                (z->z_baseptr + z->z_uindex * size);
    }
    else
    {
        chunk = z->z_freechunk;
        z->z_freechunk = z->z_freechunk->c_next;
    }
}
```

一个 zone 有两种空闲 chunk 来源：

```text
z_uindex：从未被分配过的 chunk，按地址顺序直接计算。
z_freechunk：已经分配又被释放的 chunk，单向链表弹出。
```

新 zone 不预先把每个 chunk 建链；首次使用时仅通过 `z_baseptr + index × chunk_size` 计算地址，减少初始化成本。

`z_nfree` 变为 0 时，该 zone 已满，从 `zone_array[zi]` 移除；它仍存在，只是不再作为可分配 zone。

### 没有可用 zone：复用或创建

```text
优先从 zone_free 取完全空闲的旧 zone
→ 否则调用 rt_slab_page_alloc() 申请 zone_size 对应的连续页
→ 每一页标记 PAGE_TYPE_SMALL，记录相对 zone 起点的页偏移
→ 初始化 zone 头与第一个 chunk
→ 插入 zone_array[zi]
```

zone 开头先放 `struct rt_slab_zone`，随后 chunk 数据区按规则对齐：2 的幂 chunk 尽量按自身大小对齐，其他 chunk 至少 8 B 对齐。

## 8. 大块申请：整页路径

```c
if (size >= slab->zone_limit)
{
    size = RT_ALIGN(size, RT_MM_PAGE_SIZE);
    chunk = rt_slab_page_alloc(m, size >> RT_MM_PAGE_BITS);
    kup = btokup(chunk);
    kup->type = PAGE_TYPE_LARGE;
    kup->size = size >> RT_MM_PAGE_BITS;
    return chunk;
}
```

大块以页对齐，首页 `memusage` 记录总页数。调用者必须原样保留并释放返回的起始指针，不能传内部地址给 `rt_slab_free()`。

## 9. `rt_slab_free()`：回收大页或小 chunk

### 大块

```c
if (kup->type == PAGE_TYPE_LARGE)
{
    size = kup->size;
    kup->size = 0;
    slab->parent.used -= size * RT_MM_PAGE_SIZE;
    rt_slab_page_free(m, ptr, size);
    return;
}
```

读取首页记录的页数，减少统计，再归还底层页链。

### 小块

```c
z = (struct rt_slab_zone *)
    (((rt_uintptr_t)ptr & ~RT_MM_PAGE_MASK) -
     kup->size * RT_MM_PAGE_SIZE);

chunk = (struct rt_slab_chunk *)ptr;
chunk->c_next = z->z_freechunk;
z->z_freechunk = chunk;
```

`PAGE_TYPE_SMALL` 的 `kup->size` 是页偏移，因此可从任意 chunk 所在页回推 zone 起点。释放后，用户数据前几个字节会被改写成 `c_next`；free 后绝不能继续访问 chunk。

若原来 `z_nfree == 0`：

```c
if (z->z_nfree++ == 0)
{
    z->z_next = slab->zone_array[z->z_zoneindex];
    slab->zone_array[z->z_zoneindex] = z;
}
```

说明 zone 从“满”变为“可分配”，必须重新加入对应的 `zone_array`。

## 10. 完全空 zone 的缓存与归还

当：

```text
z_nfree == z_nmax
```

即所有 chunk 都已空闲时，zone 不会立刻归还页，而是先移入 `zone_free` 缓存。

```text
zone 全空
→ 从 zone_array[zi] 摘除
→ 加入 zone_free
→ zone_free_cnt++
```

当缓存数量超过：

```c
#define ZONE_RELEASE_THRESH 2
```

才会清除对应页的 `PAGE_TYPE_SMALL` 记录并调用：

```c
rt_slab_page_free(m, z, slab->zone_size / RT_MM_PAGE_SIZE);
```

这是一种滞后释放：保留少量空 zone 供后续快速复用，又避免无限囤积页。

## 11. `rt_slab_realloc()`

|情况|行为|
|---|---|
|`ptr == RT_NULL`|等价于 alloc|
|`size == 0`|free 后返回 `RT_NULL`|
|小块新旧请求落在同一 chunk 大小等级|原指针直接返回|
|其他小块变化|申请新 chunk，复制，再释放旧 chunk|
|大块变化|申请新空间，复制 `min(old,new)`，释放旧页块|

slab 的 `realloc` 不像 memheap 那样尝试吞并右侧物理块；它更重视固定 chunk 尺寸与快速分类复用。

## 12. 源码复习问题

1. 为什么 slab 要同时维护 `page_list` 与 `memusage[]`？
2. 为什么 `zone_array[zi]` 不保存已满 zone？
3. `z_uindex` 与 `z_freechunk` 分别管理哪些 chunk？
4. 为什么小块 free 后用户数据首部会失效？
5. 为什么全空 zone 不立刻归还给 page_list？

## 13. 一句话复习

```text
slab = 页分配器提供 zone；zone 为同尺寸 chunk 服务；memusage[] 让 free 从地址反查归属；空 zone 缓存少量后再归还页。
```
