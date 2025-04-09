#include "PageCache.h"

PageCache PageCache::_sInst;

// 获取一个K页的 Span
Span *PageCache::NewSpan(size_t k)
{
	assert(k > 0);

	// 大于128页的直接向堆申请
	if (k > NPAGES - 1)
	{
		void *ptr = SystemAlloc(k);
		// Span* span = new Span;
		Span *span = _spanPool.New();
		span->_pageId = (PAGE_ID)ptr >> PAGE_SHIFT;
		span->_n = k;
		//_idSpanMap[span->_pageId] = span;
		_idSpanMap.set(span->_pageId, span);

		return span;
	}

	// 先检查第K个桶里面有没有 Span
	if (!_spanLists[k].Empty())
	{
		Span *kSpan = _spanLists[k].PopFront();

		// 建立 id 和 Span 的映射，方便 central cache 回收小块内存时查找对应的 Span
		for (PAGE_ID i = 0; i < kSpan->_n; ++i)
		{
			_idSpanMap.set(kSpan->_pageId + i, kSpan);
		}
		//_idSpanMap[kSpan->_pageId + i] = kSpan;

		return kSpan;
	}

	// 否则检查后面的桶有没有 Span
	for (size_t i = k + 1; i < NPAGES; i++)
	{
		if (!_spanLists[i].Empty())
		{
			// 切分成一个K页的 Span 和一个 n-k 页的 Span
			// K页的返回给 central cache，n-k 页的挂到第 n-k 个桶中去
			Span *nSpan = _spanLists[i].PopFront();
			// Span* kSpan = new Span;
			Span *kSpan = _spanPool.New();
			// 在 nSpan 的头部切K页
			kSpan->_pageId = nSpan->_pageId;
			kSpan->_n = k;

			nSpan->_pageId += k;
			nSpan->_n -= k;

			// 挂到第 n-k 个桶中去
			_spanLists[nSpan->_n].PushFront(nSpan);
			// 存储 nSpan 的首尾页号和 nSpan 映射，便于 page cache 回收内存时进行合并查找
			//_idSpanMap[nSpan->_pageId] = nSpan;
			//_idSpanMap[nSpan->_pageId + nSpan->_n - 1] = nSpan;
			_idSpanMap.set(nSpan->_pageId, nSpan);
			_idSpanMap.set(nSpan->_pageId + nSpan->_n - 1, nSpan);

			// 建立 id 和 Span 的映射，方便 central cache 回收小块内存时查找对应的 Span
			for (PAGE_ID i = 0; i < kSpan->_n; ++i)
				_idSpanMap.set(kSpan->_pageId + i, kSpan);
			//_idSpanMap[kSpan->_pageId + i] = kSpan;

			return kSpan;
		}
	}

	// 走到这里说明后面的桶都没有 Span
	// 找堆要一个128页的 Span
	// Span* bigSpan = new Span;
	Span *bigSpan = _spanPool.New();
	void *ptr = SystemAlloc(NPAGES - 1);
	bigSpan->_pageId = (PAGE_ID)ptr >> PAGE_SHIFT;
	bigSpan->_n = NPAGES - 1;

	_spanLists[bigSpan->_n].PushFront(bigSpan);

	return NewSpan(k);
}

Span *PageCache::MapObjectToSpan(void *obj)
{
	PAGE_ID id = (PAGE_ID)obj >> PAGE_SHIFT;

	// std::unique_lock<std::mutex> lock(_pageMtx);	// 出了作用域自动解锁

	// auto ret = _idSpanMap.find(id);
	// if (ret != _idSpanMap.end())
	//	return ret->second;
	// else
	//{
	//	assert(false);
	//	return nullptr;
	// }
	auto ret = (Span *)_idSpanMap.get(id);
	assert(ret != nullptr);
	return ret;
}

void PageCache::ReleaseSpanToPageCache(Span *span)
{
	// 大于128页的直接还给堆
	if (span->_n > NPAGES - 1)
	{
		void *ptr = (void *)(span->_pageId << PAGE_SHIFT);
		SystemFree(ptr);
		// delete span;
		_spanPool.Delete(span);

		return;
	}

	// 对 Span 前后的页尝试进行合并，缓解内存碎片问题
	while (1)
	{
		PAGE_ID prevId = span->_pageId - 1;
		// auto ret = _idSpanMap.find(prevId);
		//
		//// 前面的页没有，不进行合并
		// if (ret == _idSpanMap.end())
		//	break;

		auto ret = (Span *)_idSpanMap.get(prevId);
		if (ret == nullptr)
			break;

		// 前面的页正在被使用，不进行合并
		Span *prevSpan = ret;
		if (prevSpan->_isUse == true)
			break;
		// 合并出超过128页的 Span，无法管理，不进行合并
		if (prevSpan->_n + span->_n > NPAGES - 1)
			break;

		span->_pageId = prevSpan->_pageId;
		span->_n += prevSpan->_n;

		_spanLists[prevSpan->_n].Erase(prevSpan);
		// delete prevSpan;
		_spanPool.Delete(prevSpan);
	}

	// 向后合并
	while (1)
	{
		PAGE_ID nextId = span->_pageId + span->_n;
		// auto ret = _idSpanMap.find(nextId);
		//// 后面的页没有，不进行合并
		// if (ret == _idSpanMap.end())
		//	break;
		auto ret = (Span *)_idSpanMap.get(nextId);
		if (ret == nullptr)
			break;

		// 后面的页正在被使用，不进行合并
		Span *nextSpan = ret;
		if (nextSpan->_isUse == true)
			break;
		// 合并出超过128页的 Span，无法管理，不进行合并
		if (nextSpan->_n + span->_n > NPAGES - 1)
			break;

		span->_n += nextSpan->_n;

		_spanLists[nextSpan->_n].Erase(nextSpan);
		// delete nextSpan;
		_spanPool.Delete(nextSpan);
	}

	_spanLists[span->_n].PushFront(span);
	span->_isUse = false;
	//_idSpanMap[span->_pageId] = span;
	//_idSpanMap[span->_pageId + span->_n - 1] = span;
	_idSpanMap.set(span->_pageId, span);
	_idSpanMap.set(span->_pageId + span->_n - 1, span);
}