#pragma once
#include "Common.h"

// 单例模式
class CentralCache
{
public:
	static CentralCache *GetInstance()
	{
		return &_sInst;
	}

	// 获取⼀个非空的 span
	Span *GetOneSpan(SpanList &list, size_t size);
	// 从central cache获取一定数量的内存对象给thread cache
	size_t FetchRangeObj(void *&start, void *&end, size_t batchNum, size_t size);
	// 将⼀定数量的对象释放到 Span
	void ReleaseListToSpans(void *start, size_t size);

private:
	SpanList _spanLists[NFREELIST];

private:
	CentralCache() {}

	CentralCache(const CentralCache &) = delete;

	static CentralCache _sInst;
};