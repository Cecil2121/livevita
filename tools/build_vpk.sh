#!/bin/bash
# Builds LiveVita.vpk inside WSL (Ubuntu). Started by Build-VPK.bat; also works on its own:
#   bash tools/build_vpk.sh [TwitchClientID]
# First run: installs the build tools and VitaSDK into ~/vitasdk (10-20 minutes). Later runs take 1-2 minutes.
set -euo pipefail

cd "$(dirname "$0")/.."
PROJECT="$PWD"
CID="${1:-}"
if [[ ! "$CID" =~ ^[A-Za-z0-9]*$ ]]; then
    echo "ERROR: the Client ID should only contain letters and numbers: '$CID'" >&2
    exit 1
fi

export VITASDK="$HOME/vitasdk"
export PATH="$VITASDK/bin:$PATH"
BUILD="$HOME/.livevita-build"

step() { printf '\n==== %s ====\n' "$1"; }

# 1. Linux tools
missing=()
for c in cmake make git curl bzip2 xz tar file; do
    command -v "$c" >/dev/null 2>&1 || missing+=("$c")
done
if ((${#missing[@]})); then
    step "Installing build tools (${missing[*]}): Ubuntu may ask for your Linux password"
    sudo apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y cmake make git curl ca-certificates bzip2 xz-utils tar file python3
fi

# 2. VitaSDK
if [ ! -x "$VITASDK/bin/arm-vita-eabi-gcc" ] || [ ! -f "$VITASDK/arm-vita-eabi/lib/libvita2d.a" ]; then
    step "First run: installing VitaSDK into $VITASDK (10-20 minutes)"
    rm -rf "$VITASDK" "$HOME/.livevita-vdpm"
    git clone --depth 1 https://github.com/vitasdk/vdpm "$HOME/.livevita-vdpm"
    (cd "$HOME/.livevita-vdpm" && ./bootstrap-vitasdk.sh && ./install-all.sh)
    if [ ! -f "$VITASDK/arm-vita-eabi/lib/libvita2d.a" ]; then
        echo "ERROR: VitaSDK did not install completely. Check your internet connection and run again." >&2
        exit 1
    fi
fi

# 3. Build (in Linux's own disk: much faster than building on C:)
step "Building LiveVita"
rm -rf "$BUILD"
cmake -S "$PROJECT" -B "$BUILD" -DLV_CLIENT_ID="$CID"
cmake --build "$BUILD" -j"$(nproc)"

cp "$BUILD/LiveVita.vpk" "$PROJECT/LiveVita.vpk"
step "Done: LiveVita.vpk is in the LiveVita folder"
if [ -n "$CID" ]; then echo "Your Client ID is built in."
else echo "No Client ID built in: put it in ux0:data/LiveVita/client_id.txt on your Vita."; fi
