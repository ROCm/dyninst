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
#include "BPatch_process.h"

#include "smoke_mutator.h"

#include <cstdio>

int main(int argc, char* argv[])
{
    if(argc != 2)
    {
        std::fprintf(stderr, "usage: %s <mutatee>\n", argv[0]);
        return 2;
    }

    const char* mutatee        = argv[1];
    const char* mutatee_argv[] = { mutatee, nullptr };

    BPatch          bpatch;
    BPatch_process* proc = bpatch.processCreate(mutatee, mutatee_argv);
    if(proc == nullptr)
    {
        std::fprintf(stderr, "error: processCreate('%s') failed\n", mutatee);
        return 1;
    }

    if(!smoke::insert_marker(proc)) return 1;

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
