/*
 * Binary-rewrite smoke test.
 *
 * Opens the mutatee with BPatch::openBinary, inserts a call to its
 * dyninst_marker() at the entry of work(), and writes the result out. The
 * script then runs that binary and greps for the marker.
 *
 * This exists because "the rewritten binary still produces the right answer" is
 * not evidence that anything was instrumented: a rewriter that selects zero
 * functions and writes back a working copy passes that check. Downstream
 * rocprofiler-systems tests fail exactly that way, reporting a successful
 * rewrite whose output carries no instrumentation, so the assertion here has to
 * be the marker firing rather than the mutatee's own output.
 *
 * Usage: binary_rewrite <mutatee> <output>
 */

#include "BPatch.h"
#include "BPatch_binaryEdit.h"

#include "smoke_mutator.h"

#include <cstdio>

int main(int argc, char* argv[])
{
    if(argc != 3)
    {
        std::fprintf(stderr, "usage: %s <mutatee> <output>\n", argv[0]);
        return 2;
    }

    const char* mutatee = argv[1];
    const char* output  = argv[2];

    BPatch bpatch;

    BPatch_binaryEdit* binary = bpatch.openBinary(mutatee);
    if(binary == nullptr)
    {
        std::fprintf(stderr, "error: openBinary('%s') failed\n", mutatee);
        return 1;
    }

    if(!smoke::insert_marker(binary)) return 1;

    if(!binary->writeFile(output))
    {
        std::fprintf(stderr, "error: writeFile('%s') failed\n", output);
        return 1;
    }

    std::fprintf(stderr, "wrote instrumented binary to %s\n", output);
    return 0;
}
