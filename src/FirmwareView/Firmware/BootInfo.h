#ifndef UEFI_BOOT_LAB_BOOT_INFO_H
#define UEFI_BOOT_LAB_BOOT_INFO_H

#include <Uefi.h>
#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/SmBios.h>

//
// Boot handoff model used by this lab.
//
// Mental model category:
// - Firmware boot path.
// - Boot handoff contract between bootloader and kernel.
//
// This structure is intentionally small. A real loader would add kernel command
// line, initrd/module addresses, framebuffer info, runtime-services info, and
// architecture-specific state before jumping to the kernel entry point.
//
// Project direction note:
// - BootInfo is a learning model for understanding bootloader handoff.
// - The main deliverable of this repo is a pre-OS platform JSON report, not a
//   complete kernel loader.
//
typedef struct {
  // These root pointers are strongly typed once the ConfigurationTable GUID has
  // told us which firmware table format each address uses.
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *AcpiRsdp;
  SMBIOS_TABLE_ENTRY_POINT                      *SmbiosEntryPoint32;
  SMBIOS_TABLE_3_0_ENTRY_POINT                  *SmbiosEntryPoint64;
  EFI_MEMORY_DESCRIPTOR                         *MemoryMap;
  UINTN                                         MemoryMapSize;
  UINTN                                         MemoryMapDescriptorSize;
  UINT32                                        MemoryMapDescriptorVersion;
  UINTN                                         MemoryMapKey;
} UEFI_BOOT_LAB_BOOT_INFO;

EFI_STATUS
BuildBootHandoffInfo (
  IN  EFI_SYSTEM_TABLE                              *SystemTable,
  IN  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *AcpiRsdp,
  OUT UEFI_BOOT_LAB_BOOT_INFO                       *BootInfo
  );

VOID
PrintBootHandoffInfo (
  IN CONST UEFI_BOOT_LAB_BOOT_INFO  *BootInfo
  );

VOID
FreeBootHandoffInfo (
  IN OUT UEFI_BOOT_LAB_BOOT_INFO  *BootInfo
  );

#endif
