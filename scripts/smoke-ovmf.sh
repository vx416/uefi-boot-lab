#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 /path/to/OVMF_CODE.fd /path/to/fat-dir [/path/to/OVMF_VARS.fd]"
  exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
log_file="${QEMU_LOG:-/tmp/uefi-boot-lab-qemu.log}"
serial_log="${QEMU_SERIAL_LOG:-/tmp/uefi-boot-lab-serial.log}"
timeout_seconds="${QEMU_TIMEOUT:-10}"

rm -f "$log_file" "$serial_log"

set +e
QEMU_DISPLAY=none QEMU_TIMEOUT="$timeout_seconds" QEMU_SERIAL_LOG="$serial_log" \
  "$repo_root/scripts/run-ovmf.sh" "$@" >"$log_file" 2>&1
status=$?
set -e

if [[ "$status" != "0" && "$status" != "124" ]]; then
  echo "QEMU failed with exit code $status"
  echo "Full log: $log_file"
  exit "$status"
fi

echo "QEMU log: $log_file"
echo "Serial log: $serial_log"
echo

if grep -a -E 'Firmware view collector for uefi-boot-lab|Firmware vendor:|UEFI revision:|Configuration tables:|Memory map descriptors:' "$serial_log"; then
  echo
  echo "Smoke test passed."
  exit 0
fi

echo "Smoke test did not find FirmwareView output."
echo "QEMU log: $log_file"
echo "Serial log: $serial_log"
exit 1
