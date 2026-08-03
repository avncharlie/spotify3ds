#!/usr/bin/env bash
#
# Build -> run in Azahar -> read results -> report.
#
# Usage:
#   ./dev.sh              build, run headless, print verdict
#   ./dev.sh --build      build only
#   ./dev.sh --log        also dump the guest debug log
#   ./dev.sh --timeout N  seconds to wait for DONE (default 40)
#
set -uo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="$(basename "$PROJECT_DIR")"
THREEDSX="$PROJECT_DIR/$TARGET.3dsx"

AZAHAR_APP="/Applications/Azahar.app"
AZAHAR_DATA="$HOME/Library/Application Support/Azahar"
SDMC="$AZAHAR_DATA/sdmc"
RESULT_FILE="$SDMC/testresult.txt"
GUEST_LOG="$SDMC/spotify/log.txt"
EMU_LOG="$AZAHAR_DATA/log/azahar_log.txt"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-/opt/devkitpro/devkitARM}"

BUILD_ONLY=0
SHOW_LOG=0
TIMEOUT=40
while [[ $# -gt 0 ]]; do
	case "$1" in
		--build)   BUILD_ONLY=1; shift ;;
		--log)     SHOW_LOG=1; shift ;;
		--timeout) TIMEOUT="$2"; shift 2 ;;
		*) echo "unknown arg: $1" >&2; exit 2 ;;
	esac
done

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
dim()   { printf '\033[2m%s\033[0m\n' "$*"; }

stop_azahar() {
	pkill -f "MacOS/azahar" 2>/dev/null
	for _ in 1 2 3 4 5; do
		pgrep -f "MacOS/azahar" >/dev/null || return 0
		sleep 1
	done
	pkill -9 -f "MacOS/azahar" 2>/dev/null
	sleep 1
}

# ---------------------------------------------------------------- build
echo "==> building $TARGET"
if ! make -C "$PROJECT_DIR" -j8 2>&1 | tail -20; then
	red "BUILD FAILED"
	exit 1
fi
[[ -f "$THREEDSX" ]] || { red "BUILD FAILED: no $TARGET.3dsx"; exit 1; }
green "built $(basename "$THREEDSX")"
[[ $BUILD_ONLY -eq 1 ]] && exit 0

# ---------------------------------------------------------------- run
stop_azahar
rm -f "$RESULT_FILE"
mkdir -p "$SDMC"

echo "==> launching Azahar (timeout ${TIMEOUT}s)"
# Must launch via `open -a`: invoking Contents/MacOS/azahar directly triggers a
# modal dialog and hangs, and disables camera emulation.
open -a "$AZAHAR_APP" --args -w "$THREEDSX"

deadline=$((SECONDS + TIMEOUT))
while (( SECONDS < deadline )); do
	if [[ -f "$RESULT_FILE" ]] && grep -q '^DONE$' "$RESULT_FILE" 2>/dev/null; then
		break
	fi
	sleep 1
done

stop_azahar

# ---------------------------------------------------------------- report
echo
if [[ ! -f "$RESULT_FILE" ]]; then
	red "NO RESULT FILE — app never wrote to sdmc:/testresult.txt"
	dim "last emulator log lines:"
	tail -15 "$EMU_LOG" 2>/dev/null
	exit 1
fi

echo "==> results"
cat "$RESULT_FILE"
echo

# NB: `grep -c ... || echo 0` yields "0\n0" when grep matches nothing (it still
# prints 0 *and* exits 1), which breaks the arithmetic below. Count with a
# pipeline that always succeeds instead.
fails=$(grep 'RESULT=FAIL' "$RESULT_FILE" 2>/dev/null | wc -l | tr -d ' ')
done_ok=$(grep '^DONE$' "$RESULT_FILE" 2>/dev/null | wc -l | tr -d ' ')

if [[ $SHOW_LOG -eq 1 && -f "$GUEST_LOG" ]]; then
	echo "==> guest log"
	tail -40 "$GUEST_LOG"
	echo
fi

if [[ "$done_ok" -eq 0 ]]; then
	red "HUNG OR CRASHED — no DONE sentinel"
	dim "last emulator log lines:"
	tail -15 "$EMU_LOG" 2>/dev/null
	exit 1
elif [[ "$fails" -gt 0 ]]; then
	red "$fails step(s) FAILED"
	exit 1
else
	green "ALL STEPS PASSED"
fi
