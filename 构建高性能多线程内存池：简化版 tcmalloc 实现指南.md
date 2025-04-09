# 构建高性能多线程内存池：简化版 tcmalloc 实现指南

## 引言

在高并发应用中，频繁的小块内存申请与释放不仅会带来性能瓶颈，还容易导致内存碎片问题。为此，内存池技术应运而生，而 `tcmalloc`（Thread-Caching Malloc）作为 Google 开源的高性能内存分配器，是学习与借鉴的优秀模板。本文将以简化版 `tcmalloc` 为目标，从零手把手带你构建一个支持多线程的高性能内存池。

本项目将涉及到C/C++、数据结构（如链表、哈希桶）、操作系统内存管理、单例模式、多线程、互斥锁等技术。如果你对这些概念还不熟悉，建议先学习这些相关知识，确保理解这些基础知识后再深入本篇内容。

## 01 内存池技术概览

池化技术是一种资源管理策略，它的基本思想是程序预先申请超出当前需求的资源，然后自主管理这些资源，以备未来使用。

那么，什么是“超出需求的资源”呢？简单来说，就是那些当前不立即使用，但可以储备以备将来使用的资源。

让我们通过一个日常生活的例子来理解这个概念。

```txt
想象你住在宿舍楼，楼道里有饮水机。假如你只有一个杯子，每次想喝水都得跑到饮水机那里接水，这样既麻烦又浪费时间。如果为了提高效率，你决定买一个水壶，每次接水时带上水壶，这样可以一次性装满水，回到宿舍后直接从水壶里倒水喝。当水喝完后，再去饮水机接水。显然，这种方式比每次都去接水要高效得多。这其实就是一种池化技术。
```

在这个例子中，提前准备好水壶，就像是向系统申请了过量的资源，接水时直接从已申请的水壶中取水，而不是每次都去申请新的资源。

在技术领域中，内存池的工作原理也类似。程序首先向系统申请一块较大的内存空间，而不是每次需要内存时都向操作系统请求。之后，程序会在这块已申请的内存中进行分配和回收，而不再频繁地请求系统资源。

除了内存池，常见的池化技术还包括：

1. **连接池**：例如数据库连接池。每次与数据库建立连接都是一个耗时的操作，如果每次仅执行一个简单的SQL语句后就关闭连接，会浪费大量时间。因此，连接池通常会预先创建多个连接，执行SQL时从池中获取一个空闲连接，执行完后不关闭连接，继续等待下一次使用。这样可以避免频繁的连接和断开操作。
2. **线程池**：线程池的思想是预先创建一定数量的线程，并使它们处于休眠状态。当有客户端请求时，唤醒一个空闲线程来处理该请求。处理完毕后，线程进入休眠状态，等待下一个请求。在我的前一篇博客中，我也演示了如何实现一个简单的线程池，感兴趣的朋友可以参考。

池化技术的核心思想在于提前申请并重复使用资源，从而减少重复创建资源的开销，提升效率。不同类型的池在实现细节上可能有所不同，但其根本思想是相似的。

## 02 项目整体架构

我们借鉴 `tcmalloc` 的三层架构，设计如下模块：![](https://raw.githubusercontent.com/Kutbas/GraphBed/main/Typora/202504091649278.png)

**● Thread Cache**

每个线程拥有独立的线程缓存，避免加锁。只要申请的内存不超过 256KB，线程会优先从自身的 `Thread Cache` 获取空间，其内部使用**哈希桶+自由链表**存储不同大小的内存块。

**● Central Cache**

线程缓存不够时，从 Central Cache 请求空间。

**● Page Cache**

当 Central Cache 空间不足或释放空间过多时，向 Page Cache 请求或归还内存。Page Cache 以页为单位组织内存，负责整合碎片，进行 Span 的合并与拆分。

## 03 定长内存池：快速入门

在学习高并发内存池之前，我们先来热个身，手动实现一个**定长内存池**，帮助大家建立对“内存池”的基本认识。

我们知道 `malloc` 虽然万能，但为了应对各种情况，设计得相对复杂，在高频率、小对象申请释放的场景下，效率可能不理想。而在一些场景中，我们只需要申请和释放**固定大小**的内存块，这时就可以考虑定长内存池这种更高效、更轻量的解决方案。

这个定长内存池的原理其实不复杂：

- 预先申请一大块内存，把它切成一小块一小块的用；
- 用**自由链表**把这些小块管理起来，实现空间的重复利用；
- 如果用完了，再申请一块新的接着用；
- 每次回收时，就把小块重新挂到自由链表上。

话不多说。

## 04 Thread Cache 初步实现

我们先简单回顾下之前说的定长内存池：

在定长内存池中，有一个 `_freelist`，也就是一个自由链表，用来管理某种固定大小的小块内存。比如我们要频繁分配一个对象，那这个 `_freelist` 就可以专门为这个对象提供固定长度的内存块。

但线程申请内存时，需求就没这么“整齐”了——可能要3B、6B、18K、236K……这时候，当然不能只用一个 `_freelist`，需要为不同大小的内存块设置对应的 `_freelist` 来做管理。

不过也不能太极端。比如说最大申请256KB，如果每个字节都建一个 `_freelist`，那就是二十多万个，没必要也没意义。

所以我们要在空间粒度和管理开销之间做个平衡：比如，第一个 `_freelist` 管8B的块，第二个管16B的，再往下是24B、32B……直到256KB。虽然听上去像是每次增加8B，其实真实实现里会有更复杂的对齐策略，这里先不展开。

那内存怎么分配呢？比如线程申请5B的内存，我们就给它一个8B的块；要10B，就给16B。虽然可能“多给了一点”，但这比“给不够”要好得多。可以把它想象成你找爸妈要300块买衣服，结果爸妈豪气地给了你500，开心！但如果只给你50，让你去地摊凑合，那体验可就不太好了。

这种“多给”就带来了内碎片。就是说实际用的空间没那么大，但因为对齐分配，浪费了一点。而之前讲的外碎片，是指空间虽然多但不连续，申请不到大块。

线程要申请内存时，其实是向 `ThreadCache` 请求。`ThreadCache` 内部本质上是一个哈希桶结构，每个桶就是一个 `_freelist`，比如第0个桶管8B、第1个管16B……到最大256KB。找内存时，就看对应大小的桶里有没有空闲块，有就用；没有就从更高一层的 `CentralCache` 拿。

由于要完整实现 `ThreadCache` 需要涉及到和 `CentralCache` 的交互，不好各自独立实现，所以我们先对 `ThreadCache` 进行初步实现。

### 04.1 基础结构设计

```c++
class ThreadCache
{
    public:
    // 申请和释放内存对象
    void *Allocate(size_t size);
    void Deallocate(void *ptr, size_t size);
    // 从中心缓存获取对象
    void *FetchFromCentralCache(size_t index, size_t size);
    // 自由链表过长时，回收内存至 central cache
    void ListTooLong(FreeList &list, size_t size);

    private:
    FreeList _freeLists[NFREELIST];
};

// TLS thread local storage
static _declspec(thread) ThreadCache *pTLSThreadCache = nullptr;
```

在  `ThreadCache`  类中，`FreeList` 类就是我们管理空闲内存块的核心结构，正如前面所说的，它虽然叫`FreeList`，但实际上是一个哈希桶，桶内不需要额外的链表节点，内存块自己就是节点。它的结构如下：

```c++
class FreeList
{
    public:
    void Push(void *obj);
    void PushRange(void *start, void *end, size_t n);
    void PopRange(void *&start, void *&end, size_t n);
    void *Pop();
    bool Empty();
    size_t &MaxSize();
    size_t Size();

    private:
    void *_freeList = nullptr;
    size_t _maxSize = 1;
    size_t _size = 0;
};
```

目前我们只需要知道的是，`FreeList` 类提供两个操作：`Push` 把块放回去，`Pop` 从链表取出一块。

```C++
void Push(void *obj)
{
    // 头插
    //*(void**)obj = _freeList;
    NextObj(obj) = _freeList;
    _freeList = obj;
    ++_size;
}

void PushRange(void *start, void *end, size_t n)
{
    NextObj(end) = _freeList;
    _freeList = start;
    _size += n;
}
```

为了让代码更清晰，我们封装了 `ObjNext()` 方法来获取或设置某个内存块的“下一个指针”位置，用 `static` 修饰，避免多次包含时报错。

```C++
static void *&NextObj(void *obj)
{
    return *(void **)obj;
}
```

### 04.2 哈希桶设计与对齐规则

我们知道哈希表每个桶对应一个 `_freelist`，关键是：桶要设多少个？我们不能每8B就搞一个桶，还是太多。

于是我们参考 `tcmalloc` 的做法，按申请大小分多个区间：

- [1, 128] 对齐到8B
- [129, 1024] 对齐到16B
- [1025, 8K] 对齐到128B
- [8K+1, 64K] 对齐到1024B
- [64K+1, 256K] 对齐到8KB

这样设计下来，最多只需要 **208个桶**，空间浪费也被控制在10%左右。浪费最多的点通常是每个区间的起始，比如 1B、129B、1025B，这些对齐后会有一定冗余，但相对于分配效率，这点浪费可以接受。

### 04.3 SizeClass：对齐与桶映射工具类

为了计算每次申请要对齐到多少字节、落在哪个桶里，我们专门写一个 `SizeClass` 类，内部的方法如下所示：

```c++
class SizeClass
{
    public:
    static inline size_t _RoundUp(size_t bytes, size_t align)
    {
        /*size_t alignSize;
		if (bytes % align != 0)
			alignSize = (bytes / align + 1) * align;
		else
			alignSize = bytes;
		return alignSize;*/
        return ((bytes + align - 1) & ~(align - 1));
    }

    static inline size_t RoundUp(size_t size)
    {
        if (size <= 128)
            return _RoundUp(size, 8);
        else if (size <= 1024)
            return _RoundUp(size, 16);
        else if (size <= 8 * 1024)
            return _RoundUp(size, 128);
        else if (size <= 64 * 1024)
            return _RoundUp(size, 1024);
        else if (size <= 256 * 1024)
            return _RoundUp(size, 8 * 1024);
        else
            return _RoundUp(size, 1 << PAGE_SHIFT);
    }

    static inline size_t _Index(size_t bytes, size_t align_shift)
    {
        /*if (bytes % align == 0)
			return bytes / align - 1;
		else
			return bytes / align;*/
        return ((bytes + (1 << align_shift) - 1) >> align_shift) - 1;
    }

    // 计算映射的哪⼀个⾃由链表桶
    static inline size_t Index(size_t bytes)
    {
        assert(bytes <= MAX_BYTES);

        // 每个区间有多少个链
        static int group_array[4] = {16, 56, 56, 56};

        if (bytes <= 128)
            return _Index(bytes, 3);
        else if (bytes <= 1024)
            return _Index(bytes - 128, 4) + group_array[0];
        else if (bytes <= 8 * 1024)
            return _Index(bytes - 1024, 7) + group_array[1] + group_array[0];
        else if (bytes <= 64 * 1024)
            return _Index(bytes - 8 * 1024, 10) + group_array[2] +
            group_array[1] + group_array[0];
        else if (bytes <= 256 * 1024)
            return _Index(bytes - 64 * 1024, 13) + group_array[3] +
            group_array[2] + group_array[1] + group_array[0];

        else
        {
            assert(false);
            return -1;
        }
    }
};
```

其中有两个供外部调用的静态方法：

- `RoundUp(size)`：给定原始大小，算出应该对齐到多少字节。
- `Index(size)`：算出该大小对应哈希表中的哪个下标。

这里的 `Index` 方法会根据 size 所属的区间，结合对齐规则，精准计算它在桶数组中的位置。内部还会用到一个辅助方法 `_Index()`，这个方法可以用位运算优化，效率更高。

### 04.4 ThreadCache::Allocate 的实现

`ThreadCache::Allocate` 是管理每个 `ThreadCache` 中内存分配的核心函数。它的主要任务是根据线程请求的内存大小从相应的哈希桶中获取空闲内存，或者在没有足够的空间时向`CentralCache`请求新的内存块。在经过前面的铺垫之后下面来拆解它的实现思路。

1. **计算对齐后的字节数和哈希桶索引**。当线程请求一定大小的内存时，首先需要根据请求的字节数来确定实际分配的内存大小。这一步我们知道是通过 `RoundUp(size)` 和 `Index(size)` 两个函数来实现。
2. **查找哈希桶并分配内存**。一旦计算出需要对齐后的字节数和桶索引，`ThreadCache::Allocate` 就会尝试从哈希桶中对应的自由链表中获取内存块。这会涉及到两个过程：
   - 如果哈希桶中有空闲内存块（即该桶对应的自由链表非空），则调用 `Pop()` 方法从该链表中取出一个内存块返回给线程。
   - 如果哈希桶中没有足够的内存块（即链表为空），则需要向 `CentralCache` 请求内存。这时，`ThreadCache` 会通过 `FetchFromCentralCache()` 方法从中央缓存申请内存。
3. **向中央缓存请求内存**。如果线程请求的内存无法从当前的 `ThreadCache` 获取到，`ThreadCache::Allocate` 就会将请求转发到 `CentralCache` 。

基于以上实现思路，下面给出`ThreadCache::Allocate` 的代码：

```c++
void* ThreadCache::Allocate(size_t size)
{
    assert(size <= MAX_BYTES);
    size_t alignSize = SizeClass::RoundUp(size);
    size_t index = SizeClass::Index(size);

    if (!_freeLists[index].Empty())
        return _freeLists[index].Pop();
    else
        return FetchFromCentralCache(index, alignSize);
}
```

以上，我们便初步实现了 `ThreadCache`。下面我们进入到 `CentralCache` 的实现。

## 05 Central Cache 初步实现

现在我们知道，当 `ThreadCache` 的空间不足时，会向 `CentralCache` 请求更多的空间。在`ThreadCache`的实现中，曾留有一个接口 `FetchFromCentralCache`，这个接口的具体实现会在我们讨论完`CentralCache`的细节后完成。

### 05.1 ThreadCache 与 CentralCache 的关系

实际上，`ThreadCache`  和 `CentralCache` 有很多相似之处，比较明显的是 `CentralCache` 也采用了哈希结构来存储数据，并且同样是根据块的大小来进行映射。两者的映射规则一致带来了许多便利，比如当 `ThreadCache` 对应的哈希桶没有足够的空间时，可以直接向 `CentralCache` 的相同哈希桶请求空间。这样一来，两个系统中的哈希桶实现一一对应，查找操作也变得更加高效。



### 05.2 内存分配的线程安全

`ThreadCache`  和 `CentralCache` 之间的区别在于，线程可以自由地向 `ThreadCache` 申请空间，无需加锁，因为每个线程都拥有自己独立的 `ThreadCache`。而当多个 `ThreadCache` 的同一位置的哈希桶空间不足时，它们会并发地向 `CentralCache` 申请空间，此时 `CentralCache` 只有一个实例，这就需要加锁。举个例子，如果线程 `t1` 和 `t2` 分别向各自的 `ThreadCache` 2号桶（24B）申请空间，并且这两个桶都为空，那么它们就会同时向 `CentralCache` 的2号桶请求空间。在这种情况下，由于多个线程竞争同一资源，我们需要加锁，以确保不会发生并发冲突。

### 05.3 Span 结构与内存管理

### 05.4 内存分配与回收机制

## 06 Page Cache 实现

