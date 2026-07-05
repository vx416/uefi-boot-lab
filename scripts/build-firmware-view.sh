#!/usr/bin/env bash
set -euo pipefail

if [[ -z "${EDK2_WORKSPACE:-}" ]]; then
  echo "EDK2_WORKSPACE is not set."
  echo "Example: export EDK2_WORKSPACE=/path/to/edk2"
  exit 1
fi

if [[ ! -f "$EDK2_WORKSPACE/edksetup.sh" ]]; then
  echo "Cannot find edksetup.sh under EDK2_WORKSPACE: $EDK2_WORKSPACE"
  exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
pkg_path="$EDK2_WORKSPACE/UefiBootLabPkg"
arch="${EDK2_ARCH:-X64}"
toolchain="${EDK2_TOOLCHAIN:-GCC}"
build_target="${EDK2_BUILD_TARGET:-DEBUG}"

if [[ ! -e "$pkg_path" || ! "$repo_root" -ef "$pkg_path" ]]; then
  echo "This package should be available at: $pkg_path"
  echo "Create a symlink, for example:"
  echo "  ln -s \"$repo_root\" \"$pkg_path\""
  exit 1
fi

cd "$EDK2_WORKSPACE"
set +u
source edksetup.sh
set -u
build -a "$arch" -t "$toolchain" -b "$build_target" -p UefiBootLabPkg/UefiBootLabPkg.dsc

echo
echo "Built EFI binaries:"
echo "$EDK2_WORKSPACE/Build/UefiBootLabPkg/${build_target}_${toolchain}/${arch}/FirmwareView.efi"
echo "$EDK2_WORKSPACE/Build/UefiBootLabPkg/${build_target}_${toolchain}/${arch}/FirmwareProducerDxe.efi"
