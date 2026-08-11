#!/usr/bin/env bash
#
# Smoke-test an installed Dyninst against a trivial mutatee.
#
# Two checks, covering the two halves of Dyninst that a link-only test misses:
#
#   binary rewrite      parseThat instruments and writes out a new binary, which
#                       is then executed
#   runtime instrument  a mutator built against the install uses
#                       BPatch::processCreate to instrument a live process
#
# The runtime check is the only one that exercises ProcControlAPI and the
# injection of libdyninstAPI_RT, so it fails on a class of breakage that binary
# rewriting cannot see.
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

export DYNINSTAPI_RT_LIB="${libdir}/libdyninstAPI_RT.so"

# Two paths, because the runtime check deliberately runs without ${libdir} on
# it. Measured against a Dyninst 13.0.0 install, BPatch::processCreate hung
# 10 times out of 10 with ${libdir} on LD_LIBRARY_PATH and roughly 1 in 10
# without it, so keeping the mutator's Dyninst libraries on the search path
# turns an occasional hang into a guaranteed one. The mutator is linked with an
# rpath to ${libdir}, so it resolves them without help; the third-party entries
# have to stay because libdyninstAPI.so's own dependencies are found through
# LD_LIBRARY_PATH, not through the mutator's rpath.
tpl_ld_path="${tpl_prefix:+${tpl_prefix}/elfutils/lib:${tpl_prefix}/tbb/lib}"

runtime_ld_path="${tpl_ld_path}"
if [[ -n "${LD_LIBRARY_PATH:-}" ]]; then
    runtime_ld_path="${runtime_ld_path:+${runtime_ld_path}:}${LD_LIBRARY_PATH}"
fi

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

# ---------------------------------------------------------------------------
# Runtime instrumentation
# ---------------------------------------------------------------------------

step "build the runtime-instrumentation mutator"
# Without this, a prefix missing its headers falls through to whatever Dyninst
# happens to sit in a default include path, and the failure arrives as a wall of
# template errors from the wrong version.
if [[ ! -f "${prefix}/include/BPatch.h" ]]; then
    echo "error: ${prefix}/include/BPatch.h not found; is --prefix an install tree?" >&2
    exit 1
fi
"${cxx}" -std=c++17 -g -O0 \
    -o "${workdir}/runtime_instrument" \
    "${script_dir}/runtime_instrument.cpp" \
    -I"${prefix}/include" \
    -L"${libdir}" -Wl,-rpath,"${libdir}" \
    -ldyninstAPI

step "instrument a live process with BPatch::processCreate"
# Logged unconditionally: a Dyninst lib directory reaching this variable is the
# one known cause of a hang here, and a container that sets LD_LIBRARY_PATH
# itself can reintroduce it without this script changing.
echo "runtime LD_LIBRARY_PATH: ${runtime_ld_path:-(unset)}"

# Only a hang is retried, and only when --retries asks for it. A mutator that
# runs to completion with the wrong output is a regression and must stay red.
attempt=0
while true; do
    run_logged "${workdir}/runtime.txt" \
        "${runtime_env[@]}" "${workdir}/runtime_instrument" "${workdir}/mutatee"

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
        echo "       check whether a Dyninst lib directory reached LD_LIBRARY_PATH" >&2
        echo "       (logged above); that reproduces this hang every time" >&2
    fi
    exit 1
done

# The mutatee never calls dyninst_marker() itself, so this line can only come
# from the snippet inserted at the entry of work().
grep -q 'dyninst-marker-ran' "${workdir}/runtime.txt"
grep -q 'mutatee-ok 42' "${workdir}/runtime.txt"

printf '\nall smoke checks passed\n'
