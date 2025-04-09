#include "CentralCache.h"
#include "PageCache.h"
CentralCache CentralCache::_sInst;

// 获取⼀个非空的 span
Span *CentralCache::GetOneSpan(SpanList &list, size_t size)
{
	// 查看当前的 spanlist 是否还有未分配对象的 Span
	Span *it = list.Begin();
	while (it != list.End())
	{
		if (it->_freeList != nullptr)
		{
			return it;
		}
		else
			it = it->_next;
	}

	// 走到这里说明没有空闲 Span 了，找 page cache 要

	// 先把 central cache 的桶锁解开，防止其他线程释放对象时阻塞
	list._mtx.unlock();

	PageCache::GetInstance()->_pageMtx.lock();
	Span *span = PageCache::GetInstance()->NewSpan(SizeClass::NumMovePage(size));
	span->_isUse = true;
	span->_objSize = size;
	PageCache::GetInstance()->_pageMtx.unlock();

	// 计算 Span 大块内存的起始地址和大小
	// 以下操作不需要加锁，因为目前其他线程访问不到该Span
	char *start = (char *)(span->_pageId << PAGE_SHIFT);
	size_t bytes = span->_n << PAGE_SHIFT;
	char *end = start + bytes;
	// 把大块内存切成自由链表链接起来
	span->_freeList = start;
	start += size;
	void *tail = span->_freeList;
	int i = 1;
	while (start < end)
	{
		++i;
		NextObj(tail) = start;
		tail = NextObj(tail);
		start += size;
	}

	NextObj(tail) = nullptr;

	// 将切好的 Span 挂到桶里面时需要加锁
	list._mtx.lock();
	list.PushFront(span);

	return span;
}

// 从central cache获取一定数量的内存对象给thread cache
size_t CentralCache::FetchRangeObj(void *&start, void *&end, size_t batchNum, size_t size)
{
	size_t index = SizeClass::Index(size);
	_spanLists[index]._mtx.lock();

	Span *span = GetOneSpan(_spanLists[index], size);
	assert(span);
	assert(span->_freeList);

	// 从 span 中获取 batchNum 个对象
	// 如果不够，有多少拿多少
	start = span->_freeList;
	end = start;

	size_t i = 0;
	size_t actualNum = 1;
	while (i < batchNum - 1 && NextObj(end) != nullptr)
	{
		end = NextObj(end);
		++i;
		++actualNum;
	}

	span->_freeList = NextObj(end);
	NextObj(end) = nullptr;
	span->_useCount += actualNum;

	_spanLists[index]._mtx.unlock();
	return actualNum;
}

void CentralCache::ReleaseListToSpans(void *start, size_t size)
{
	size_t index = SizeClass::Index(size);

	_spanLists[index]._mtx.lock();

	while (start)
	{
		void *next = NextObj(start);
		Span *span = PageCache::GetInstance()->MapObjectToSpan(start);
		NextObj(start) = span->_freeList;
		span->_freeList = start;

		span->_useCount--;

		if (span->_useCount == 0)
		{
			// 说明 Span 切分出去的所有小块内存都已归还
			// 此时该 Span 可以再向 page cache 归还并尝试相邻页的合并
			_spanLists[index].Erase(span);
			span->_freeList = nullptr;
			span->_next = nullptr;
			span->_prev = nullptr;
			// 释放 Span 给 page cache 时，使用 page cache 的锁即可
			// 暂时把桶锁解掉
			_spanLists[index]._mtx.unlock();

			PageCache::GetInstance()->_pageMtx.lock();
			PageCache::GetInstance()->ReleaseSpanToPageCache(span);
			PageCache::GetInstance()->_pageMtx.unlock();

			_spanLists[index]._mtx.lock();
		}

		start = next;
	}

	_spanLists[index]._mtx.unlock();
}