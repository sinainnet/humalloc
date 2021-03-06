// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright 2009 Google Inc. All Rights Reserved.
// Author: fikes@google.com (Andrew Fikes)
//
// Use of this source code is governed by a BSD-style license that can
// be found in the LICENSE file.

#include "config_for_unittests.h"

#include <stdio.h>

#include <memory>

#include "page_heap.h"
#include "system-alloc.h"
#include "base/logging.h"
#include "common.h"

DECLARE_int64(tcmalloc_heap_limit_mb);

namespace {

// The system will only release memory if the block size is equal or hight than
// system page size.
static bool HaveSystemRelease =
    TCMalloc_SystemRelease(
      TCMalloc_SystemAlloc(getpagesize(), NULL, 0), getpagesize());

static void CheckStats(const tcmalloc::PageHeap* ph,
                       uint64_t system_pages,
                       uint64_t free_pages,
                       uint64_t unmapped_pages) {
}

static void TestPageHeap_Stats() {
}

// The number of kMaxPages-sized Spans we will allocate and free during the
// tests.
// We will also do twice this many kMaxPages/2-sized ones.
static constexpr int kNumberMaxPagesSpans = 10;

// Allocates all the last-level page tables we will need. Doing this before
// calculating the base heap usage is necessary, because otherwise if any of
// these are allocated during the main test it will throw the heap usage
// calculations off and cause the test to fail.
static void AllocateAllPageTables() {
}

static void TestPageHeap_Limit() {
}

}  // namespace

int main(int argc, char **argv) {
  TestPageHeap_Stats();
  TestPageHeap_Limit();
  printf("PASS\n");
  // on windows as part of library destructors we call getenv which
  // calls malloc which fails due to our exhausted heap limit. It then
  // causes fancy stack overflow because log message we're printing
  // for failed allocation somehow cause malloc calls too
  //
  // To keep us out of trouble we just drop malloc limit
  FLAGS_tcmalloc_heap_limit_mb = 0;
  return 0;
}
