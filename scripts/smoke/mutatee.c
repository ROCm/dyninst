/*
 * Target program for the CI smoke tests. Shared by the binary-rewrite check and
 * the runtime-instrumentation check.
 *
 * dyninst_marker() is deliberately never called. Its output appears only if a
 * mutator inserted a call to it, which makes "did instrumentation actually
 * execute" a grep rather than an inference from a tool's exit code.
 */

#include <stdio.h>

__attribute__((noinline, used)) void dyninst_marker(void)
{
    printf("dyninst-marker-ran\n");
    fflush(stdout);
}

__attribute__((noinline, used)) int work(int x)
{
    return x * 2;
}

int main(void)
{
    printf("mutatee-ok %d\n", work(21));
    fflush(stdout);
    return 0;
}
