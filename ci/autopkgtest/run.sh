#!/bin/sh
# ci/autopkgtest/run.sh
#
# Universal DESTDIR-staging wrapper for the downstream Debian autopkgtests.
# Points PATH / LD_LIBRARY_PATH / PKG_CONFIG_PATH at the staged install tree
# ($CIROOT) produced by `make stage-ciroot`, then runs the unmodified
# downstream scripts vendored under ci/autopkgtest/debian-tests/.
#
# Env in:
#   CIROOT             staging root        (default: $PWD/_ciroot)
#   CIPREFIX           configured prefix   (default: /usr)
#   TOP_BUILDDIR       build tree          (default: $PWD)
#   AUTOPKGTEST_BINDS  optional space-separated "host:guest" proot binds for
#                      scripts that read installed data via absolute paths
#                      (unused by pappl-retrofit).
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TESTS_DIR="$SCRIPT_DIR/debian-tests"

: "${CIROOT:=$PWD/_ciroot}"
: "${CIPREFIX:=/usr}"
: "${TOP_BUILDDIR:=$PWD}"

if [ ! -d "$CIROOT" ]; then
    echo "run.sh: staging root not found: $CIROOT (run 'make stage-ciroot' first)" >&2
    exit 1
fi

ROOT="$CIROOT$CIPREFIX"
MULTIARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null \
            || gcc -dumpmachine 2>/dev/null || echo "")

PATH="$ROOT/bin:$ROOT/sbin:$TOP_BUILDDIR:$TOP_BUILDDIR/.libs:$PATH"
LD_LIBRARY_PATH="$ROOT/lib${MULTIARCH:+:$ROOT/lib/$MULTIARCH}:$TOP_BUILDDIR/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
PKG_CONFIG_PATH="$ROOT/lib/pkgconfig${MULTIARCH:+:$ROOT/lib/$MULTIARCH/pkgconfig}:$ROOT/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export PATH LD_LIBRARY_PATH PKG_CONFIG_PATH

# Optional surgical /usr redirection (Fix C) for repos whose scripts read
# installed binaries/data via absolute paths.  No-op for pappl-retrofit.
if [ -n "${AUTOPKGTEST_BINDS:-}" ] && [ -z "${_UNDER_PROOT:-}" ]; then
    if command -v proot >/dev/null 2>&1; then
        binds=""
        for pair in $AUTOPKGTEST_BINDS; do binds="$binds -b $pair"; done
        _UNDER_PROOT=1; export _UNDER_PROOT
        # shellcheck disable=SC2086
        exec proot $binds -- "$0" "$@"
    fi
    echo "run.sh: AUTOPKGTEST_BINDS set but 'proot' not installed" >&2
    exit 1
fi

if [ "$#" -eq 0 ]; then
    echo "run.sh: usage: run.sh <test-name> [test-name...]" >&2
    exit 2
fi

rc=0
for name in "$@"; do
    script="$TESTS_DIR/$name"
    if [ ! -f "$script" ]; then
        echo "run.sh: no such test: $script" >&2
        rc=1
        continue
    fi
    chmod +x "$script" 2>/dev/null || true
    workdir=$(mktemp -d)
    echo "=== autopkgtest: $name (CIROOT=$CIROOT, prefix=$CIPREFIX) ==="
    if ( cd "$workdir" && "$script" ); then
        echo "=== PASS: $name ==="
    else
        rc=$?
        echo "=== FAIL: $name (exit $rc) ===" >&2
        rc=1
    fi
    rm -rf "$workdir"
done
exit $rc
