// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2008, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// ---
// Author: Sanjay Ghemawat <opensource@google.com>

#include <config.h>
#ifdef HAVE_INTTYPES_H
#include <inttypes.h> // for PRIuPTR
#endif
#include <errno.h>						 // for ENOMEM, errno
#include <gperftools/malloc_extension.h> // for MallocRange, etc
#include "base/basictypes.h"
#include "base/commandlineflags.h"
#include "internal_logging.h"	 // for ASSERT, TCMalloc_Printer, etc
#include "page_heap_allocator.h" // for PageHeapAllocator
#include "static_vars.h"		 // for Static
#include "system-alloc.h"		 // for TCMalloc_SystemAlloc, etc
#include <cmath>
#include <fstream>
#include <sys/syscall.h>
#include <vector>
#include <chrono>

DEFINE_double(tcmalloc_release_rate,
			  EnvToDouble("TCMALLOC_RELEASE_RATE", 1.0),
			  "Rate at which we release unused memory to the system.  "
			  "Zero means we never release memory back to the system.  "
			  "Increase this flag to return memory faster; decrease it "
			  "to return memory slower.  Reasonable rates are in the "
			  "range [0,10]");

DEFINE_int64(tcmalloc_heap_limit_mb,
			 EnvToInt("TCMALLOC_HEAP_LIMIT_MB", 0),
			 "Limit total size of the process heap to the "
			 "specified number of MiB. "
			 "When we approach the limit the memory is released "
			 "to the system more aggressively (more minor page faults). "
			 "Zero means to allocate as long as system allows.");

namespace tcmalloc
{

	PageHeap::PageHeap()
	{
		DLL_Init(&free_.normal);
		DLL_Init(&free_.returned);
	}

	Span *PageHeap::New(Length n)
	{
		ASSERT(Check());
		ASSERT(n > 0);

		pid_t thread_id = syscall(__NR_gettid);

		//    Log(kLog, __FILE__, __LINE__,
		//        "pageheap new is called! ", thread_id
		//        );

		//		Span* head = &free_.normal;
		//		Span* temp = head;
		//		int length = 0;
		//		if(head != NULL){
		//						do{
		//										temp = temp->next;
		//										length++;
		//						} while( temp != head );
		//		}
		//
		//    Log(kLog, __FILE__, __LINE__,
		//        "length of free normal list is: ", length, " ", thread_id
		//        );

		if (n > 1)
		{
			SpinLockHolder h(Static::extended_lock());
			Span *large_span = Static::extended_memory()->AllocLarge(n);
			//Log(kLog, __FILE__, __LINE__, "lage span length: ", large_span->length, large_span->sizeclass);
			large_span->location = Span::IN_USE;
			return large_span;
		}

		//auto start = std::chrono::high_resolution_clock::now();
		Span *ll = &free_.normal;
		if (!DLL_IsEmpty(ll))
		{
			Span *span = ll->next;
			ASSERT(span->location == Span::ON_NORMAL_FREELIST);
			ASSERT(span->location != Span::IN_USE);
			//auto stop = std::chrono::high_resolution_clock::now();
			//auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(stop-start);
			//	Log(kLog, __FILE__, __LINE__,
			//									"inner allcation duration: ", duration.count()
			//		 );
			RemoveFromFreeList(span);
			span->location = Span::IN_USE;
			return span;
		}
		// Alternatively, maybe there's a usable returned span.
		ll = &free_.returned;
		if (!DLL_IsEmpty(ll))
		{
			ASSERT(ll->next->location == Span::ON_RETURNED_FREELIST);
			Span *span = ll->next;
			ASSERT(span->location != Span::IN_USE);
			RemoveFromFreeList(span);
			span->location = Span::IN_USE;
			return span;
		}

		//		int request_pages = std::pow(2, std::ceil(log2(n+1))+1);
		//		Note on request pages value:
		//		while using 2mb hugepage size, request_pages size should be 12
		//		and for using 1gb hugepage size, request_pages size should be
		int request_pages = 12;
		Span *large_span = NULL;
		Span *new_spans[request_pages];
		{
			//			Log(kLog, __FILE__, __LINE__,
			//											"thread ", thread_id, " wants to acquire extended lock."
			//				 );
			SpinLockHolder h(Static::extended_lock());
			//			Log(kLog, __FILE__, __LINE__,
			//											"thread ", thread_id, " acquired extended lock."
			//				 );
			//			Log(kLog, __FILE__, __LINE__,
			//											"thread ", thread_id, " needs more memory, so requests 4 pages from extended memory unit."
			//				 );
			large_span = Static::extended_memory()->AllocLarge(request_pages);
			//			Log(kLog, __FILE__, __LINE__,
			//											"thread ", thread_id, " recieved 4 pages from extended memory unit."
			//				 );
			for (int p = 0; p < large_span->length - 1; p++)
			{
				new_spans[p] = Static::span_allocator()->New();
			}
		}
		if (large_span != NULL)
		{
			ASSERT(large_span->location != Span::IN_USE);
			const int old_location = large_span->location;
			large_span->location = Span::IN_USE;
			Event(large_span, 'A', request_pages);

			int large_span_items = large_span->length;
			for (int i = 1; i < large_span_items; i++)
			{
				Span *new_span = InitSpan(new_spans[i - 1], large_span->start + i, 1);
				new_span->location = old_location;
				Event(new_span, 'S', 1);
				Static::pagemap()->RecordSpan(new_span);

#ifndef NDEBUG
				const PageID p = new_span->start;
				const Length len = new_span->length;
				Span *next = Static::pagemap()->GetDescriptor(p + len);
				ASSERT(next == NULL ||
					   next->location == Span::IN_USE ||
					   next->location != new_span->location);
#endif

				PrependToFreeList(new_span); // Skip coalescing - no candidates possible
			}
			large_span->length = 1;
			Static::pagemap()->SetPageMap(large_span->start, large_span);
			large_span->location = old_location;
			ASSERT(Check());
			if (old_location == Span::ON_RETURNED_FREELIST)
			{
				Static::pagemap()->CommitSpan(large_span);
			}
			PrependToFreeList(large_span);
			//}
		}
		return New(n);
	}

	void PageHeap::PrependToFreeList(Span *span)
	{
		ASSERT(span->location != Span::IN_USE);
		if (span->location == Span::ON_NORMAL_FREELIST)
			Static::pagemap()->AddFreeBytes(span->length << kPageShift);
		else
			Static::pagemap()->AddUnmappedBytes(span->length << kPageShift);
		//		if(span->length != 1){
		//			int f = 0;
		//		}
		SpanList *list = &free_;
		if (span->location == Span::ON_NORMAL_FREELIST)
		{
			DLL_Prepend(&list->normal, span);
		}
		else
		{
			DLL_Prepend(&list->returned, span);
		}
		/*	Span* head = &free_.normal;
		Span* temp = head;
		int length = 0;
		if(head != NULL){
				do{
						temp = temp->next;
						length++;
				} while( temp != head );
		}
		if (length > 5)
		{
				int p = 9;
		i}*/
	}

	void PageHeap::RemoveFromFreeList(Span *span)
	{
		ASSERT(span->location != Span::IN_USE);
		if (span->location == Span::ON_NORMAL_FREELIST)
		{
			Static::pagemap()->ReduceFreeBytes(span->length << kPageShift);
		}
		else
		{
			Static::pagemap()->ReduceUnmappedBytes(span->length << kPageShift);
		}
		//		auto start = std::chrono::high_resolution_clock::now();
		DLL_Remove(span);
		//		auto stop = std::chrono::high_resolution_clock::now();
		//		auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(stop-start);
		//		Log(kLog, __FILE__, __LINE__,
		//										"dll remove time: " ,duration.count()
		//			 );
	}

	void PageHeap::AppendSpantoPageHeap(Span *span)
	{
		ASSERT(Check());
		ASSERT(span->location == Span::IN_USE);
		ASSERT(span->length > 0);
		ASSERT(Static::pagemap()->GetDescriptor(span->start) == span);
		ASSERT(Static::pagemap()->GetDescriptor(span->start + span->length - 1) == span);
		const Length n = span->length;
		span->sizeclass = 0;
		span->sample = 0;
		span->location = Span::ON_NORMAL_FREELIST;
		//Event(span, 'D', span->length);
		if (span->length > 1)
		{
			SpinLockHolder h(Static::extended_lock());
			Static::extended_memory()->PrependToFreeSet(span);
		}
		else
		{
			PrependToFreeList(span);
			ASSERT(Static::pagemap()->GetUnmappedBytes() + Static::pagemap()->GetCommitedBytes() == Static::pagemap()->GetSystemBytes());
			ASSERT(Check());
		}
	}
	void PageHeap::GetSmallSpanStats(SmallSpanStats *result)
	{
		result->normal_length = DLL_Length(&free_.normal);
		result->returned_length = DLL_Length(&free_.returned);
	}

	bool PageHeap::Check()
	{
		return true;
	}

	bool PageHeap::CheckExpensive()
	{
		bool result = Check();
		Static::extended_lock()->Lock();
		Static::extended_memory()->CheckSet();
		Static::extended_lock()->Unlock();
		CheckList(&free_.normal, Span::ON_NORMAL_FREELIST);
		CheckList(&free_.returned, Span::ON_RETURNED_FREELIST);
		return result;
	}

	bool PageHeap::CheckList(Span *list, int freelist)
	{
		for (Span *s = list->next; s != list; s = s->next)
		{
			CHECK_CONDITION(s->location == freelist); // NORMAL or RETURNED
			CHECK_CONDITION(s->length == 1);
			CHECK_CONDITION(Static::pagemap()->GetDescriptor(s->start) == s);
			CHECK_CONDITION(Static::pagemap()->GetDescriptor(s->start + s->length - 1) == s);
		}
		return true;
	}

	//taghavi
	//extended memory unit nested class methods
	PageHeap::ExtendedMemory::ExtendedMemory()
		: aggressive_decommit_(false),
		  scavenge_counter_(0)
	{
	}

	static const size_t kForcedCoalesceInterval = 128 * 1024 * 1024;

	Span *PageHeap::ExtendedMemory::AllocLarge(Length n)
	{
		Span *best = NULL;
		Span *best_normal = NULL;

		// Create a Span to use as an upper bound.
		Span bound;
		bound.start = 0;
		bound.length = n;

		// First search the NORMAL spans..
		SpanSet::iterator place = large_normal_.upper_bound(SpanPtrWithLength(&bound));
		if (place != large_normal_.end())
		{
			best = place->span;
			best_normal = best;
			ASSERT(best->location == Span::ON_NORMAL_FREELIST);
		}

		// Try to find better fit from RETURNED spans.
		place = large_returned_.upper_bound(SpanPtrWithLength(&bound));
		if (place != large_returned_.end())
		{
			Span *c = place->span;
			ASSERT(c->location == Span::ON_RETURNED_FREELIST);
			if (best_normal == NULL || c->length < best->length || (c->length == best->length && c->start < best->start))
				best = place->span;
		}

		if (best == best_normal && best != NULL)
		{
			return CarveLarge(best, n);
		}

		// best comes from RETURNED set.
		if (Static::pagemap()->EnsureLimit(n, false) && best != NULL)
		{
			return CarveLarge(best, n);
		}

		if (Static::pagemap()->EnsureLimit(n, true) && best != NULL)
		{
			// best could have been destroyed by coalescing.
			// best_normal is not a best-fit, and it could be destroyed as well.
			// We retry, the limit is already ensured:
			return AllocLarge(n);
		}

		// If best_normal existed, EnsureLimit would succeeded:
		ASSERT(best_normal == NULL);
		// We are not allowed to take best from returned list.

		/* if (Static::pagemap()->GetFreeBytes() != 0 && Static::pagemap()->GetUnmappedBytes() != 0
				&& Static::pagemap()->GetFreeBytes() + Static::pagemap()->GetUnmappedBytes() >= Static::pagemap()->GetSystemBytes() / 4
				&& (Static::pagemap()->GetSystemBytes() / kForcedCoalesceInterval
					!= (Static::pagemap()->GetSystemBytes() + (n << kPageShift)) / kForcedCoalesceInterval)) {
			// We're about to grow heap, but there are lots of free pages.
			// tcmalloc's design decision to keep unmapped and free spans
			// separately and never coalesce them means that sometimes there
			// can be free pages span of sufficient size, but it consists of
			// "segments" of different type so page heap search cannot find
			// it. In order to prevent growing heap and wasting memory in such
			// case we're going to unmap all free pages. So that all free
			// spans are maximally coalesced.
			//
			// We're also limiting 'rate' of going into this path to be at
			// most once per 128 megs of heap growth. Otherwise programs that
			// grow heap frequently (and that means by small amount) could be
			// penalized with higher count of minor page faults.
			//
			// See also large_heap_fragmentation_unittest.cc and
			// https://code.google.com/p/gperftools/issues/detail?id=368
			ReleaseAtLeastNPages(static_cast<Length>(0x7fffffff));

			// then try again. If we are forced to grow heap because of large
			// spans fragmentation and not because of problem described above,
			// then at the very least we've just unmapped free but
			// insufficiently big large spans back to OS. So in case of really
			// unlucky memory fragmentation we'll be consuming virtual address
			// space, but not real memory
			Span* result = AllocLarge(n); 
			if (result != NULL) return result;
		}*/

		// Grow the heap and try again.

		/*
		   >>> for flowchart 18 goto implementation of GrowHeap method in this file.
		 */
		if (!GrowHeap(n))
		{
			ASSERT(Static::pagemap()->GetUnmappedBytes() + Static::pagemap()->GetCommitedBytes() == Static::pagemap()->GetSystemBytes());
			ASSERT(Check());
			// underlying SysAllocator likely set ENOMEM but we can get here
			// due to EnsureLimit so we set it here too.
			//
			// Setting errno to ENOMEM here allows us to avoid dealing with it
			// in fast-path.
			errno = ENOMEM;
			return NULL;
		}
		return AllocLarge(n);
	}

	Span *PageHeap::ExtendedMemory::CarveLarge(Span *span, Length n)
	{
		ASSERT(n > 0);
		ASSERT(span->location != Span::IN_USE);
		const int old_location = span->location;
		RemoveFromFreeSet(span);
		span->location = Span::IN_USE;
		Event(span, 'A', n);

		const int extra = span->length - n;
		ASSERT(extra >= 0);
		if (extra > 0)
		{
			Span *leftover = NewSpan(span->start + n, extra);
			leftover->location = old_location;
			Event(leftover, 'S', extra);
			Static::pagemap()->RecordSpan(leftover);

			// The previous span of |leftover| was just splitted -- no need to
			// coalesce them. The next span of |leftover| was not previously coalesced
			// with |span|, i.e. is NULL or has got location other than |old_location|.
#ifndef NDEBUG
			const PageID p = leftover->start;
			const Length len = leftover->length;
			Span *next = Static::pagemap()->GetDescriptor(p + len);
			ASSERT(next == NULL ||
				   next->location == Span::IN_USE ||
				   next->location != leftover->location);
#endif

			PrependToFreeSet(leftover); // Skip coalescing - no candidates possible
			span->length = n;
			Static::pagemap()->SetPageMap(span->start + n - 1, span);
		}
		span->location = old_location;
		ASSERT(Check());
		if (old_location == Span::ON_RETURNED_FREELIST)
		{
			// We need to recommit this address space.
			Static::pagemap()->CommitSpan(span);
		}
		ASSERT(span->location != Span::IN_USE);
		ASSERT(span->length == n);
		ASSERT(Static::pagemap()->GetUnmappedBytes() + Static::pagemap()->GetCommitedBytes() == Static::pagemap()->GetSystemBytes());
		return span;
	}

	void PageHeap::ExtendedMemory::RemoveFromFreeSet(Span *span)
	{
		ASSERT(span->location != Span::IN_USE);
		if (span->location == Span::ON_NORMAL_FREELIST)
		{
			Static::pagemap()->ReduceFreeBytes(span->length << kPageShift);
		}
		else
		{
			Static::pagemap()->ReduceUnmappedBytes(span->length << kPageShift);
		}
		SpanSet *set = &large_normal_;
		if (span->location == Span::ON_RETURNED_FREELIST)
			set = &large_returned_;
		SpanSet::iterator iter = span->ExtractSpanSetIterator();
		ASSERT(iter->span == span);
		ASSERT(set->find(SpanPtrWithLength(span)) == iter);
		set->erase(iter);
	}

	void PageHeap::ExtendedMemory::PrependToFreeSet(Span *span)
	{
		ASSERT(span->location != Span::IN_USE);
		if (span->location == Span::ON_NORMAL_FREELIST)
			Static::pagemap()->AddFreeBytes(span->length << kPageShift);
		else
			Static::pagemap()->AddUnmappedBytes(span->length << kPageShift);

		SpanSet *set = &large_normal_;
		if (span->location == Span::ON_RETURNED_FREELIST)
			set = &large_returned_;
		std::pair<SpanSet::iterator, bool> p =
			set->insert(SpanPtrWithLength(span));
		ASSERT(p.second); // We never have duplicates since span->start is unique.
		span->SetSpanSetIterator(p.first);
		return;
	}

	bool PageHeap::ExtendedMemory::Check()
	{
		return true;
	}

	Length PageHeap::ExtendedMemory::ReleaseAtLeastNPages(Length num_pages)
	{
		Length released_pages = 0;

		while (released_pages < num_pages && Static::pagemap()->GetFreeBytes() > 0)
		{
			Span *s;
			if (large_normal_.empty())
			{
				return 0;
			}
			s = (large_normal_.begin())->span;

			Length released_len = ReleaseSpan(s);
			// Some systems do not support release
			if (released_len == 0)
				return released_pages;
			released_pages += released_len;
		}
		return released_pages;
	}

	Length PageHeap::ExtendedMemory::ReleaseSpan(Span *s)
	{
		ASSERT(s->location == Span::ON_NORMAL_FREELIST);

		if (Static::pagemap()->DecommitSpan(s))
		{
			RemoveFromFreeSet(s);
			const Length n = s->length;
			s->location = Span::ON_RETURNED_FREELIST;
			MergeIntoFreeSet(s); // Coalesces if possible.
			return n;
		}

		return 0;
	}

	void PageHeap::ExtendedMemory::MergeIntoFreeSet(Span *span)
	{
		ASSERT(span->location != Span::IN_USE);

		// Coalesce -- we guarantee that "p" != 0, so no bounds checking
		// necessary.  We do not bother resetting the stale pagemap
		// entries for the pieces we are merging together because we only
		// care about the pagemap entries for the boundaries.
		//
		// Note: depending on aggressive_decommit_ mode we allow only
		// similar spans to be coalesced.
		//
		// The following applies if aggressive_decommit_ is enabled:
		//
		// TODO(jar): "Always decommit" causes some extra calls to commit when we are
		// called in GrowHeap() during an allocation :-/.  We need to eval the cost of
		// that oscillation, and possibly do something to reduce it.

		// TODO(jar): We need a better strategy for deciding to commit, or decommit,
		// based on memory usage and free heap sizes.

		const PageID p = span->start;
		const Length n = span->length;

		if (aggressive_decommit_ && span->location == Span::ON_NORMAL_FREELIST)
		{
			if (Static::pagemap()->DecommitSpan(span))
			{
				span->location = Span::ON_RETURNED_FREELIST;
			}
		}

		Span *prev = CheckAndHandlePreMerge(span, Static::pagemap()->GetDescriptor(p - 1));
		if (prev != NULL)
		{
			// Merge preceding span into this span
			ASSERT(prev->start + prev->length == p);
			const Length len = prev->length;
			DeleteSpan(prev);
			span->start -= len;
			span->length += len;
			Static::pagemap()->SetPageMap(span->start, span);
			Event(span, 'L', len);
		}
		Span *next = CheckAndHandlePreMerge(span, Static::pagemap()->GetDescriptor(p + n));
		if (next != NULL)
		{
			// Merge next span into this span
			ASSERT(next->start == p + n);
			const Length len = next->length;
			DeleteSpan(next);
			span->length += len;
			Static::pagemap()->SetPageMap(span->start + span->length - 1, span);
			Event(span, 'R', len);
		}

		PrependToFreeSet(span);
	}

	// Given span we're about to free and other span (still on free list),
	// checks if 'other' span is mergable with 'span'. If it is, removes
	// other span from free list, performs aggressive decommit if
	// necessary and returns 'other' span. Otherwise 'other' span cannot
	// be merged and is left untouched. In that case NULL is returned.
	Span *PageHeap::ExtendedMemory::CheckAndHandlePreMerge(Span *span, Span *other)
	{
		if (other == NULL)
		{
			return other;
		}
		if (other->has_span_iter == false)
		{
			return NULL;
		}
		// if we're in aggressive decommit mode and span is decommitted,
		// then we try to decommit adjacent span.
		if (aggressive_decommit_ && other->location == Span::ON_NORMAL_FREELIST && span->location == Span::ON_RETURNED_FREELIST)
		{
			bool worked = Static::pagemap()->DecommitSpan(other);
			if (!worked)
			{
				return NULL;
			}
		}
		else if (other->location != span->location)
		{
			return NULL;
		}
		RemoveFromFreeSet(other);
		return other;
	}

	static void RecordGrowth(size_t growth)
	{
		StackTrace *t = Static::stacktrace_allocator()->New();
		t->depth = GetStackTrace(t->stack, kMaxStackDepth - 1, 3);
		t->size = growth;
		t->stack[kMaxStackDepth - 1] = reinterpret_cast<void *>(Static::growth_stacks());
		Static::set_growth_stacks(t);
	}

	bool PageHeap::ExtendedMemory::GrowHeap(Length n)
	{
		if (n > kMaxValidPages)
			return false;
		Length ask = (n > kMinSystemAlloc) ? n : static_cast<Length>(kMinSystemAlloc);
		size_t actual_size;
		void *ptr = NULL;
		if (Static::pagemap()->EnsureLimit(ask))
		{
			/*
			   >>> flowchart 18. grow heap by  requesting  m size of bytes that is multiple 
			   >>> size of n pages. for more info about allocating by system calls goto
			   >>> implementation of TCMalloc_SystemAlloc in system_alloc.cc file.
			 */
			ptr = TCMalloc_SystemAlloc(ask << kPageShift, &actual_size, kPageSize);
		}
		if (ptr == NULL)
		{
			if (n < ask)
			{
				// Try growing just "n" pages
				ask = n;
				if (Static::pagemap()->EnsureLimit(ask))
				{
					ptr = TCMalloc_SystemAlloc(ask << kPageShift, &actual_size, kPageSize);
				}
			}
			if (ptr == NULL)
				return false;
		}
		ask = actual_size >> kPageShift;
		RecordGrowth(ask << kPageShift);

		Static::pagemap()->AddReserveCount(1);
		Static::pagemap()->AddCommitCount(1);

		uint64_t old_system_bytes = Static::pagemap()->GetSystemBytes();
		Static::pagemap()->AddSystemBytes(ask << kPageShift);
		Static::pagemap()->AddCommitedBytes(ask << kPageShift);

		Static::pagemap()->AddTotalCommitBytes(ask << kPageShift);
		Static::pagemap()->AddTotalReserveBytes(ask << kPageShift);

		const PageID p = reinterpret_cast<uintptr_t>(ptr) >> kPageShift;
		ASSERT(p > 0);

		// If we have already a lot of pages allocated, just pre allocate a bunch of
		// memory for the page map. This prevents fragmentation by pagemap metadata
		// when a program keeps allocating and freeing large blocks.

		if (old_system_bytes < kPageMapBigAllocationThreshold && Static::pagemap()->GetSystemBytes() >= kPageMapBigAllocationThreshold)
		{
			Static::pagemap()->PreallocateMoreMemoryPageMap();
		}

		// Make sure pagemap_ has entries for all of the new pages.
		// Plus ensure one before and one after so coalescing code
		// does not need bounds-checking.
		if (Static::pagemap()->EnsurePageMap(p - 1, ask + 2))
		{
			// Pretend the new area is allocated and then Delete() it to cause
			// any necessary coalescing to occur.

			/*
			   >>> flowchart 19. add span with size m to page heap free linked list of spans
			   >>> or page heap spanset of large spans. for more info goto impl of 
			   >>> NewSpan and Delete method in this file.
			 */
			Span *span = NewSpan(p, ask);
			Static::pagemap()->RecordSpan(span);
			Delete(span);
			ASSERT(Static::pagemap()->GetUnmappedBytes() + Static::pagemap()->GetCommitedBytes() == Static::pagemap()->GetSystemBytes());
			ASSERT(Check());
			return true;
		}
		else
		{
			// We could not allocate memory within "pagemap_"
			// TODO: Once we can return memory to the system, return the new span
			return false;
		}
	}

	void PageHeap::ExtendedMemory::Delete(Span *span)
	{
		ASSERT(Check());
		ASSERT(span->location == Span::IN_USE);
		ASSERT(span->length > 0);
		ASSERT(Static::pagemap()->GetDescriptor(span->start) == span);
		ASSERT(Static::pagemap()->GetDescriptor(span->start + span->length - 1) == span);
		const Length n = span->length;
		span->sizeclass = 0;
		span->sample = 0;
		span->location = Span::ON_NORMAL_FREELIST;
		Event(span, 'D', span->length);
		MergeIntoFreeSet(span); // Coalesces if possible
		IncrementalScavenge(n);
		ASSERT(Static::pagemap()->GetUnmappedBytes() + Static::pagemap()->GetCommitedBytes() == Static::pagemap()->GetSystemBytes());
		ASSERT(Check());
	}

	Span *PageHeap::ExtendedMemory::Split(Span *span, Length n)
	{
		ASSERT(0 < n);
		ASSERT(n < span->length);
		ASSERT(span->location == Span::IN_USE);
		ASSERT(span->sizeclass == 0);
		Event(span, 'T', n);

		const int extra = span->length - n;
		Span *leftover = NewSpan(span->start + n, extra);
		ASSERT(leftover->location == Span::IN_USE);
		Event(leftover, 'U', extra);
		Static::pagemap()->RecordSpan(leftover);
		Static::pagemap()->SetPageMap(span->start + n - 1, span); // Update map from pageid to span
		span->length = n;

		return leftover;
	}

	void PageHeap::ExtendedMemory::IncrementalScavenge(Length n)
	{
		// Fast path; not yet time to release memory
		scavenge_counter_ -= n;
		if (scavenge_counter_ >= 0)
			return; // Not yet time to scavenge

		const double rate = FLAGS_tcmalloc_release_rate;
		if (rate <= 1e-6)
		{
			// Tiny release rate means that releasing is disabled.
			scavenge_counter_ = kDefaultReleaseDelay;
			return;
		}

		Static::pagemap()->AddScavengeCount(1);

		Length released_pages = ReleaseAtLeastNPages(1);

		if (released_pages == 0)
		{
			// Nothing to scavenge, delay for a while.
			scavenge_counter_ = kDefaultReleaseDelay;
		}
		else
		{
			// Compute how long to wait until we return memory.
			// FLAGS_tcmalloc_release_rate==1 means wait for 1000 pages
			// after releasing one page.
			const double mult = 1000.0 / rate;
			double wait = mult * static_cast<double>(released_pages);
			if (wait > kMaxReleaseDelay)
			{
				// Avoid overflow and bound to reasonable range.
				wait = kMaxReleaseDelay;
			}
			scavenge_counter_ = static_cast<int64_t>(wait);
		}
	}

	bool PageHeap::ExtendedMemory::CheckSet()
	{
		for (SpanSet::iterator it = large_normal_.begin(); it != large_normal_.end(); ++it)
		{
			Span *s = it->span;
			CHECK_CONDITION(s->length == it->length);
			CHECK_CONDITION(s->location == Span::ON_NORMAL_FREELIST);
			CHECK_CONDITION(Static::pagemap()->GetDescriptor(s->start) == s);
			CHECK_CONDITION(Static::pagemap()->GetDescriptor(s->start + s->length - 1) == s);
		}
		for (SpanSet::iterator it = large_returned_.begin(); it != large_returned_.end(); ++it)
		{
			Span *s = it->span;
			CHECK_CONDITION(s->length == it->length);
			CHECK_CONDITION(s->location == Span::ON_RETURNED_FREELIST);
			CHECK_CONDITION(Static::pagemap()->GetDescriptor(s->start) == s);
			CHECK_CONDITION(Static::pagemap()->GetDescriptor(s->start + s->length - 1) == s);
		}
		return true;
	}

	void PageHeap::ExtendedMemory::GetLargeSpanStats(LargeSpanStats *result)
	{
		result->spans = 0;
		result->normal_pages = 0;
		result->returned_pages = 0;
		for (SpanSet::iterator it = large_normal_.begin(); it != large_normal_.end(); ++it)
		{
			result->normal_pages += it->length;
			result->spans++;
		}
		for (SpanSet::iterator it = large_returned_.begin(); it != large_returned_.end(); ++it)
		{
			result->returned_pages += it->length;
			result->spans++;
		}
	}

	PageHeap::PageMap::PageMap()
		: pagemap_(MetaDataAlloc)
	{
	}

	void PageHeap::PageMap::CommitSpan(Span *span)
	{
		++stats_.commit_count;

		TCMalloc_SystemCommit(reinterpret_cast<void *>(span->start << kPageShift),
							  static_cast<size_t>(span->length << kPageShift));
		stats_.committed_bytes += span->length << kPageShift;
		stats_.total_commit_bytes += (span->length << kPageShift);
	}

	bool PageHeap::PageMap::EnsureLimit(Length n, bool withRelease)
	{
		Length limit = (FLAGS_tcmalloc_heap_limit_mb * 1024 * 1024) >> kPageShift;
		if (limit == 0)
			return true; //there is no limit

		// We do not use stats_.system_bytes because it does not take
		// MetaDataAllocs into account.
		Length takenPages = TCMalloc_SystemTaken >> kPageShift;
		//XXX takenPages may be slightly bigger than limit for two reasons:
		//* MetaDataAllocs ignore the limit (it is not easy to handle
		//  out of memory there)
		//* sys_alloc may round allocation up to huge page size,
		//  although smaller limit was ensured

		ASSERT(takenPages >= stats_.unmapped_bytes >> kPageShift);
		takenPages -= stats_.unmapped_bytes >> kPageShift;

		if (takenPages + n > limit && withRelease)
		{
			takenPages -= Static::extended_memory()->ReleaseAtLeastNPages(takenPages + n - limit);
		}

		return takenPages + n <= limit;
	}

	bool PageHeap::PageMap::DecommitSpan(Span *span)
	{
		++stats_.decommit_count;

		bool rv = TCMalloc_SystemRelease(reinterpret_cast<void *>(span->start << kPageShift),
										 static_cast<size_t>(span->length << kPageShift));
		if (rv)
		{
			stats_.committed_bytes -= span->length << kPageShift;
			stats_.total_decommit_bytes += (span->length << kPageShift);
		}

		return rv;
	}

	void PageHeap::PageMap::SetPageMap(Number k, void *v)
	{
		pagemap_.set(k, v);
	}

	void *PageHeap::PageMap::NextPageMap(Number k)
	{
		return pagemap_.Next(k);
	}

	void PageHeap::PageMap::PreallocateMoreMemoryPageMap()
	{
		pagemap_.PreallocateMoreMemory();
	}

	bool PageHeap::PageMap::EnsurePageMap(Number start, size_t n)
	{
		return pagemap_.Ensure(start, n);
	}

	void PageHeap::PageMap::RegisterSizeClass(Span *span, uint32 sc)
	{
		// Associate span object with all interior pages as well
		ASSERT(span->location == Span::IN_USE);
		ASSERT(GetDescriptor(span->start) == span);
		ASSERT(GetDescriptor(span->start + span->length - 1) == span);
		Event(span, 'C', sc);
		span->sizeclass = sc;
		for (Length i = 1; i < span->length - 1; i++)
		{
			pagemap_.set(span->start + i, span);
		}
	}

	bool PageHeap::PageMap::GetNextRange(PageID start, base::MallocRange *r)
	{
		Span *span = reinterpret_cast<Span *>(pagemap_.Next(start));
		if (span == NULL)
		{
			return false;
		}
		r->address = span->start << kPageShift;
		r->length = span->length << kPageShift;
		r->fraction = 0;
		switch (span->location)
		{
		case Span::IN_USE:
			r->type = base::MallocRange::INUSE;
			r->fraction = 1;
			if (span->sizeclass > 0)
			{
				// Only some of the objects in this span may be in use.
				const size_t osize = Static::sizemap()->class_to_size(span->sizeclass);
				r->fraction = (1.0 * osize * span->refcount) / r->length;
			}
			break;
		case Span::ON_NORMAL_FREELIST:
			r->type = base::MallocRange::FREE;
			break;
		case Span::ON_RETURNED_FREELIST:
			r->type = base::MallocRange::UNMAPPED;
			break;
		default:
			r->type = base::MallocRange::UNKNOWN;
			break;
		}
		return true;
	}
} // namespace tcmalloc
