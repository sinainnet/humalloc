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

#ifndef TCMALLOC_THREAD_CACHE_H_
#define TCMALLOC_THREAD_CACHE_H_

#include <config.h>
#ifdef HAVE_PTHREAD
#include <pthread.h> // for pthread_t, pthread_key_t
#endif
#include <stddef.h> // for size_t, NULL
#ifdef HAVE_STDINT_H
#include <stdint.h> // for uint32_t, uint64_t
#endif
#include <sys/types.h> // for ssize_t
#include "base/commandlineflags.h"
#include "common.h"
#include "linked_list.h"
#include "maybe_threads.h"
#include "page_heap_allocator.h"
#include "sampler.h"
#include "static_vars.h"

#include "common.h"              // for SizeMap, kMaxSize, etc
#include "internal_logging.h"    // for ASSERT, etc
#include "linked_list.h"         // for SLL_Pop, SLL_PopRange, etc
#include "page_heap_allocator.h" // for PageHeapAllocator
#include "sampler.h"             // for Sampler
#include "static_vars.h"         // for Static
#include <map>

DECLARE_int64(tcmalloc_sample_parameter);

namespace tcmalloc
{
  //-------------------------------------------------------------------
  // Data kept per thread
  //-------------------------------------------------------------------

  class ThreadCache
  {
  public:
#ifdef HAVE_TLS
    enum
    {
      have_tls = true
    };
#else
    enum
    {
      have_tls = false
    };
#endif

    void Init(pthread_t tid);
    void Cleanup();

    // Accessors (mostly just for printing stats)
    int freelist_length(uint32 cl) const { return list_[cl].sc_length(); }

    // Total byte size in cache
    size_t Size() const { return size_; }

    // Allocate an object of the given size and class. The size given
    // must be the same as the size of the class in the size map.
    void *Allocate(size_t size, uint32 cl, void *(*oom_handler)(size_t size));
    void Deallocate(void *ptr, uint32 size_class);

    void Scavenge();

    int GetSamplePeriod();

    // Record allocation of "k" bytes.  Return true iff allocation
    // should be sampled
    bool SampleAllocation(size_t k);

    bool TryRecordAllocationFast(size_t k);

    static void InitModule();
    static void InitTSD();
    static ThreadCache *GetThreadHeap();
    static ThreadCache *GetCache();
    static ThreadCache *GetCacheIfPresent();
    static ThreadCache *GetFastPathCache();
    static ThreadCache *GetCacheWhichMustBePresent();
    static ThreadCache *CreateCacheIfNecessary();
    static void BecomeIdle();
    static void BecomeTemporarilyIdle();
    static void SetUseEmergencyMalloc();
    static void ResetUseEmergencyMalloc();
    static bool IsUseEmergencyMalloc();

    // Return the number of thread heaps in use.
    static inline int HeapsInUse();

    // Adds to *total_bytes the total number of bytes used by all thread heaps.
    // Also, if class_count is not NULL, it must be an array of size kNumClasses,
    // and this function will increment each element of class_count by the number
    // of items in all thread-local freelists of the corresponding size class.
    // REQUIRES: Static::pageheap_lock is held.
    static void GetThreadStats(uint64_t *total_bytes, uint64_t *class_count);

    // Sets the total thread cache size to new_size, recomputing the
    // individual thread cache sizes as necessary.
    // REQUIRES: Static::pageheap lock is held.
    static void set_overall_thread_cache_size(size_t new_size);
    static size_t overall_thread_cache_size()
    {
      return overall_thread_cache_size_;
    }

  private:
    class FreeList
    {
    private:
      void *list_; // Linked list of nodes

#ifdef _LP64
      // On 64-bit hardware, manipulating 16-bit values may be slightly slow.
      uint32_t length_; // Current length.
#else
      // If we aren't using 64-bit pointers then pack these into less space.
      uint16_t length_;
#endif

    public:
      void Init(size_t size)
      {
        list_ = NULL;
        length_ = 0;
      }

      // Return current length of list
      size_t length() const
      {
        return length_;
      }

      uint32_t Push(void *ptr)
      {
        uint32_t length = length_ + 1;
        SLL_Push(&list_, ptr);
        length_ = length;
        return length;
      }

      void *Pop()
      {
        ASSERT(list_ != NULL);
        length_--;
        return SLL_Pop(&list_);
      }

      bool TryPop(void **rv)
      {
        if (SLL_TryPop(&list_, rv))
        {
          length_--;
          return true;
        }
        return false;
      }

      void PushRange(int N, void *start, void *end)
      {
        SLL_PushRange(&list_, start, end);
        length_ += N;
      }

      void PopRange(int N, void **start, void **end)
      {
        SLL_PopRange(&list_, N, start, end);
        ASSERT(length_ >= N);
        length_ -= N;
      }
    };

    class FreeListInventory
    {
    private:
      std::map<Span *, FreeList *> freelists;
      uint32_t sc_length_;
      int32_t sc_size_;
      uint32_t sc_max_length_;
      // Tracks the number of times a deallocation has caused
      // length_ > max_length_.  After the kMaxOverages'th time, max_length_
      // shrinks and length_overages_ is reset to zero.
      uint32_t sc_length_overages_;
      uint32_t lowater_; // Low water mark for list length.

      Span *MapObjectToSpan(void *object)
      {
        const PageID p = reinterpret_cast<uintptr_t>(object) >> kPageShift;
        Span *span = Static::pagemap()->GetDescriptor(p);
        return span;
      }

    public:
      void SCInit(size_t size)
      {
        sc_length_ = 0;
        sc_max_length_ = 1;
        sc_size_ = size;
        lowater_ = 0;
      }

      size_t sc_length() const
      {
        return sc_length_;
      }

      int32_t sc_object_size() const
      {
        return sc_size_;
      }

      size_t sc_max_length() const
      {
        return sc_max_length_;
      }

      void sc_set_max_length(size_t new_max)
      {
        sc_max_length_ = new_max;
      }

      size_t sc_length_overages() const
      {
        return sc_length_overages_;
      }

      void sc_set_length_overages(size_t new_count)
      {
        sc_length_overages_ = new_count;
      }

      // Low-water mark management
      int lowwatermark() const { return lowater_; }
      void clear_lowwatermark() { lowater_ = sc_length_; }

      void SCPushRange(int N, void *start, void *end)
      {
        Span *span = MapObjectToSpan(start);
        if (freelists.count(span) == 0)
        {
          FreeList fl;
          fl.Init(sc_size_);
          FreeList *flp = &fl;
          freelists[span] = flp;
        }
        freelists[span]->PushRange(N, start, end);
        sc_length_ += N;
      }

      bool SCPopRange(int N, void **start, void **end)
      {
        std::map<Span *, FreeList *>::iterator it;
        for (it = freelists.begin(); it != freelists.end(); it++)
        {
          if (it->second->length() >= N)
          {
            it->second->PopRange(N, start, end);
            sc_length_ -= N;
            if (sc_length_ < lowater_)
              lowater_ = sc_length_;
            return true;
          }
        }
        return false;
      }

      bool SCTryPop(void **rv)
      {
        std::map<Span *, FreeList *>::iterator it;
        for (it = freelists.begin(); it != freelists.end(); it++)
        {
          if (it->second->length() > 0)
          {
            it->second->TryPop(rv);
            sc_length_--;
            if (PREDICT_FALSE(sc_length_ < lowater_))
              lowater_ = sc_length_;
            return true;
          }
        }
        return false;
      }

      void *SCPop()
      {
        std::map<Span *, FreeList *>::iterator it;
        for (it = freelists.begin(); it != freelists.end(); it++)
        {
          if (it->second->length() > 0)
          {
            sc_length_--;
            if (sc_length_ < lowater_)
              lowater_ = sc_length_;
            return it->second->Pop();
          }
        }
      }

      uint32_t SCPush(void *ptr)
      {
        Span *span = MapObjectToSpan(ptr);
        if (freelists.count(span) == 0)
        {
          FreeList fl;
          fl.Init(sc_size_);
          FreeList *flp = &fl;
          freelists[span] = flp;
        }
        freelists[span]->Push(ptr);
        sc_length_++;
        return sc_length_;
      }
    };

    // Gets and returns an object from the central cache, and, if possible,
    // also adds some objects of that size class to this thread cache.
    void *FetchFromCentralCache(uint32 cl, int32_t byte_size,
                                void *(*oom_handler)(size_t size));

    void ListTooLong(void *ptr, uint32 cl);

    // Releases some number of items from src.  Adjusts the list's max_length
    // to eventually converge on num_objects_to_move(cl).
    void ListTooLong(FreeListInventory *src, uint32 cl);

    // Releases N items from this thread cache.
    void ReleaseToCentralCache(FreeListInventory *src, uint32 cl, int N);

    void SetMaxSize(int32 new_max_size);

    // Increase max_size_ by reducing unclaimed_cache_space_ or by
    // reducing the max_size_ of some other thread.  In both cases,
    // the delta is kStealAmount.
    void IncreaseCacheLimit();
    // Same as above but requires Static::pageheap_lock() is held.
    void IncreaseCacheLimitLocked();

    // If TLS is available, we also store a copy of the per-thread object
    // in a __thread variable since __thread variables are faster to read
    // than pthread_getspecific().  We still need pthread_setspecific()
    // because __thread variables provide no way to run cleanup code when
    // a thread is destroyed.
    // We also give a hint to the compiler to use the "initial exec" TLS
    // model.  This is faster than the default TLS model, at the cost that
    // you cannot dlopen this library.  (To see the difference, look at
    // the CPU use of __tls_get_addr with and without this attribute.)
    // Since we don't really use dlopen in google code -- and using dlopen
    // on a malloc replacement is asking for trouble in any case -- that's
    // a good tradeoff for us.
#ifdef HAVE_TLS
    struct ThreadLocalData
    {
      ThreadCache *fast_path_heap;
      ThreadCache *heap;
      bool use_emergency_malloc;
    };
    static __thread ThreadLocalData threadlocal_data_
        CACHELINE_ALIGNED ATTR_INITIAL_EXEC;

#endif

    // Thread-specific key.  Initialization here is somewhat tricky
    // because some Linux startup code invokes malloc() before it
    // is in a good enough state to handle pthread_keycreate().
    // Therefore, we use TSD keys only after tsd_inited is set to true.
    // Until then, we use a slow path to get the heap object.
    static ATTRIBUTE_HIDDEN bool tsd_inited_;
    static pthread_key_t heap_key_;

    // Linked list of heap objects.  Protected by Static::pageheap_lock.
    static ThreadCache *thread_heaps_;
    static int thread_heap_count_;

    // A pointer to one of the objects in thread_heaps_.  Represents
    // the next ThreadCache from which a thread over its max_size_ should
    // steal memory limit.  Round-robin through all of the objects in
    // thread_heaps_.  Protected by Static::pageheap_lock.
    static ThreadCache *next_memory_steal_;

    // Overall thread cache size.  Protected by Static::pageheap_lock.
    static size_t overall_thread_cache_size_;

    // Global per-thread cache size.  Writes are protected by
    // Static::pageheap_lock.  Reads are done without any locking, which should be
    // fine as long as size_t can be written atomically and we don't place
    // invariants between this variable and other pieces of state.
    static volatile size_t per_thread_cache_size_;

    // Represents overall_thread_cache_size_ minus the sum of max_size_
    // across all ThreadCaches.  Protected by Static::pageheap_lock.
    static ssize_t unclaimed_cache_space_;

    // This class is laid out with the most frequently used fields
    // first so that hot elements are placed on the same cache line.

    FreeListInventory list_[kClassSizesMax]; // Array indexed by size-class

    int32 size_;     // Combined size of data
    int32 max_size_; // size_ > max_size_ --> Scavenge()

    // We sample allocations, biased by the size of the allocation
    Sampler sampler_; // A sampler

    pthread_t tid_;       // Which thread owns it
    bool in_setspecific_; // In call to pthread_setspecific?

    // Allocate a new heap. REQUIRES: Static::pageheap_lock is held.
    static ThreadCache *NewHeap(pthread_t tid);

    // Use only as pthread thread-specific destructor function.
    static void DestroyThreadCache(void *ptr);

    static void DeleteCache(ThreadCache *heap);
    static void RecomputePerThreadCacheSize();

  public:
    // All ThreadCache objects are kept in a linked list (for stats collection)
    ThreadCache *next_;
    ThreadCache *prev_;

    // Ensure that this class is cacheline-aligned. This is critical for
    // performance, as false sharing would negate many of the benefits
    // of a per-thread cache.
  } CACHELINE_ALIGNED;

  // Allocator for thread heaps
  // This is logically part of the ThreadCache class, but MSVC, at
  // least, does not like using ThreadCache as a template argument
  // before the class is fully defined.  So we put it outside the class.
  extern PageHeapAllocator<ThreadCache> threadcache_allocator;

  inline int ThreadCache::HeapsInUse()
  {
    return threadcache_allocator.inuse();
  }

  inline ATTRIBUTE_ALWAYS_INLINE void *ThreadCache::Allocate(
      size_t size, uint32 cl, void *(*oom_handler)(size_t size))
  {
    FreeListInventory *list = &list_[cl];

#ifdef NO_TCMALLOC_SAMPLES
    size = list->sc_object_size();
#endif

    ASSERT(size <= kMaxSize);
    ASSERT(size != 0);
    ASSERT(size == 0 || size == Static::sizemap()->ByteSizeForClass(cl));

    void *rv;

    /*
  >>> flowchart 4. is thread cache free list empty?
  >>> flowchart 5. remove object from thread cache free list
  >>> process of removing object from thread cache is implemented
  >>> in TryPop function in this file.
  */
    if (!list->SCTryPop(&rv))
    {

      /*
    >>> for flowchart 7 goto implementation of FetchFromCentralCache method
    >>> in thread_cache.cc file.
    */
      return FetchFromCentralCache(cl, size, oom_handler);
    }
    size_ -= size;

    /*
  >>> flowchart 6. return object to program
  >>> object will return to do_malloc method in tcmalloc.cc
  >>> and then return to program.
  */
    return rv;
  }

  inline ATTRIBUTE_ALWAYS_INLINE void ThreadCache::Deallocate(void *ptr, uint32 cl)
  {
    ASSERT(list_[cl].sc_max_length() > 0);
    FreeListInventory *list = &list_[cl];

    // This catches back-to-back frees of allocs in the same size
    // class. A more comprehensive (and expensive) test would be to walk
    // the entire freelist. But this might be enough to find some bugs.
    // ASSERT(ptr != list->Next());

    uint32_t length = list->SCPush(ptr);

    if (PREDICT_FALSE(length > list->sc_max_length()))
    {
      ListTooLong(list, cl);
      return;
    }

    size_ += list->sc_object_size();
    if (PREDICT_FALSE(size_ > max_size_))
    {
      Scavenge();
    }
  }

  inline ThreadCache *ThreadCache::GetThreadHeap()
  {
#ifdef HAVE_TLS
    return threadlocal_data_.heap;
#else
    return reinterpret_cast<ThreadCache *>(
        perftools_pthread_getspecific(heap_key_));
#endif
  }

  inline ThreadCache *ThreadCache::GetCacheWhichMustBePresent()
  {
#ifdef HAVE_TLS
    ASSERT(threadlocal_data_.heap);
    return threadlocal_data_.heap;
#else
    ASSERT(perftools_pthread_getspecific(heap_key_));
    return reinterpret_cast<ThreadCache *>(
        perftools_pthread_getspecific(heap_key_));
#endif
  }

  inline ThreadCache *ThreadCache::GetCache()
  {
#ifdef HAVE_TLS
    ThreadCache *ptr = GetThreadHeap();
#else
    ThreadCache *ptr = NULL;
    if (PREDICT_TRUE(tsd_inited_))
    {
      ptr = GetThreadHeap();
    }
#endif
    if (ptr == NULL)
      ptr = CreateCacheIfNecessary();
    return ptr;
  }

  // In deletion paths, we do not try to create a thread-cache.  This is
  // because we may be in the thread destruction code and may have
  // already cleaned up the cache for this thread.
  inline ThreadCache *ThreadCache::GetCacheIfPresent()
  {
#ifndef HAVE_TLS
    if (PREDICT_FALSE(!tsd_inited_))
      return NULL;
#endif
    return GetThreadHeap();
  }

  inline ThreadCache *ThreadCache::GetFastPathCache()
  {
#ifndef HAVE_TLS
    return GetCacheIfPresent();
#else
    return threadlocal_data_.fast_path_heap;
#endif
  }

  inline void ThreadCache::SetUseEmergencyMalloc()
  {
#ifdef HAVE_TLS
    threadlocal_data_.fast_path_heap = NULL;
    threadlocal_data_.use_emergency_malloc = true;
#endif
  }

  inline void ThreadCache::ResetUseEmergencyMalloc()
  {
#ifdef HAVE_TLS
    ThreadCache *heap = threadlocal_data_.heap;
    threadlocal_data_.fast_path_heap = heap;
    threadlocal_data_.use_emergency_malloc = false;
#endif
  }

  inline bool ThreadCache::IsUseEmergencyMalloc()
  {
#if defined(HAVE_TLS) && defined(ENABLE_EMERGENCY_MALLOC)
    return PREDICT_FALSE(threadlocal_data_.use_emergency_malloc);
#else
    return false;
#endif
  }

  inline void ThreadCache::SetMaxSize(int32 new_max_size)
  {
    max_size_ = new_max_size;
  }

#ifndef NO_TCMALLOC_SAMPLES

  inline bool ThreadCache::SampleAllocation(size_t k)
  {
    return !sampler_.RecordAllocation(k);
  }

  inline bool ThreadCache::TryRecordAllocationFast(size_t k)
  {
    return sampler_.TryRecordAllocationFast(k);
  }

#else

  inline bool ThreadCache::SampleAllocation(size_t k)
  {
    return false;
  }

  inline bool ThreadCache::TryRecordAllocationFast(size_t k)
  {
    return true;
  }

#endif

} // namespace tcmalloc

#endif // TCMALLOC_THREAD_CACHE_H_
