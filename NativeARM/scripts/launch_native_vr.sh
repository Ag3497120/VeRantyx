#!/usr/bin/env bash
# Developer entry point for a session configuration produced by the app.
# Players install, select Native ARM64 and press Play in VeRantyx.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR/../build/native-session" "$@"
