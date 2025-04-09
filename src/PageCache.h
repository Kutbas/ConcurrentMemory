#pragma once
#include "Common.h"
#include "ObjectPool.h"
#include "PageMap.h"

class PageCache
{
public:
	static PageCache *GetInstance()
	{
		return &_sInst;
	}

	// 获取从对象到 Span 的映射
	Span *MapObjectToSpan(void *obj);

	// 获取一个 k 页的 Span
	Span *NewSpan(size_t k);
	// 将空闲 Span归还至 page cache，并合并相邻 Span
	void ReleaseSpanToPageCache(Span *span);

public:
	std::mutex _pageMtx;

private:
	SpanList _spanLists[NPAGES];
	ObjectPool<Span> _spanPool;
	// std::unordered_map<PAGE_ID, Span*> _idSpanMap;
	TCMalloc_PageMap1<32 - PAGE_SHIFT> _idSpanMap;

	static PageCache _sInst;

	PageCache() {}
	PageCache(const PageCache &) = delete;
};