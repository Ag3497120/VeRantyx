#!/usr/bin/env bash
# run_timeout.sh <seconds> <command...>: runs the command, kills it (SIGKILL)
# after the time limit. macOS has no coreutils timeout, and the engine installs
# a SIGALRM handler, so perl's alarm does not end it.
limit="$1"; shift
"$@" &
pid=$!
( sleep "$limit"; kill -9 "$pid" 2>/dev/null ) &
watchdog=$!
wait "$pid"; rc=$?
kill "$watchdog" 2>/dev/null; wait "$watchdog" 2>/dev/null
exit $rc
