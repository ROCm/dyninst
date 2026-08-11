/*
 * Runtime-instrumentation smoke test.
 *
 * Creates a process with BPatch::processCreate, inserts a call to the mutatee's
 * dyninst_marker() at the entry of work(), and runs it to completion. This is
 * the half of Dyninst that binary rewriting never touches: ProcControlAPI
 * process control, injection of libdyninstAPI_RT into a live process, and code
 * patching of a running mutatee.
 *
 * Mirrors the runtime_instrument mode of the rocprofiler-systems test suite, so
 * a break here shows up before the much slower downstream job runs.
 *
 * Usage: runtime_instrument <mutatee>
 */

#include "BPatch.h"
#include "BPatch_addressSpace.h"
#include "BPatch_function.h"
#include "BPatch_image.h"
#include "BPatch_point.h"
#include "BPatch_process.h"
#include "BPatch_snippet.h"

#include <cstdio>
#include <cstdlib>

namespace {

BPatch_function* find_only(BPatch_image* image, const char* name)
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

}  // namespace

int main(int argc, char* argv[])
{
    if(argc != 2)
    {
        std::fprintf(stderr, "usage: %s <mutatee>\n", argv[0]);
        return 2;
    }

    const char* mutatee   = argv[1];
    const char* mutatee_argv[] = { mutatee, nullptr };

    BPatch            bpatch;
    BPatch_process*   proc = bpatch.processCreate(mutatee, mutatee_argv);
    if(proc == nullptr)
    {
        std::fprintf(stderr, "error: processCreate('%s') failed\n", mutatee);
        return 1;
    }

    BPatch_image* image = proc->getImage();
    if(image == nullptr)
    {
        std::fprintf(stderr, "error: getImage() returned null\n");
        return 1;
    }

    BPatch_function* work   = find_only(image, "work");
    BPatch_function* marker = find_only(image, "dyninst_marker");
    if(work == nullptr || marker == nullptr) return 1;

    BPatch_Vector<BPatch_point*>* entry = work->findPoint(BPatch_locEntry);
    if(entry == nullptr || entry->empty())
    {
        std::fprintf(stderr, "error: no entry point found for work()\n");
        return 1;
    }

    BPatch_Vector<BPatch_snippet*> no_args;
    BPatch_funcCallExpr            call_marker(*marker, no_args);

    if(proc->insertSnippet(call_marker, *entry) == nullptr)
    {
        std::fprintf(stderr, "error: insertSnippet() failed\n");
        return 1;
    }

    if(!proc->continueExecution())
    {
        std::fprintf(stderr, "error: continueExecution() failed\n");
        return 1;
    }

    while(!proc->isTerminated())
        bpatch.waitForStatusChange();

    int exit_code = proc->getExitCode();
    std::fprintf(stderr, "mutatee terminated, exit code %d\n", exit_code);
    return exit_code;
}
