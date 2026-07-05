#ifndef UEFI_BOOT_LAB_FIRMWARE_JSON_H
#define UEFI_BOOT_LAB_FIRMWARE_JSON_H

#include <Uefi.h>
#include <IndustryStandard/Acpi.h>

#include "../Firmware/BootInfo.h"

// Mental model category: Phase 1 firmware view report output.
//
// This module writes the firmware-side view to the EFI FAT volume so Linux-side
// tooling can later read it and compare it with the OS-visible view.
EFI_STATUS
WriteFirmwareViewJson (
  IN EFI_HANDLE                                      ImageHandle,
  IN EFI_SYSTEM_TABLE                                *SystemTable,
  IN EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER    *AcpiRsdp,
  IN CONST UEFI_BOOT_LAB_BOOT_INFO                   *BootInfo
  );

#endif
