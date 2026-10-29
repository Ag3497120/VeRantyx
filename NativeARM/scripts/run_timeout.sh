#!/usr/bin/env bash
# run_timeout.sh [--quit-timeout 60] [--allow-force-kill] <seconds> <command...>
# Native hosts receive an OpenVR quit request. A timeout never implies SIGKILL
# unless explicitly enabled; commands without this protocol are left to exit.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$SCRIPT_DIR/../tools/native_quit.py" "$@"
