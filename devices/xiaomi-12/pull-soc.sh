#!/bin/bash
# Pull the phone's own device tree over adb and turn it into the SoC boot-args HakiOS needs on this
# device (tools/soc-args.py). The Xiaomi 12 differs from the kernel defaults (built for the POCO X5
# Pro), so its soc.args carries its own addresses. Needs the phone in Android with adb (root not
# required: /sys/firmware/fdt is world-readable).
set -e
cd "$(dirname "$0")/../.."
PATH=$PWD/tools/macos-bin/platform-tools:$PATH
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
adb pull /sys/firmware/fdt "$tmp/fdt" >/dev/null || { echo "pull-soc.sh: cannot read /sys/firmware/fdt over adb"; exit 1; }
tools/soc-args.py "$tmp/fdt" > "$tmp/soc.args"
# The memory addresses the tool does not generate yet (see devices/xiaomi-12/README.md): the
# reserved-memory nodes of the phone's device tree.
cat >> "$tmp/soc.args" <<'ARGS'
soc_cmd_db=0x80860000
soc_mpss_mem=0x8bc00000
modem_ipa_fw_at=0x8b910000
soc_pas_mpss=0x4
ARGS
cp "$tmp/soc.args" "$(dirname "$0")/soc.args"
echo "pull-soc.sh: wrote $(dirname "$0")/soc.args"
