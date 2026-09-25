/*
 * See the dyninst/COPYRIGHT file for copyright information.
 *
 * We provide the Paradyn Tools (below described as "Paradyn")
 * on an AS IS basis, and do not warrant its validity or performance.
 * We reserve the right to update, modify, or discontinue this
 * software at any time.  We shall have no obligation to supply such
 * updates or modifications or any other form of support to you.
 *
 * By your use of Paradyn, you understand and agree that we (or any
 * other person or entity with proprietary rights in Paradyn) are
 * under no obligation to provide either maintenance services,
 * update services, notices of latent defects, or correction of
 * defects for Paradyn.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "vgannotations.h"
#include "concurrent.h"

#if defined(_OPENMP)
#include <omp.h>
#endif

#include <iostream>

using namespace Dyninst;

thread_local dyn_thread dyn_thread::me;

dyn_thread::dyn_thread() {}

unsigned int dyn_thread::getId() {
#if defined(_OPENMP) 
    return omp_get_thread_num();
#else
    return 0;
#endif
}

unsigned int dyn_thread::threads() {
#if defined(_OPENMP)
    return omp_get_num_threads();
#else
    return 1;
#endif
}

#ifndef ENABLE_VG_ANNOTATIONS
void dyn_c_annotations::rwinit(void*) {}
void dyn_c_annotations::rwdeinit(void*) {}
void dyn_c_annotations::wlock(void*) {}
void dyn_c_annotations::wunlock(void*) {}
void dyn_c_annotations::rlock(void*) {}
void dyn_c_annotations::runlock(void*) {}
#else
void dyn_c_annotations::rwinit(void* ptr) {
    ANNOTATE_RWLOCK_CREATE(ptr);
}
void dyn_c_annotations::rwdeinit(void* ptr) {
    ANNOTATE_RWLOCK_DESTROY(ptr);
}
void dyn_c_annotations::wlock(void* ptr) {
    ANNOTATE_RWLOCK_ACQUIRED(ptr, 1 /* writer mode */);
    ANNOTATE_HAPPENS_AFTER(static_cast<char*>(ptr) + 1);  // After all the readers
}
void dyn_c_annotations::wunlock(void* ptr) {
    ANNOTATE_HAPPENS_BEFORE(static_cast<char*>(ptr) + 2);
    ANNOTATE_RWLOCK_RELEASED(ptr, 1 /* writer mode */);
}
void dyn_c_annotations::rlock(void* ptr) {
    ANNOTATE_RWLOCK_ACQUIRED(ptr, 0 /* reader mode */);
    ANNOTATE_HAPPENS_AFTER(static_cast<char*>(ptr) + 2);  // After the last writer
}
void dyn_c_annotations::runlock(void* ptr) {
    ANNOTATE_HAPPENS_BEFORE(static_cast<char*>(ptr) + 1);
    ANNOTATE_RWLOCK_RELEASED(ptr, 0 /* reader mode */);
}
#endif

#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>

#include <cstdlib>
#include <cstring>

// Parsing a large binary performs on the order of 10^8 small allocations. By
// default glibc extends the main heap in 128KB brk() increments, which turns
// into thousands of syscalls plus the page faults that follow them. TBB used to
// hide this because libtbbmalloc_proxy replaced malloc process-wide and served
// everything from large mmap'd regions; with TBB gone the parse step regresses
// roughly 20% in wall time at high thread counts. Asking glibc for larger
// growth increments recovers it.
//
// Only the mutator links libcommon, so this never perturbs the allocator of a
// process under instrumentation.
namespace {

bool dyn_malloc_tuning_disabled() {
    if(const char* v = std::getenv("DYNINST_MALLOC_TUNING"))
        if(std::strcmp(v, "0") == 0)
            return true;

    // An explicit setting from the user wins; glibc has already applied it.
    if(std::getenv("MALLOC_TOP_PAD_"))
        return true;
    if(const char* t = std::getenv("GLIBC_TUNABLES"))
        if(std::strstr(t, "glibc.malloc.top_pad"))
            return true;

    return false;
}

struct dyn_malloc_tuner {
    dyn_malloc_tuner() {
        if(dyn_malloc_tuning_disabled())
            return;
        mallopt(M_TOP_PAD, 32 * 1024 * 1024);
    }
};

dyn_malloc_tuner const dyn_malloc_tuner_instance;

} // namespace
#endif
