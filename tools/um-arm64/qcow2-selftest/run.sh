#!/usr/bin/env bash
# run.sh -- host-side selftest for the ubd qcow2 engine (qcow2_user.c).
#
# Builds the engine with the host shims and exercises four fixtures:
#   c  compressed (deflate) 8M image converted from a raw pattern
#   p  plain converted 8M image (standard clusters)
#   e  empty image, 4K clusters: allocator, new L2 tables, new refcount
#      blocks (a 12M write crosses the 2048-entry refcount block)
#   z  empty image, 64K clusters (the default)
#
# Judgement is external to the engine: qemu-img check after the dirty
# phase (accounting: no errors, no leaked clusters) and qemu-img convert
# -O raw byte-compared against the model the engine was driven against.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
DRV="$ROOT/arch/um/drivers"
CC=${CC:-cc}
W=$(mktemp -d "${TMPDIR:-/tmp}/qc2test.XXXXXX")
trap 'rm -rf "$W"' EXIT

echo "== build =="
$CC -O2 -Wall -Wextra -Wno-unused-parameter \
	-DQCOW2_SELFTEST -I "$HERE" -I "$HERE/shim" -I "$DRV" \
	"$HERE/test_qcow2.c" "$DRV/qcow2_user.c" "$DRV/qcow2_inflate_user.c" \
	-o "$W/test_qcow2"

echo "== fixtures =="
python3 - "$W" <<'PY'
import random, sys, os
w = sys.argv[1]
r = random.Random(42)
data = bytearray(r.randbytes(8 * 1024 * 1024))
for _ in range(20):                      # zero runs: the source is sparse-ish
    off = r.randrange(0, len(data) - 65536)
    data[off:off + 65536] = b"\0" * 65536
open(os.path.join(w, "raw8m"), "wb").write(data)
zf = open(os.path.join(w, "zeros64m"), "wb")
zf.truncate(64 * 1024 * 1024)
zf.close()
print("fixtures written")
PY
qemu-img convert -O qcow2 -c "$W/raw8m" "$W/c.qcow2"
qemu-img convert -O qcow2    "$W/raw8m" "$W/p.qcow2"
qemu-img create -f qcow2 -o cluster_size=4096 "$W/e.qcow2" 64M >/dev/null
qemu-img create -f qcow2 "$W/z.qcow2" 64M >/dev/null

run_one() {
	local name=$1 img=$2 src=$3

	echo "== $name: run =="
	"$W/test_qcow2" "$img" "$src" "$W/$name.expected" run
	echo "== $name: qemu-img check =="
	qemu-img check "$img" | tee "$W/$name.check"
	grep -q "No errors were found" "$W/$name.check"
	! grep -qiE "leaked|corrupt" "$W/$name.check"
	echo "== $name: verify (fresh open) =="
	"$W/test_qcow2" "$img" - "$W/$name.expected" verify
	echo "== $name: convert-back byte compare =="
	qemu-img convert -O raw "$img" "$W/$name.back.raw"
	cmp "$W/$name.back.raw" "$W/$name.expected"
	echo "== $name: OK =="
}

run_one c "$W/c.qcow2" "$W/raw8m"
run_one p "$W/p.qcow2" "$W/raw8m"
run_one e "$W/e.qcow2" "$W/zeros64m"
run_one z "$W/z.qcow2" "$W/zeros64m"

echo "ALL PASS"