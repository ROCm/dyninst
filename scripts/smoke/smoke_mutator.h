/*
 * Shared by the two smoke-test mutators.
 *
 * Both checks insert the same snippet into the same mutatee and differ only in
 * how the address space is obtained and what is done with it afterwards, so the
 * instrumentation itself lives here rather than being written twice.
 */

#pragma once

#include "BPatch_addressSpace.h"
#include "BPatch_function.h"
#include "BPatch_image.h"
#include "BPatch_point.h"
#include "BPatch_snippet.h"

#include <cstdio>

namespace smoke
{

inline BPatch_function* find_only(BPatch_image* image, const char* name)
{
    BPatch_Vector<BPatch_function*> funcs;
    image->findFunction(name, funcs);

    // Anything other than exactly one match means the mutatee was built
    // differently than this test assumes, not that Dyninst is broken.
    if(funcs.size() != 1)
    {
        std::fprintf(stderr, "error: expected exactly one '%s', found %lu\n", name,
                     static_cast<unsigned long>(funcs.size()));
        return nullptr;
    }
    return funcs[0];
}

// Inserts a call to the mutatee's dyninst_marker() at the entry of work().
inline bool insert_marker(BPatch_addressSpace* aspace)
{
    BPatch_image* image = aspace->getImage();
    if(image == nullptr)
    {
        std::fprintf(stderr, "error: getImage() returned null\n");
        return false;
    }

    BPatch_function* work   = find_only(image, "work");
    BPatch_function* marker = find_only(image, "dyninst_marker");
    if(work == nullptr || marker == nullptr) return false;

    BPatch_Vector<BPatch_point*>* entry = work->findPoint(BPatch_locEntry);
    if(entry == nullptr || entry->empty())
    {
        std::fprintf(stderr, "error: no entry point found for work()\n");
        return false;
    }

    BPatch_Vector<BPatch_snippet*> no_args;
    BPatch_funcCallExpr            call_marker(*marker, no_args);

    if(aspace->insertSnippet(call_marker, *entry) == nullptr)
    {
        std::fprintf(stderr, "error: insertSnippet() failed\n");
        return false;
    }

    return true;
}

}  // namespace smoke
