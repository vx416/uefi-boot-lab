#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0"
  echo
  echo "Build a custom OVMF image that includes FirmwareProducerDxe in the DXE FV."
  echo
  echo "Environment:"
  echo "  EDK2_WORKSPACE   EDK II workspace path. Default: /home/vagrant/workspace/edk2"
  echo "  EDK2_TOOLCHAIN   EDK II toolchain tag. Default: GCCNOLTO"
  echo "  EDK2_BUILD_TARGET Build target. Default: DEBUG"
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi

EDK2_WORKSPACE="${EDK2_WORKSPACE:-/home/vagrant/workspace/edk2}"
toolchain="${EDK2_TOOLCHAIN:-GCCNOLTO}"
build_target="${EDK2_BUILD_TARGET:-DEBUG}"
arch="X64"
platform_dsc="OvmfPkg/OvmfPkgX64.dsc"
platform_fdf="OvmfPkg/OvmfPkgX64.fdf"
producer_inf="UefiBootLabPkg/src/FirmwareProducerDxe/FirmwareProducerDxe.inf"

dsc_path="$EDK2_WORKSPACE/$platform_dsc"
fdf_path="$EDK2_WORKSPACE/$platform_fdf"
pkg_path="$EDK2_WORKSPACE/UefiBootLabPkg"

if [[ ! -d "$EDK2_WORKSPACE" ]]; then
  echo "EDK2_WORKSPACE not found: $EDK2_WORKSPACE"
  exit 1
fi

if [[ ! -d "$pkg_path" ]]; then
  echo "This package should be available at: $pkg_path"
  echo "Create a symlink, for example:"
  echo "  ln -s \"$(pwd)\" \"$pkg_path\""
  exit 1
fi

if [[ ! -f "$dsc_path" || ! -f "$fdf_path" ]]; then
  echo "OVMF DSC/FDF not found under: $EDK2_WORKSPACE/OvmfPkg"
  exit 1
fi

patch_dsc() {
  # The DSC controls what modules are legal to build for the OVMF platform.
  # Adding a small standalone [Components] section keeps the upstream file
  # change narrow and makes the script idempotent.
  if ! grep -Fq "$producer_inf" "$dsc_path"; then
    if [[ ! -f "$dsc_path.uefi-boot-lab.bak" ]]; then
      cp "$dsc_path" "$dsc_path.uefi-boot-lab.bak"
    fi
    {
      echo
      echo "# UEFI Boot Lab: lab firmware producer included in custom OVMF builds."
      echo "[Components]"
      echo "  $producer_inf"
    } >> "$dsc_path"
  fi

  # Some current distro GCC versions warn about an older OVMF SNP helper as
  # maybe-uninitialized. OVMF treats warnings as errors, so this lab build only
  # relaxes that one warning class instead of disabling Werror globally.
  if ! grep -Fq "UEFI Boot Lab: GCC warning compatibility" "$dsc_path"; then
    if [[ ! -f "$dsc_path.uefi-boot-lab.bak" ]]; then
      cp "$dsc_path" "$dsc_path.uefi-boot-lab.bak"
    fi
    {
      echo
      echo "# UEFI Boot Lab: GCC warning compatibility for distro toolchains."
      echo "[BuildOptions]"
      echo "  GCC:*_*_*_CC_FLAGS = -Wno-error=maybe-uninitialized"
    } >> "$dsc_path"
  fi
}

patch_fdf() {
  # The FDF decides which built modules are actually packed into the firmware
  # volume. Put FirmwareProducerDxe in DXEFV so OVMF dispatches it during DXE,
  # before BOOTX64.EFI / FirmwareView.efi is launched.
  if grep -Fq "$producer_inf" "$fdf_path"; then
    return
  fi

  if [[ ! -f "$fdf_path.uefi-boot-lab.bak" ]]; then
    cp "$fdf_path" "$fdf_path.uefi-boot-lab.bak"
  fi

  awk -v producer_inf="$producer_inf" '
    BEGIN {
      in_dxefv = 0
      inserted = 0
    }

    /^\[FV\.DXEFV\]/ {
      in_dxefv = 1
      print
      next
    }

    /^\[FV\./ {
      if (in_dxefv && !inserted) {
        print "INF  " producer_inf
        inserted = 1
      }
      in_dxefv = 0
    }

    in_dxefv && !inserted && /^INF[[:space:]]+/ {
      print "INF  " producer_inf
      inserted = 1
    }

    { print }

    END {
      if (!inserted) {
        exit 2
      }
    }
  ' "$fdf_path" > "$fdf_path.uefi-boot-lab.tmp" || {
    rm -f "$fdf_path.uefi-boot-lab.tmp"
    echo "Failed to insert $producer_inf into [FV.DXEFV] in $fdf_path"
    exit 1
  }

  mv "$fdf_path.uefi-boot-lab.tmp" "$fdf_path"
}

patch_dsc
patch_fdf

cd "$EDK2_WORKSPACE"

# edksetup.sh initializes WORKSPACE, EDK_TOOLS_PATH, CONF_PATH, and PATH. It is
# written for normal shell semantics, so temporarily relax nounset while sourcing.
set +u
source ./edksetup.sh
set -u

build -a "$arch" -t "$toolchain" -b "$build_target" -p "$platform_dsc"

echo
echo "Custom OVMF image:"
echo "$EDK2_WORKSPACE/Build/OvmfX64/${build_target}_${toolchain}/FV/OVMF_CODE.fd"
echo
echo "Use it with:"
echo "  ./scripts/run-e2e-linux.sh $EDK2_WORKSPACE/Build/OvmfX64/${build_target}_${toolchain}/FV/OVMF_CODE.fd"
