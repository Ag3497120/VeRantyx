#!/usr/bin/env bash
# Fetches the ARM64 system libraries the game expects from the Steam Linux
# Runtime (which has no public ARM64 depot) as Debian trixie packages, and
# extracts their .so files to NativeARM/build/debian/usr/lib/aarch64-linux-gnu.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$SCRIPT_DIR/../build/debian}"
MIRROR="${DEBIAN_MIRROR:-https://deb.debian.org/debian}"
PACKAGES=(libstdc++6 libgcc-s1 libogg0 libatomic1 libpcre2-8-0 libffi8 libmount1 libblkid1 libselinux1 libglib2.0-0t64 libpng16-16t64 libssl3t64 libudev1 libexpat1 libzstd1 libuuid1 libbz2-1.0 libdrm2 libbrotli1 libfribidi0 libpango-1.0-0 libfontconfig1 zlib1g libicu76)
mkdir -p "$OUT/debs"
curl -sL -o "$OUT/Packages.gz" "$MIRROR/dists/trixie/main/binary-arm64/Packages.gz"
gunzip -kf "$OUT/Packages.gz"
python3 - "$OUT" "$MIRROR" "${PACKAGES[@]}" <<'PY'
import re, subprocess, sys, os
out, mirror, want = sys.argv[1], sys.argv[2], sys.argv[3:]
text = open(os.path.join(out, "Packages")).read()
found = {}
for block in text.split("\n\n"):
    m = re.search(r"^Package: (\S+)", block, re.M)
    if m and m.group(1) in want:
        found[m.group(1)] = re.search(r"^Filename: (\S+)", block, re.M).group(1)
missing = [w for w in want if w not in found]
if missing: raise SystemExit(f"not in trixie arm64: {missing}")
for name, fn in found.items():
    dest = os.path.join(out, "debs", os.path.basename(fn))
    if not os.path.exists(dest):
        subprocess.run(["curl", "-sL", "-o", dest, mirror + "/" + fn], check=True)
    print(f"{name}: {os.path.basename(fn)}")
PY
cd "$OUT/debs"
for deb in *.deb; do
    ar x "$deb"
    data="$(ls data.tar.* | head -1)"
    tar -xf "$data" -C "$OUT"
    rm -f data.tar.* control.tar.* debian-binary
done
echo "Libraries: $OUT/usr/lib/aarch64-linux-gnu"
ls "$OUT/usr/lib/aarch64-linux-gnu" | grep -c '\.so' | sed 's/$/ files/'
