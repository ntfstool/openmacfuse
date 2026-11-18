#!/bin/bash
# Smoke test: start openmacfuse-m0 -> mount with the system mount_nfs -> ls / cat / checksum -> umount -> server exits cleanly
#
# Copyright 2026 The OpenMacFUSE Authors
# SPDX-License-Identifier: MIT
#
# Usage: tests/m0_smoke.sh [tcp|unix|all]   (default all: one run over a TCP port and one over a local unix socket)
# No root needed; mount point and socket live in a temporary directory under $TMPDIR and are cleaned up at the end.
# Every step has a timeout; if still mounted when the script exits it force-unmounts, leaving no zombie mount.

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
BIN="$HERE/build/openmacfuse-m0"
MODE=${1:-all}
EXPORT=OPENMACFUSEM0

PASS=0
FAIL=0
ok()   { echo "  [PASS] $*"; PASS=$((PASS + 1)); }
bad()  { echo "  [FAIL] $*"; FAIL=$((FAIL + 1)); }
# Run with a timeout (perl ships with macOS; no dependency on timeout from coreutils)
T()    { perl -e 'alarm shift; exec @ARGV or exit 127' "$@"; }

if [ ! -x "$BIN" ]; then
	echo "building openmacfuse-m0 ..."
	make -C "$HERE" >/dev/null || { echo "build failed"; exit 1; }
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/openmacfusem0.XXXXXX")
# The mount table records the physical path (/var -> /private/var): resolve before comparing
WORK=$(cd "$WORK" && pwd -P)
MNT="$WORK/mnt"
mkdir -p "$MNT"
SRV_PID=

cleanup() {
	if mount | grep -q " on $MNT "; then
		echo "  cleanup: still mounted, force-unmounting"
		T 15 umount -f "$MNT" 2>/dev/null || T 15 diskutil unmount force "$MNT" >/dev/null 2>&1
	fi
	if [ -n "$SRV_PID" ] && kill -0 "$SRV_PID" 2>/dev/null; then
		kill -INT "$SRV_PID" 2>/dev/null
		sleep 0.5
		kill -9 "$SRV_PID" 2>/dev/null
	fi
	rm -rf "$WORK"
}
trap cleanup EXIT

# MD5 of big.bin computed with the formula in backend_mem.c
EXPECT_MD5=$(perl -MDigest::MD5 -e 'my $s=""; for my $i (0..1048575){ $s.=chr((($i*31)+($i>>8)) & 0xff) } print Digest::MD5::md5_hex($s)')

run_one() {
	local mode=$1 spec opts line
	echo "== mode: $mode ($(sw_vers -productVersion), $(uname -m), user $(id -un))"
	: > "$WORK/srv.out"
	if [ "$mode" = unix ]; then
		"$BIN" -n "$EXPORT" -U "$WORK/n.sock" > "$WORK/srv.out" 2> "$WORK/srv.err" &
	else
		"$BIN" -n "$EXPORT" -p 0 > "$WORK/srv.out" 2> "$WORK/srv.err" &
	fi
	SRV_PID=$!
	for _ in $(seq 1 50); do
		line=$(head -1 "$WORK/srv.out" 2>/dev/null)
		[ -n "$line" ] && break
		sleep 0.1
	done
	[ -n "$line" ] && ok "server started: $line" || { bad "server did not start"; return; }

	if [ "$mode" = unix ]; then
		# local socket: netid is ticotsord, nocallback is mandatory (otherwise mount returns EINVAL)
		spec="<$WORK/n.sock>:/$EXPORT"
		opts="vers=4.0,proto=ticotsord,nocallback"
	else
		spec="127.0.0.1:/$EXPORT"
		opts="vers=4.0,tcp,port=$(echo "$line" | awk '{print $4}')"
	fi
	opts="$opts,rwsize=262144,soft,intr,timeo=10,retrans=3,nobrowse"
	local t0=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
	if T 30 /sbin/mount_nfs -o "$opts" "$spec" "$MNT" 2> "$WORK/mount.err"; then
		local t1=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
		ok "mount_nfs succeeded ($(perl -e "printf '%.3f', $t1-$t0") s): $(mount | grep " on $MNT " | sed 's/ on .*(/ (/')"
	else
		bad "mount_nfs failed: $(cat "$WORK/mount.err")"
		return
	fi

	local ls_out
	ls_out=$(T 20 ls -la "$MNT" 2>&1)
	echo "$ls_out" | sed 's/^/      /'
	for f in hello.txt big.bin sub link many $'\xe4\xb8\xad\xe6\x96\x87\xe5\x90\x8d.txt'; do
		echo "$ls_out" | grep -q " $f\( \|$\)" && ok "ls -la shows $f" || bad "ls -la does not show $f"
	done
	[ "$(T 20 cat "$MNT/hello.txt")" = "Hello from openmacfuse M0" ] && ok "cat hello.txt content correct" || bad "cat hello.txt content wrong"
	[ "$(T 20 cat "$MNT/"$'\xe4\xb8\xad\xe6\x96\x87\xe5\x90\x8d.txt')" = $'\xe4\xb8\xad\xe6\x96\x87\xe5\x86\x85\xe5\xae\xb9' ] && ok "UTF-8 file name readable" || bad "reading the UTF-8 file name failed"
	[ "$(T 20 cat "$MNT/sub/inner.txt")" = "inner file" ] && ok "file in subdirectory readable" || bad "reading the file in the subdirectory failed"
	[ "$(T 20 readlink "$MNT/link")" = "hello.txt" ] && ok "readlink correct" || bad "readlink wrong"
	local md5
	md5=$(T 60 md5 -q "$MNT/big.bin")
	[ "$md5" = "$EXPECT_MD5" ] && ok "big.bin (1 MiB, assembled from several READs) MD5 matches" || bad "big.bin MD5 mismatch: $md5 != $EXPECT_MD5"
	local n
	n=$(T 60 ls "$MNT/many" | wc -l | tr -d ' ')
	[ "$n" = "3000" ] && ok "3000-entry directory fully listed across pages ($n)" || bad "large directory entry count $n != 3000"
	T 20 stat "$MNT/no-such-file" >/dev/null 2>&1 && bad "stat of a nonexistent file succeeded" || ok "nonexistent file returns ENOENT"

	if T 30 umount "$MNT" 2> "$WORK/umount.err"; then
		ok "umount succeeded"
	else
		bad "umount failed: $(cat "$WORK/umount.err")"
	fi
	mount | grep -q " on $MNT " && bad "still in the mount table after unmounting" || ok "mount point no longer in the mount table"

	kill -INT "$SRV_PID"
	local rc=
	for _ in $(seq 1 30); do
		kill -0 "$SRV_PID" 2>/dev/null || { wait "$SRV_PID"; rc=$?; break; }
		sleep 0.1
	done
	[ "$rc" = "0" ] && ok "server exited normally after SIGINT (exit code 0)" || bad "server did not exit normally (rc=${rc:-still running})"
	SRV_PID=
	grep -E "COMPOUND|failed [1-9]" "$WORK/srv.err" | sed 's/^/      /'
}

case "$MODE" in
	tcp)  run_one tcp ;;
	unix) run_one unix ;;
	all)  run_one tcp; run_one unix ;;
	*)    echo "usage: $0 [tcp|unix|all]"; exit 2 ;;
esac

echo "== result: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && echo "smoke test: PASS" || echo "smoke test: FAIL"
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
