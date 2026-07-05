#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "Usage: $0 /path/to/OVMF_CODE.fd /path/to/fat-dir [/path/to/OVMF_VARS.fd]"
  echo
  echo "The FAT directory should contain EFI files, for example:"
  echo "  fat-dir/EFI/BOOT/BOOTX64.EFI"
  exit 1
fi

ovmf_code="$1"
fat_dir="$2"
ovmf_vars_template="${3:-}"
qemu_display="${QEMU_DISPLAY:-curses}"
qemu_timeout="${QEMU_TIMEOUT:-}"
qemu_serial_log="${QEMU_SERIAL_LOG:-}"

if [[ ! -f "$ovmf_code" ]]; then
  echo "OVMF_CODE.fd not found: $ovmf_code"
  exit 1
fi

if [[ ! -d "$fat_dir" ]]; then
  echo "FAT directory not found: $fat_dir"
  exit 1
fi

if [[ -z "$ovmf_vars_template" ]]; then
  ovmf_dir="$(dirname "$ovmf_code")"
  code_name="$(basename "$ovmf_code")"
  vars_name="${code_name/CODE/VARS}"
  ovmf_vars_template="$ovmf_dir/$vars_name"
fi

if [[ ! -f "$ovmf_vars_template" ]]; then
  echo "OVMF_VARS.fd not found: $ovmf_vars_template"
  echo "Pass a VARS image explicitly, for example:"
  echo "  $0 \"$ovmf_code\" \"$fat_dir\" /usr/share/OVMF/OVMF_VARS_4M.fd"
  exit 1
fi

ovmf_vars="$(mktemp /tmp/uefi-boot-lab-vars.XXXXXX.fd)"
cp "$ovmf_vars_template" "$ovmf_vars"
trap 'rm -f "$ovmf_vars"' EXIT

serial_args=(-serial stdio)
if [[ -n "$qemu_serial_log" ]]; then
  serial_args=(-serial "file:$qemu_serial_log")
fi

qemu_args=(
  -machine q35,accel=tcg \
  -m 1024 \
  -display "$qemu_display" \
  -monitor none \
  -drive "if=pflash,format=raw,unit=0,readonly=on,file=$ovmf_code" \
  -drive "if=pflash,format=raw,unit=1,file=$ovmf_vars" \
  -drive "format=raw,file=fat:rw:$fat_dir" \
  -net none \
  "${serial_args[@]}"
)

if [[ -n "$qemu_timeout" ]]; then
  timeout "$qemu_timeout" qemu-system-x86_64 "${qemu_args[@]}"
else
  qemu-system-x86_64 "${qemu_args[@]}"
fi
