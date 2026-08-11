#!/usr/bin/env bash
#
# Smoke-test an installed Dyninst against a trivial mutatee.
#
# Two checks, covering the two halves of Dyninst that a link-only test misses:
#
#   binary rewrite      parseThat writes out a new binary, and a mutator built
#                       against the install writes out another with a snippet of
#                       our own in it
#   runtime instrument  a mutator built against the install uses
#                       BPatch::processCreate to instrument a live process
#
# The runtime check is the only one that exercises ProcControlAPI and the
# injection of libdyninstAPI_RT, so it fails on a class of breakage that binary
# rewriting cannot see.
#
# Both checks assert that an inserted snippet ran rather than that the tools
# exited zero and the mutatee still works. A rewriter that selects no functions
# at all satisfies the weaker condition, and that is the shape of the binary
# rewriting failures seen downstream in rocprofiler-systems.
#
# Every Dyninst invocation is wrapped in a timeout. ProcControl process startup
# has been observed to hang rather than fail: the mutatee sits in ptrace_stop
# while the mutator's event thread waits on a futex that nothing will post, so
# without a bound the job stalls until the runner kills it and the log ends
# mid-step with no indication of which stage was to blame.
#
# Usage:
#   run-smoke.sh --prefix DIR [--tpl-prefix DIR] [--cc CC] [--cxx CXX]
#                [--workdir DIR] [--timeout SECONDS] [--retries N]
#
#   --prefix      Dyninst install prefix (must contain bin/parseThat and
#                 lib/libdyninstAPI_RT.so)
#   --tpl-prefix  third-party library prefix from build-tpls.sh, added to
#                 LD_LIBRARY_PATH so the install can resolve elfutils and TBB
#   --timeout     per-invocation limit in seconds (default 120)
#   --retries     extra attempts for the runtime check only, which is the one
#                 stage observed to hang nondeterministically (default 0)

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

prefix=""
tpl_prefix=""
cc="${CC:-gcc}"
cxx="${CXX:-g++}"
workdir=""
timeout_secs=120
retries=0

usage() {
    sed -n '3,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --prefix) prefix="$2"; shift 2 ;;
        --tpl-prefix) tpl_prefix="$2"; shift 2 ;;
        --cc) cc="$2"; shift 2 ;;
        --cxx) cxx="$2"; shift 2 ;;
        --workdir) workdir="$2"; shift 2 ;;
        --timeout) timeout_secs="$2"; shift 2 ;;
        --retries) retries="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "${prefix}" ]]; then
    echo "error: --prefix is required" >&2
    exit 2
fi
prefix="$(cd "${prefix}" && pwd)"

if [[ -z "${workdir}" ]]; then
    workdir="$(mktemp -d)"
    trap 'rm -rf "${workdir}"' EXIT
fi
mkdir -p "${workdir}"

libdir="${prefix}/lib"
[[ -d "${libdir}" ]] || libdir="${prefix}/lib64"

# Honoured by the ppc, aarch64 and FreeBSD backends. The x86_64 Linux backend
# ignores it and resolves the runtime library by search instead, which is what
# DYNINST_REWRITER_PATHS below is for.
export DYNINSTAPI_RT_LIB="${libdir}/libdyninstAPI_RT.so"

# How PCProcess::setEnvPreload finds the library it LD_PRELOADs into the
# mutatee. On x86_64 Linux it calls BinaryEdit::getResolvedLibraryPath with the
# bare name "libdyninstAPI_RT.so" and takes the first hit from, in order, this
# variable, LD_LIBRARY_PATH, and the compiler's search directories. Naming
# ${libdir} here pins the runtime library to the install under test and lets the
# runtime check keep ${libdir} off LD_LIBRARY_PATH: without it the search either
# finds nothing, and process creation fails during bootstrap with no explanation
# unless DYNINST_DEBUG_STARTUP is set, or finds some other Dyninst that happens
# to be on the path and instruments the mutatee with the wrong runtime.
export DYNINST_REWRITER_PATHS="${libdir}"

tpl_ld_path="${tpl_prefix:+${tpl_prefix}/elfutils/lib:${tpl_prefix}/tbb/lib}"

# Two paths, because the runtime check runs without ${libdir} on it: the mutator
# reaches its own libraries through the RPATH cmake links it with, and leaving
# the directory off means nothing on the loader's search path can stand in for
# the install being tested. The third-party entries have to stay, because
# libdyninstAPI.so's own dependencies resolve through LD_LIBRARY_PATH rather
# than through the mutator's RUNPATH, which is not transitive.
#
# The inherited value is filtered the same way rather than trusted, since a
# developer shell or a container image can point it at a different Dyninst.
strip_dyninst_dirs() {
    local result="" entry
    local -a entries=()
    IFS=':' read -r -a entries <<< "$1"
    for entry in "${entries[@]}"; do
        [[ -z "${entry}" ]] && continue
        if compgen -G "${entry}/libdyninstAPI*.so*" > /dev/null; then
            echo "note: dropping Dyninst library directory '${entry}' from the runtime search path" >&2
            continue
        fi
        result="${result:+${result}:}${entry}"
    done
    printf '%s' "${result}"
}

runtime_ld_path="${tpl_ld_path}"
if [[ -n "${LD_LIBRARY_PATH:-}" ]]; then
    runtime_ld_path="${runtime_ld_path:+${runtime_ld_path}:}${LD_LIBRARY_PATH}"
fi
runtime_ld_path="$(strip_dyninst_dirs "${runtime_ld_path}")"

if [[ -n "${runtime_ld_path}" ]]; then
    runtime_env=(env "LD_LIBRARY_PATH=${runtime_ld_path}")
else
    # An empty LD_LIBRARY_PATH is not the same as an unset one: the loader reads
    # the empty entry as the current directory.
    runtime_env=(env -u LD_LIBRARY_PATH)
fi

export LD_LIBRARY_PATH="${libdir}${runtime_ld_path:+:${runtime_ld_path}}"

if [[ ! -f "${DYNINSTAPI_RT_LIB}" ]]; then
    echo "error: ${DYNINSTAPI_RT_LIB} not found" >&2
    exit 1
fi

step() { printf '\n=== %s ===\n' "$1"; }

# SIGKILL rather than the default SIGTERM: a mutator wedged inside ProcControl
# is not reliably servicing signals, and a timeout that itself hangs is worse
# than no timeout at all. GNU timeout then reports 137 instead of 124.
is_timeout() { [[ "$1" -eq 124 || "$1" -eq 137 ]]; }

# Runs a command under the timeout and tees its output, leaving the status in
# run_rc. errexit is lifted around the pipeline so the caller can tell a hang
# apart from a non-zero exit instead of the script dying on the spot.
run_logged() {
    local logfile="$1"
    shift
    set +e
    timeout -s KILL "${timeout_secs}" "$@" 2>&1 | tee "${logfile}"
    run_rc="${PIPESTATUS[0]}"
    set -e
}

report_failure() {
    if is_timeout "$2"; then
        echo "error: $1 did not finish within ${timeout_secs}s" >&2
        echo "       this is a hang, not a failed assertion; see ${3}" >&2
    else
        echo "error: $1 exited $2" >&2
    fi
}

# ---------------------------------------------------------------------------
# Baseline
# ---------------------------------------------------------------------------

step "build the mutatee"
# -O0 keeps work() and dyninst_marker() as distinct, findable functions.
"${cc}" -g -O0 -o "${workdir}/mutatee" "${script_dir}/mutatee.c"

step "run the mutatee uninstrumented"
run_logged "${workdir}/baseline.txt" "${workdir}/mutatee"
if [[ "${run_rc}" -ne 0 ]]; then
    report_failure "the uninstrumented mutatee" "${run_rc}" "${workdir}/baseline.txt"
    exit 1
fi
grep -q 'mutatee-ok 42' "${workdir}/baseline.txt"
# Nothing has been instrumented yet, so the marker must be silent. If this
# fires, the mutatee itself calls dyninst_marker() and the runtime check below
# would pass without Dyninst doing anything.
if grep -q 'dyninst-marker-ran' "${workdir}/baseline.txt"; then
    echo "error: marker ran without instrumentation" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Mutators
# ---------------------------------------------------------------------------

step "build the mutators"
# Without this, a prefix missing its headers falls through to whatever Dyninst
# happens to sit in a default include path, and the failure arrives as a wall of
# template errors from the wrong version.
if [[ ! -f "${prefix}/include/BPatch.h" ]]; then
    echo "error: ${prefix}/include/BPatch.h not found; is --prefix an install tree?" >&2
    exit 1
fi

# See scripts/smoke/CMakeLists.txt for why this is a CMake build and not a
# compiler invocation. The mutators end up with a build-tree RPATH covering the
# install's libraries, which is what lets the runtime check below drop them from
# LD_LIBRARY_PATH.
mutator_build="${workdir}/mutators"
cmake_args=(
    -S "${script_dir}"
    -B "${mutator_build}"
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DCMAKE_CXX_COMPILER="${cxx}"
    -DCMAKE_PREFIX_PATH="${prefix}"
)
if [[ -n "${tpl_prefix}" ]]; then
    cmake_args+=(
        -DTBB_ROOT_DIR="${tpl_prefix}/tbb"
        -DElfUtils_ROOT_DIR="${tpl_prefix}/elfutils"
    )
fi

run_logged "${workdir}/cmake.txt" cmake "${cmake_args[@]}"
if [[ "${run_rc}" -ne 0 ]]; then
    report_failure "configuring the mutators" "${run_rc}" "${workdir}/cmake.txt"
    exit 1
fi

run_logged "${workdir}/cmake-build.txt" cmake --build "${mutator_build}" --parallel
if [[ "${run_rc}" -ne 0 ]]; then
    report_failure "building the mutators" "${run_rc}" "${workdir}/cmake-build.txt"
    exit 1
fi

# ---------------------------------------------------------------------------
# Binary rewrite
# ---------------------------------------------------------------------------

step "rewrite the mutatee with parseThat"
# -i 1 instruments function entries, so this covers instrumentation rather than
# only parse-and-write-back.
run_logged "${workdir}/rewrite.txt" \
    "${prefix}/bin/parseThat" -i 1 \
    --binary-edit="${workdir}/mutatee.rewritten" \
    "${workdir}/mutatee"
if [[ "${run_rc}" -ne 0 ]]; then
    report_failure "parseThat" "${run_rc}" "${workdir}/rewrite.txt"
    exit 1
fi

step "run the rewritten mutatee"
test -x "${workdir}/mutatee.rewritten"
run_logged "${workdir}/rewritten.txt" "${workdir}/mutatee.rewritten"
if [[ "${run_rc}" -ne 0 ]]; then
    report_failure "the rewritten mutatee" "${run_rc}" "${workdir}/rewritten.txt"
    exit 1
fi
grep -q 'mutatee-ok 42' "${workdir}/rewritten.txt"

step "rewrite the mutatee with a snippet of our own"
# parseThat above shows the rewriter produces a working binary, which it also
# does when it instruments nothing at all. This inserts a call we can observe so
# the run below can tell those two outcomes apart.
run_logged "${workdir}/rewrite-snippet.txt" \
    "${mutator_build}/binary_rewrite" "${workdir}/mutatee" "${workdir}/mutatee.snippet"
if [[ "${run_rc}" -ne 0 ]]; then
    report_failure "binary rewriting" "${run_rc}" "${workdir}/rewrite-snippet.txt"
    exit 1
fi

step "run the rewritten mutatee carrying the snippet"
test -x "${workdir}/mutatee.snippet"
run_logged "${workdir}/snippet.txt" "${workdir}/mutatee.snippet"
if [[ "${run_rc}" -ne 0 ]]; then
    report_failure "the rewritten mutatee" "${run_rc}" "${workdir}/snippet.txt"
    exit 1
fi
# The mutatee never calls dyninst_marker() itself, so this line can only come
# from the snippet written into the entry of work().
grep -q 'dyninst-marker-ran' "${workdir}/snippet.txt"
grep -q 'mutatee-ok 42' "${workdir}/snippet.txt"

# ---------------------------------------------------------------------------
# Runtime instrumentation
# ---------------------------------------------------------------------------

step "instrument a live process with BPatch::processCreate"
# Logged unconditionally, so that a hang here can be read against the search
# path that produced it without having to reconstruct what was filtered.
echo "runtime LD_LIBRARY_PATH: ${runtime_ld_path:-(unset)}"

# Only a hang is retried, and only when --retries asks for it. A mutator that
# runs to completion with the wrong output is a regression and must stay red.
attempt=0
while true; do
    run_logged "${workdir}/runtime.txt" \
        "${runtime_env[@]}" "${mutator_build}/runtime_instrument" "${workdir}/mutatee"

    if [[ "${run_rc}" -eq 0 ]]; then
        break
    fi

    if is_timeout "${run_rc}" && [[ "${attempt}" -lt "${retries}" ]]; then
        attempt=$((attempt + 1))
        echo "warning: runtime instrumentation hung, retrying (${attempt}/${retries})" >&2
        continue
    fi

    report_failure "runtime instrumentation" "${run_rc}" "${workdir}/runtime.txt"
    if is_timeout "${run_rc}"; then
        echo "       re-run with DYNINST_DEBUG_STARTUP=1 DYNINST_DEBUG_PROCCONTROL=1" >&2
        echo "       to see which runtime library was injected and how far" >&2
        echo "       bootstrap got before it stopped" >&2
    fi
    exit 1
done

# The mutatee never calls dyninst_marker() itself, so this line can only come
# from the snippet inserted at the entry of work().
grep -q 'dyninst-marker-ran' "${workdir}/runtime.txt"
grep -q 'mutatee-ok 42' "${workdir}/runtime.txt"

printf '\nall smoke checks passed\n'
