#!/bin/zsh
# Makes a folder Henge on iOS can bring in from a vphone-cli virtual iPhone (vresearch101):
# drop it on Henge in Finder (the iPhone's Files section) or pick it in the Files app.
#   tools/export_iphone.sh VM-DIR [NAME] [RAM_MB] [OUTDIR]
# VM-DIR is the VM bundle (e.g. ~/.vphone/machines/NAME): Disk.img, nvram.bin, SEPStorage,
# OpenAVB.bin, OpenSEP.bin, config.plist. Shut the VM down first. The disk goes in packed
# (disk.img.hvsparse: holes left out). Reads the bundle; writes nothing to it.
# The folder holds Apple firmware and the machine's identity: keep it to yourself.
set -e
VM=${1:?usage: $0 VM-DIR [NAME] [RAM_MB] [OUTDIR]}
NAME=${2:-iPhone}; RAM=${3:-2048}; OUT=${4:-$HOME/Desktop/$NAME}
for f in Disk.img nvram.bin SEPStorage OpenAVB.bin OpenSEP.bin config.plist; do
    [ -f "$VM/$f" ] || { echo "$VM has no $f" >&2; exit 2; }
done
read ECID W H <<<"$(python3 -c '
import plistlib, sys
d = plistlib.load(open(sys.argv[1], "rb"))
ecid = plistlib.loads(d["machineIdentifier"])["ECID"]
s = d.get("screenConfig", {})
print(ecid, s.get("width", 1206), s.get("height", 2622))' "$VM/config.plist")"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
make -C "$HERE" build/hvsparse >/dev/null
mkdir -p "$OUT"
cp "$VM/OpenAVB.bin" "$VM/OpenSEP.bin" "$VM/SEPStorage" "$VM/nvram.bin" "$OUT/"
echo "packing the disk (only its data moves)..."
"$HERE/build/hvsparse" pack "$VM/Disk.img" "$OUT/disk.img.hvsparse"
cat > "$OUT/machine.json" <<JSON
{"name": "$NAME", "kind": "iphone", "ecid": $ECID, "seed": 0, "ramMB": $RAM, "screenWidth": $W, "screenHeight": $H}
JSON
echo "$OUT: $(du -sh "$OUT" | cut -f1) (ECID $(printf 0x%x $ECID), screen ${W}x${H}, ${RAM} MB)"
