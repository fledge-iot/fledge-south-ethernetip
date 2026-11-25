#!/usr/bin/env bash
#
# Virtual SLC 500 for integration testing.
#
# Wraps ab_server, the reference EtherNet/IP simulator that ships in the
# libplctag source tree. It is the same project that provides the client
# library the plugin links against, so its PCCC wire format is authoritative:
# if the plugin talks to this, it talks to a real SLC.
#
# ab_server is self-contained - it does not link libplctag itself - so this
# script fetches the pinned source and compiles the ~15 files directly rather
# than running libplctag's full CMake build.
#
# Usage:
#   ./tests/vplc/vplc.sh build     Fetch and compile ab_server (cached)
#   ./tests/vplc/vplc.sh start     Start the simulator in the background
#   ./tests/vplc/vplc.sh stop      Stop it
#   ./tests/vplc/vplc.sh status    Report whether it is listening
#   ./tests/vplc/vplc.sh log       Tail the simulator log
#   ./tests/vplc/vplc.sh env       Print the VPLC_* exports for the test binary
#
# Environment:
#   VPLC_SLC_PORT       Listen port (default 44819 - 44818 is often already
#                       taken by another vPLC on developer machines)
#   LIBPLCTAG_VERSION   libplctag tag to build ab_server from (default 2.7.2,
#                       matching requirements.sh)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK_DIR="${REPO_ROOT}/build/vplc"
SRC_DIR="${WORK_DIR}/libplctag"
BIN="${WORK_DIR}/ab_server"
PID_FILE="${WORK_DIR}/ab_server.pid"
LOG_FILE="${WORK_DIR}/ab_server.log"

PORT="${VPLC_SLC_PORT:-44819}"
LIBPLCTAG_VERSION="${LIBPLCTAG_VERSION:-2.7.2}"

# Data files the simulator serves. These are the only files ab_server's PCCC
# implementation supports, and they cover every SLC type the plugin maps:
#   B3   bit file      -> BOOL      N7   16-bit integer -> INT
#   F8   float         -> REAL      L19  32-bit integer -> DINT
#   ST18 82-char string-> STRING
# Sizes are generous enough for the tests to use distinct elements per case.
TAGS=(
	'--tag=B3[10]'
	'--tag=N7[10]'
	'--tag=F8[10]'
	'--tag=L19[10]'
	'--tag=ST18[4]'
)

die() { echo "vplc: $*" >&2; exit 1; }

cmd_build() {
	if [ -x "${BIN}" ]; then
		echo "vplc: ab_server already built at ${BIN}"
		return 0
	fi

	command -v git   >/dev/null || die "git is required to fetch libplctag"
	command -v cc    >/dev/null || die "a C compiler (cc) is required"

	mkdir -p "${WORK_DIR}"

	if [ ! -d "${SRC_DIR}" ]; then
		echo "vplc: fetching libplctag v${LIBPLCTAG_VERSION}..."
		git clone --quiet --depth 1 --branch "v${LIBPLCTAG_VERSION}" \
			https://github.com/libplctag/libplctag.git "${SRC_DIR}"
	fi

	local ab="${SRC_DIR}/src/tools/ab_server"
	[ -d "${ab}" ] || die "ab_server sources not found in ${SRC_DIR} - unexpected layout for v${LIBPLCTAG_VERSION}"

	echo "vplc: compiling ab_server..."
	# -I src for utils/random_utils.h. random_utils.c is deliberately NOT
	# compiled in: ab_server's own utils.c already defines random_u64().
	cc -O2 -o "${BIN}" -I "${SRC_DIR}/src" -I "${ab}" "${ab}"/*.c -lpthread -lm

	echo "vplc: built ${BIN}"
}

is_running() {
	[ -f "${PID_FILE}" ] && kill -0 "$(cat "${PID_FILE}")" 2>/dev/null
}

cmd_start() {
	cmd_build

	if is_running; then
		echo "vplc: already running (pid $(cat "${PID_FILE}")) on port ${PORT}"
		return 0
	fi

	echo "vplc: starting SLC 500 simulator on port ${PORT}..."
	# ab_server logs at INFO on stdout with no quiet flag, so it goes to a file
	# rather than drowning the test output.
	nohup "${BIN}" --plc=SLC500 "--port=${PORT}" "${TAGS[@]}" \
		> "${LOG_FILE}" 2>&1 &
	echo $! > "${PID_FILE}"

	# Wait for the listener rather than sleeping a fixed amount.
	local i
	for i in $(seq 1 50); do
		if (exec 3<>"/dev/tcp/127.0.0.1/${PORT}") 2>/dev/null; then
			exec 3<&- 3>&-
			echo "vplc: listening on 127.0.0.1:${PORT} (pid $(cat "${PID_FILE}"))"
			return 0
		fi
		is_running || { echo "vplc: simulator died on startup - log follows" >&2; cat "${LOG_FILE}" >&2; exit 1; }
		sleep 0.1
	done

	die "simulator did not start listening on port ${PORT} within 5s (see ${LOG_FILE})"
}

cmd_stop() {
	if ! is_running; then
		echo "vplc: not running"
		rm -f "${PID_FILE}"
		return 0
	fi
	local pid
	pid="$(cat "${PID_FILE}")"
	echo "vplc: stopping pid ${pid}"
	kill "${pid}" 2>/dev/null || true
	local i
	for i in $(seq 1 30); do
		kill -0 "${pid}" 2>/dev/null || break
		sleep 0.1
	done
	kill -9 "${pid}" 2>/dev/null || true
	rm -f "${PID_FILE}"
}

cmd_status() {
	if is_running; then
		echo "vplc: running (pid $(cat "${PID_FILE}")) on 127.0.0.1:${PORT}"
	else
		echo "vplc: not running"
		return 1
	fi
}

cmd_log() {
	[ -f "${LOG_FILE}" ] || die "no log at ${LOG_FILE}"
	tail -f "${LOG_FILE}"
}

# The address carries the port because the simulator does not run on 44818 by
# default; libplctag accepts host:port in its gateway attribute.
cmd_env() {
	echo "export VPLC_SLC_ADDRESS=127.0.0.1:${PORT}"
}

case "${1:-}" in
	build)  cmd_build  ;;
	start)  cmd_start  ;;
	stop)   cmd_stop   ;;
	status) cmd_status ;;
	log)    cmd_log    ;;
	env)    cmd_env    ;;
	*)      echo "usage: $0 {build|start|stop|status|log|env}" >&2; exit 1 ;;
esac
