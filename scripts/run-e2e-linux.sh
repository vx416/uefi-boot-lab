#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 /path/to/OVMF_CODE.fd [work-dir] [/path/to/OVMF_VARS.fd]"
  echo
  echo "This runs the Phase 1 -> Phase 2/3 lab flow:"
  echo "  1. Boot OVMF with FirmwareView.efi and write firmware-view.json to ESP."
  echo "  2. Boot an x86_64 Ubuntu cloud guest with the same ESP attached."
  echo "  3. Run tools/platform_report.py inside the Linux guest."
  echo "  4. Copy platform-report.md/json back to the ESP."
  exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ovmf_code="$1"
work_dir="${2:-$repo_root/artifacts/e2e-linux}"
ovmf_vars_template="${3:-}"

esp_dir="$work_dir/esp"
image_dir="$work_dir/images"
run_dir="$work_dir/run"
report_dir_on_esp="$esp_dir/EFI/UEFI-BOOT-LAB/linux-report"
firmware_json="$esp_dir/EFI/UEFI-BOOT-LAB/firmware-view.json"
esp_img="$run_dir/esp.img"

cloud_image_url="${E2E_CLOUD_IMAGE_URL:-https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img}"
cloud_image_base="$image_dir/ubuntu-noble-server-cloudimg-amd64.img"
guest_disk="$run_dir/linux-guest.qcow2"
seed_img="$run_dir/seed.img"
serial_log="$run_dir/linux-serial.log"
qemu_log="$run_dir/linux-qemu.log"
timeout_seconds="${E2E_QEMU_TIMEOUT:-600}"

if [[ ! -f "$ovmf_code" ]]; then
  echo "OVMF_CODE.fd not found: $ovmf_code"
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
  exit 1
fi

for command in qemu-system-x86_64 qemu-img mkfs.vfat mcopy wget python3 timeout; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "Missing required command: $command"
    exit 1
  fi
done

mkdir -p "$esp_dir/EFI/BOOT" "$image_dir" "$run_dir" "$report_dir_on_esp"
rm -rf "$esp_dir/EFI/UEFI-BOOT-LAB"
mkdir -p "$esp_dir/EFI/UEFI-BOOT-LAB"

firmware_efi="${EDK2_WORKSPACE:-/home/vagrant/workspace/edk2}/Build/UefiBootLabPkg/DEBUG_${EDK2_TOOLCHAIN:-GCCNOLTO}/X64/FirmwareView.efi"
producer_efi="${EDK2_WORKSPACE:-/home/vagrant/workspace/edk2}/Build/UefiBootLabPkg/DEBUG_${EDK2_TOOLCHAIN:-GCCNOLTO}/X64/FirmwareProducerDxe.efi"
if [[ ! -f "$firmware_efi" ]]; then
  echo "FirmwareView.efi not found: $firmware_efi"
  echo "Run make build first."
  exit 1
fi
if [[ ! -f "$producer_efi" ]]; then
  echo "FirmwareProducerDxe.efi not found: $producer_efi"
  echo "Run make build first."
  exit 1
fi

# UEFI default removable media path. OVMF will execute this in the first boot
# stage and FirmwareView.efi will write EFI/UEFI-BOOT-LAB/firmware-view.json.
cp "$firmware_efi" "$esp_dir/EFI/BOOT/BOOTX64.EFI"
cp "$producer_efi" "$esp_dir/EFI/UEFI-BOOT-LAB/FirmwareProducerDxe.efi"

echo "==> Stage 1: boot FirmwareView.efi and write firmware-view.json"
QEMU_TIMEOUT="${E2E_FIRMWARE_TIMEOUT:-10}" QEMU_DISPLAY=none \
  "$repo_root/scripts/smoke-ovmf.sh" "$ovmf_code" "$esp_dir" "$ovmf_vars_template"

if [[ ! -f "$firmware_json" ]]; then
  echo "Firmware JSON was not produced: $firmware_json"
  exit 1
fi

python3 -m json.tool "$firmware_json" >/dev/null
echo "Firmware JSON: $firmware_json"

if [[ ! -f "$cloud_image_base" ]]; then
  echo "==> Downloading Ubuntu cloud image"
  wget -O "$cloud_image_base.tmp" "$cloud_image_url"
  mv "$cloud_image_base.tmp" "$cloud_image_base"
fi

rm -f "$guest_disk" "$seed_img" "$esp_img" "$serial_log" "$qemu_log"
qemu-img create -f qcow2 -F qcow2 -b "$cloud_image_base" "$guest_disk" 12G >/dev/null

# Stage 1 uses QEMU's directory-backed FAT support because it is convenient for
# FirmwareView.efi to write directly into the host directory. Linux guests do
# not reliably mount that vvfat presentation as a normal block device, so Stage
# 2 gets a real FAT image seeded from the Stage 1 ESP directory.
truncate -s 128M "$esp_img"
mkfs.vfat -n UEFI-LAB "$esp_img" >/dev/null
mcopy -s -i "$esp_img" "$esp_dir/EFI" ::

cat >"$run_dir/meta-data" <<'EOF_META'
instance-id: uefi-boot-lab-e2e
local-hostname: uefi-boot-lab-e2e
EOF_META

cat >"$run_dir/user-data" <<'EOF_USER'
#cloud-config
runcmd:
  - |
    set -eux

    # Install useful Linux-side inventory tools when the guest has network
    # access. Keep this best-effort so the lab still produces a report in
    # offline environments; missing tools are recorded as diagnostics.
    apt-get update || true
    apt-get install -y dmidecode pciutils numactl python3 || true

    # Mount the repository shared from the QEMU host through 9p. This keeps the
    # Linux guest running the exact Python collector from the working tree.
    mkdir -p /opt/uefi-boot-lab
    mount -t 9p -o trans=virtio,version=9p2000.L,ro uefi_boot_lab /opt/uefi-boot-lab

    # Find the ESP that contains FirmwareView's JSON. The device name is not
    # hard-coded because QEMU may expose it as vdX, sdX, or another block name.
    mkdir -p /mnt/uefi-boot-lab-esp
    for dev in /dev/vd? /dev/sd? /dev/nvme?n?; do
      [ -b "$dev" ] || continue
      if mount -t vfat "$dev" /mnt/uefi-boot-lab-esp 2>/dev/null; then
        if [ -f /mnt/uefi-boot-lab-esp/EFI/UEFI-BOOT-LAB/firmware-view.json ]; then
          break
        fi
        umount /mnt/uefi-boot-lab-esp
      fi
    done

    test -f /mnt/uefi-boot-lab-esp/EFI/UEFI-BOOT-LAB/firmware-view.json

    # Phase 2/3: collect the Linux OS view from this booted guest, then compare
    # it with the firmware JSON and the expected platform contract.
    expected_args=""
    if [ -f /opt/uefi-boot-lab/expected-platform.yaml ]; then
      expected_args="--expected-platform /opt/uefi-boot-lab/expected-platform.yaml"
    fi
    python3 /opt/uefi-boot-lab/tools/platform_report.py all \
      --out /var/lib/uefi-boot-lab/linux-view \
      --firmware-json /mnt/uefi-boot-lab-esp/EFI/UEFI-BOOT-LAB/firmware-view.json \
      $expected_args \
      --fail-on-error

    rm -rf /mnt/uefi-boot-lab-esp/EFI/UEFI-BOOT-LAB/linux-report
    mkdir -p /mnt/uefi-boot-lab-esp/EFI/UEFI-BOOT-LAB/linux-report
    cp -a /var/lib/uefi-boot-lab/linux-view/report/. \
      /mnt/uefi-boot-lab-esp/EFI/UEFI-BOOT-LAB/linux-report/
    sync
    poweroff
EOF_USER

truncate -s 64M "$seed_img"
mkfs.vfat -n cidata "$seed_img" >/dev/null
mcopy -i "$seed_img" "$run_dir/meta-data" ::meta-data
mcopy -i "$seed_img" "$run_dir/user-data" ::user-data

ovmf_vars="$run_dir/OVMF_VARS.fd"
cp "$ovmf_vars_template" "$ovmf_vars"

echo "==> Stage 2: boot Linux guest and run platform_report.py"
set +e
timeout "$timeout_seconds" qemu-system-x86_64 \
  -machine q35,accel=tcg \
  -m 2048 \
  -smp 2 \
  -display none \
  -monitor none \
  -drive "if=pflash,format=raw,unit=0,readonly=on,file=$ovmf_code" \
  -drive "if=pflash,format=raw,unit=1,file=$ovmf_vars" \
  -drive "if=virtio,format=qcow2,file=$guest_disk" \
  -drive "if=virtio,format=raw,file=$esp_img" \
  -drive "if=virtio,format=raw,file=$seed_img" \
  -virtfs "local,path=$repo_root,mount_tag=uefi_boot_lab,security_model=none,readonly=on" \
  -netdev user,id=net0 \
  -device virtio-net-pci,netdev=net0 \
  -serial "file:$serial_log" \
  >"$qemu_log" 2>&1
status=$?
set -e

if [[ "$status" != "0" && "$status" != "124" ]]; then
  echo "Linux QEMU failed with exit code $status"
  echo "QEMU log: $qemu_log"
  echo "Serial log: $serial_log"
  exit "$status"
fi

mkdir -p "$report_dir_on_esp"
mcopy -o -i "$esp_img" "::EFI/UEFI-BOOT-LAB/firmware-view.json" "$firmware_json"
mcopy -o -i "$esp_img" "::EFI/UEFI-BOOT-LAB/linux-report/platform-report.json" "$report_dir_on_esp/platform-report.json"
mcopy -o -i "$esp_img" "::EFI/UEFI-BOOT-LAB/linux-report/platform-report.md" "$report_dir_on_esp/platform-report.md"

if [[ ! -f "$report_dir_on_esp/platform-report.json" ]]; then
  echo "Linux guest did not produce platform-report.json"
  echo "QEMU log: $qemu_log"
  echo "Serial log: $serial_log"
  exit 1
fi

echo "E2E report JSON: $report_dir_on_esp/platform-report.json"
echo "E2E report Markdown: $report_dir_on_esp/platform-report.md"
