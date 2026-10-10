# screen-lock.sh (sourced by run.sh; bash or zsh): wait for an unlocked screen before -allowProvisioningUpdates.
# screen_locked returns 0 while the Mac's login screen is locked (IOConsoleUsers' CGSSessionScreenIsLocked).
# xcodebuild steps that load the Apple developer account (-allowProvisioningUpdates, -exportArchive) cannot read
# Xcode's keychain credentials then ("Invalid credentials in keychain for <id>, missing Xcode-Username", 2026-10-03
# 09:44 CDT), and Xcode's account manager drops the account: Xcode is signed out. Such steps call wait_unlocked first.
# SCREEN_LOCK_FAKE_SECS=N (dry runs) pretends the screen is locked for the first N seconds after the first check.
screen_locked() {
  if [ -n "${SCREEN_LOCK_FAKE_SECS:-}" ]; then
    : "${_sl_t0:=$(date +%s)}"
    [ $(( $(date +%s) - _sl_t0 )) -lt "$SCREEN_LOCK_FAKE_SECS" ]
    return
  fi
  [ "$(ioreg -n Root -d1 -a 2>/dev/null | python3 -c '
import plistlib, sys
d = plistlib.loads(sys.stdin.buffer.read())
print(int(any(u.get("CGSSessionScreenIsLocked") for u in d.get("IOConsoleUsers", []))))' 2>/dev/null)" = 1 ]
}
# wait_unlocked <what>: returns at once while the screen is unlocked; otherwise says why it waits (again every
# 10 minutes) and re-checks every 30 s.
wait_unlocked() {
  local n=0
  while screen_locked; do
    [ $((n % 20)) = 0 ] && echo "$(date -u +%T) UTC: the Mac's screen is locked; waiting to $1. Xcode can't read the" \
      "Apple account while it is locked, and xcodebuild would then drop the account (sign Xcode out)." >&2
    n=$((n + 1)); sleep 30
  done
  [ $n = 0 ] || echo "$(date -u +%T) UTC: the screen is unlocked; continuing" >&2
  return 0
}
