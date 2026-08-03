#!/usr/bin/env bash
#
# Build and verify, against either the emulator or a real console.
#
#   ./dev.sh                    emulator (Azahar)
#   ./dev.sh --hw               real hardware over 3dslink
#   ./dev.sh --hw --ip A.B.C.D  override the device address
#   ./dev.sh --build            build only
#   ./dev.sh --log              also dump the guest debug log
#   ./dev.sh --timeout N        seconds to wait for DONE
#
# Both targets share one verdict parser, so a pass means the same thing on
# each. Exit codes are distinct so the loop is scriptable:
#
#   0  PASS
#   1  FAIL (a step failed) or HUNG (no DONE before the deadline)
#   2  NOT LAUNCHED (3dslink could not reach the console)
#   3  NO OUTPUT (uploaded, but the app printed nothing - early crash)
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
HW_LOG="$PROJECT_DIR/.hwrun.log"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-/opt/devkitpro/devkitARM}"

MODE=emu
BUILD_ONLY=0
SHOW_LOG=0
TIMEOUT=""
IP_ARG=""
# 3dslink retries its connect; this bounds how long we wait for the console to
# answer before calling it NOT LAUNCHED, separately from the run timeout.
CONNECT_DEADLINE=25

while [[ $# -gt 0 ]]; do
	case "$1" in
		--hw)      MODE=hw; shift ;;
		--build)   BUILD_ONLY=1; shift ;;
		--log)     SHOW_LOG=1; shift ;;
		--timeout) TIMEOUT="$2"; shift 2 ;;
		--ip)      IP_ARG="$2"; shift 2 ;;
		*) echo "unknown arg: $1" >&2; exit 64 ;;
	esac
done

# Hardware needs longer: real WiFi association plus TLS on a 268MHz ARM11.
[[ -z "$TIMEOUT" ]] && { [[ "$MODE" == hw ]] && TIMEOUT=90 || TIMEOUT=40; }

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
amber() { printf '\033[33m%s\033[0m\n' "$*"; }
dim()   { printf '\033[2m%s\033[0m\n' "$*"; }

# ---------------------------------------------------------------- build
do_build() {
	echo "==> building $TARGET"
	if ! make -C "$PROJECT_DIR" -j8 2>&1 | tail -20; then
		red "BUILD FAILED"
		exit 1
	fi
	[[ -f "$THREEDSX" ]] || { red "BUILD FAILED: no $TARGET.3dsx"; exit 1; }
	green "built $(basename "$THREEDSX")"
}

# ---------------------------------------------------------------- verdict
#
# Classify a transcript. Shared by both targets so "PASS" means the same thing
# whether it came from Azahar's SD card or a console's stdout.
#
# $1 transcript file, $2 mode
verdict() {
	local file="$1" mode="$2"

	# Every hardware run should self-describe; surfacing the build stamp here
	# is what stops us debugging a stale .3dsx.
	if grep -q '^BANNER ' "$file" 2>/dev/null; then
		echo "==> device"
		grep '^BANNER ' "$file" | sed 's/^BANNER /    /'
		echo
	fi

	echo "==> results"
	grep -E '^(PHASE=|DONE$)' "$file" 2>/dev/null || dim "(no step output)"
	echo

	local fails done_ok
	fails=$(grep -c 'RESULT=FAIL' "$file" 2>/dev/null | tr -d ' ')
	done_ok=$(grep -c '^DONE$' "$file" 2>/dev/null | tr -d ' ')

	if [[ "$done_ok" -eq 0 ]]; then
		red "HUNG OR CRASHED - no DONE sentinel"
		if [[ "$mode" == emu ]]; then
			dim "last emulator log lines:"
			tail -15 "$EMU_LOG" 2>/dev/null
		fi
		return 1
	elif [[ "$fails" -gt 0 ]]; then
		red "$fails step(s) FAILED"
		grep 'RESULT=FAIL' "$file" | sed 's/^/    /'
		return 1
	fi

	green "ALL STEPS PASSED"
	return 0
}

# ---------------------------------------------------------------- emulator
stop_azahar() {
	pkill -f "MacOS/azahar" 2>/dev/null
	for _ in 1 2 3 4 5; do
		pgrep -f "MacOS/azahar" >/dev/null || return 0
		sleep 1
	done
	pkill -9 -f "MacOS/azahar" 2>/dev/null
	sleep 1
}

run_emu() {
	stop_azahar
	rm -f "$RESULT_FILE" "$GUEST_LOG"
	mkdir -p "$SDMC/spotify"
	# Opt the app into auto-exit. Hardware uses an argv[0] sentinel instead.
	touch "$SDMC/spotify/.smoketest"

	echo "==> launching Azahar (timeout ${TIMEOUT}s)"
	# Must go through `open -a`: invoking Contents/MacOS/azahar directly pops a
	# modal and hangs, and disables camera emulation.
	open -a "$AZAHAR_APP" --args -w "$THREEDSX"

	local deadline=$((SECONDS + TIMEOUT))
	while (( SECONDS < deadline )); do
		grep -q '^DONE$' "$RESULT_FILE" 2>/dev/null && break
		sleep 1
	done

	stop_azahar

	if [[ ! -f "$RESULT_FILE" ]]; then
		red "NO RESULT FILE - app never wrote to sdmc:/testresult.txt"
		dim "last emulator log lines:"
		tail -15 "$EMU_LOG" 2>/dev/null
		return 3
	fi

	[[ $SHOW_LOG -eq 1 && -f "$GUEST_LOG" ]] && {
		echo "==> guest log"; tail -40 "$GUEST_LOG"; echo
	}

	verdict "$RESULT_FILE" emu
}

# ---------------------------------------------------------------- hardware
resolve_ip() {
	if [[ -n "$IP_ARG" ]]; then echo "$IP_ARG"; return 0; fi
	if [[ -n "${SPOTIFY3DS_IP:-}" ]]; then echo "$SPOTIFY3DS_IP"; return 0; fi
	if [[ -f "$PROJECT_DIR/.hwip" ]]; then
		head -1 "$PROJECT_DIR/.hwip" | tr -d '[:space:]'
		return 0
	fi
	return 1
}

run_hw() {
	local ip
	if ! ip=$(resolve_ip) || [[ -z "$ip" ]]; then
		red "no device address"
		dim "set one with:  echo 192.168.1.x > $PROJECT_DIR/.hwip"
		dim "or:            ./dev.sh --hw --ip 192.168.1.x"
		dim "or export SPOTIFY3DS_IP"
		return 64
	fi

	rm -f "$HW_LOG"
	echo "==> netloading to $ip (timeout ${TIMEOUT}s)"
	dim "the console must be sitting in the Homebrew Launcher"

	# -s serves the app's stdout back to us after upload; it never returns on
	# its own, so we own the clock. argv[0] carries the smoketest sentinel,
	# because 3dslink 0.6.3 cannot pass any other argument.
	3dslink -s -r 20 -0 "$TARGET-smoketest" -a "$ip" "$THREEDSX" \
		> "$HW_LOG" 2>&1 &
	local pid=$!

	# Phase 1: did we even reach the console?
	local connect_by=$((SECONDS + CONNECT_DEADLINE))
	local connected=0
	while (( SECONDS < connect_by )); do
		if grep -q 'Sending' "$HW_LOG" 2>/dev/null; then connected=1; break; fi
		if grep -qi 'Connection to .* failed\|failed to connect' "$HW_LOG" 2>/dev/null; then
			break
		fi
		kill -0 "$pid" 2>/dev/null || break
		sleep 1
	done

	if [[ $connected -eq 0 ]]; then
		kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
		red "NOT LAUNCHED - 3dslink could not reach $ip"
		dim "open the Homebrew Launcher on the console and leave it on the"
		dim "main menu, then rerun. (netloading is only accepted there.)"
		[[ -s "$HW_LOG" ]] && { echo; dim "3dslink said:"; sed 's/^/    /' "$HW_LOG"; }
		return 2
	fi

	# Phase 2: wait for the app to finish talking.
	local deadline=$((SECONDS + TIMEOUT))
	while (( SECONDS < deadline )); do
		grep -q '^DONE$' "$HW_LOG" 2>/dev/null && break
		kill -0 "$pid" 2>/dev/null || break
		sleep 1
	done

	kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null

	[[ $SHOW_LOG -eq 1 ]] && { echo "==> full transcript"; sed 's/^/    /' "$HW_LOG"; echo; }

	# Uploaded but silent: the app died before link3dsStdio() connected, or the
	# netload never actually started the binary.
	if ! grep -qE '^(BANNER |PHASE=)' "$HW_LOG" 2>/dev/null; then
		red "NO OUTPUT - uploaded, but the app printed nothing"
		dim "likely an early crash before stdout was redirected."
		dim "check SD:/spotify/log.txt on the card - it is written regardless."
		echo; dim "3dslink said:"; sed 's/^/    /' "$HW_LOG"
		return 3
	fi

	verdict "$HW_LOG" hw
}

# ---------------------------------------------------------------- main
do_build
[[ $BUILD_ONLY -eq 1 ]] && exit 0

if [[ "$MODE" == hw ]]; then
	run_hw
else
	run_emu
fi
exit $?
