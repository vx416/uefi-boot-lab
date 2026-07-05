#include <Uefi.h>
#include <Guid/SmBios.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiLib.h>

#include "BootInfo.h"
#include "ConfigTables.h"

//
// Boot handoff struct builder.
//
// Mental model category:
// - Firmware boot path.
// - Boot handoff contract between bootloader and kernel.
//
// Where this sits in the platform flow:
// - A bootloader first discovers firmware-provided entry points such as ACPI
//   and SMBIOS.
// - Before ExitBootServices(), it captures the current UEFI memory map.
// - It then passes one compact structure to the kernel entry point.
//
// Hardware / platform information collected here:
// - ACPI RSDP pointer: root of ACPI platform topology tables.
// - SMBIOS 32-bit / 64-bit entry pointers: root of inventory tables.
// - UEFI memory map descriptor buffer: current physical address ownership.
//
// Purpose:
// - Turn scattered firmware interfaces into one explicit kernel handoff object.
// - Show the difference between "printing firmware data" and "preserving the
//   data a kernel needs after firmware services are gone".
// - Keep this as a learning side model while the project mainline moves toward
//   writing platform-report.json for OS/userspace collection.
//
// What the kernel / OS loader can do with this information:
// - Use AcpiRsdp to parse XSDT/RSDT, MADT, MCFG, FADT, DSDT, and other ACPI
//   tables after the handoff.
// - Use SMBIOS pointers to parse board, BIOS, chassis, CPU, and DIMM inventory.
// - Use the memory map to initialize the physical page allocator while avoiding
//   reserved, runtime, ACPI NVS, and MMIO ranges.
//
// What RD / validation can debug from this:
// - Missing ACPI/SMBIOS pointers explain why kernel-side table discovery fails.
// - MemoryMapSize / DescriptorSize mismatches explain corrupt memory-map walks.
// - MapKey is the value a real loader passes to ExitBootServices(); if the map
//   changes after capture, ExitBootServices() can fail and the loader must retry.
//

STATIC
EFI_STATUS
CaptureMemoryMapForHandoff (
  IN  EFI_BOOT_SERVICES          *BootServices,
  OUT UEFI_BOOT_LAB_BOOT_INFO    *BootInfo
  )
{
  EFI_STATUS             Status;
  UINTN                  MemoryMapSize;
  EFI_MEMORY_DESCRIPTOR  *MemoryMap;
  UINTN                  MapKey;
  UINTN                  DescriptorSize;
  UINT32                 DescriptorVersion;

  MemoryMap         = NULL;
  MemoryMapSize     = 0;
  MapKey            = 0;
  DescriptorSize    = 0;
  DescriptorVersion = 0;

  // First query asks firmware how large the memory-map descriptor buffer must
  // be. The NULL buffer is expected to return EFI_BUFFER_TOO_SMALL.
  Status = BootServices->GetMemoryMap (
                           &MemoryMapSize,
                           MemoryMap,
                           &MapKey,
                           &DescriptorSize,
                           &DescriptorVersion
                           );
  if (Status != EFI_BUFFER_TOO_SMALL) {
    Print(L"BootInfo memory-map size query failed: %r\r\n", Status);
    return Status;
  }

  // Allocate slack because this AllocatePool() call itself can split or add
  // BootServicesData descriptors and make the required map slightly larger.
  MemoryMapSize += DescriptorSize * 8;
  MemoryMap = AllocatePool(MemoryMapSize);
  if (MemoryMap == NULL) {
    Print(L"BootInfo failed to allocate memory-map buffer\r\n");
    return EFI_OUT_OF_RESOURCES;
  }

  // This second call captures the actual descriptor array that the kernel would
  // later walk. A production loader repeats this step if ExitBootServices()
  // reports that the MapKey is stale.
  Status = BootServices->GetMemoryMap (
                           &MemoryMapSize,
                           MemoryMap,
                           &MapKey,
                           &DescriptorSize,
                           &DescriptorVersion
                           );
  if (EFI_ERROR(Status)) {
    Print(L"BootInfo memory-map capture failed: %r\r\n", Status);
    FreePool(MemoryMap);
    return Status;
  }

  BootInfo->MemoryMap                  = MemoryMap;
  BootInfo->MemoryMapSize              = MemoryMapSize;
  BootInfo->MemoryMapDescriptorSize    = DescriptorSize;
  BootInfo->MemoryMapDescriptorVersion = DescriptorVersion;
  BootInfo->MemoryMapKey               = MapKey;

  return EFI_SUCCESS;
}

EFI_STATUS
BuildBootHandoffInfo (
  IN  EFI_SYSTEM_TABLE                              *SystemTable,
  IN  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *AcpiRsdp,
  OUT UEFI_BOOT_LAB_BOOT_INFO                       *BootInfo
  )
{
  EFI_STATUS  Status;

  ZeroMem(BootInfo, sizeof(*BootInfo));

  // ACPI RSDP was already discovered by the ConfigurationTable layer. Keep the
  // pointer instead of copying every ACPI table, because the kernel can follow
  // the ACPI table tree from this root.
  BootInfo->AcpiRsdp = AcpiRsdp;

  // SMBIOS has separate 32-bit and 64-bit entry-point GUIDs. Capturing both
  // lets later code choose the richer SMBIOS 3.x path when firmware provides it.
  BootInfo->SmbiosEntryPoint32 = (SMBIOS_TABLE_ENTRY_POINT *)FindConfigurationTable(
                                                               SystemTable,
                                                               &gEfiSmbiosTableGuid
                                                               );
  BootInfo->SmbiosEntryPoint64 = (SMBIOS_TABLE_3_0_ENTRY_POINT *)FindConfigurationTable(
                                                                 SystemTable,
                                                                 &gEfiSmbios3TableGuid
                                                                 );

  Status = CaptureMemoryMapForHandoff(SystemTable->BootServices, BootInfo);
  if (EFI_ERROR(Status)) {
    ZeroMem(BootInfo, sizeof(*BootInfo));
    return Status;
  }

  return EFI_SUCCESS;
}

VOID
PrintBootHandoffInfo (
  IN CONST UEFI_BOOT_LAB_BOOT_INFO  *BootInfo
  )
{
  UINTN  DescriptorCount;

  DescriptorCount = 0;
  if (BootInfo->MemoryMapDescriptorSize != 0) {
    DescriptorCount = BootInfo->MemoryMapSize / BootInfo->MemoryMapDescriptorSize;
  }

  Print(L"Boot handoff info:\r\n");
  Print(L"  ACPI RSDP:          0x%p\r\n", BootInfo->AcpiRsdp);
  Print(L"  SMBIOS 32-bit EP:   0x%p\r\n", BootInfo->SmbiosEntryPoint32);
  Print(L"  SMBIOS 64-bit EP:   0x%p\r\n", BootInfo->SmbiosEntryPoint64);
  Print(L"  Memory map buffer:  0x%p\r\n", BootInfo->MemoryMap);
  Print(L"  Memory map bytes:   %u\r\n", BootInfo->MemoryMapSize);
  Print(L"  Descriptor size:    %u\r\n", BootInfo->MemoryMapDescriptorSize);
  Print(L"  Descriptor version: %u\r\n", BootInfo->MemoryMapDescriptorVersion);
  Print(L"  Descriptor count:   %u\r\n", DescriptorCount);
  Print(L"  Map key:            %u\r\n", BootInfo->MemoryMapKey);
}

VOID
FreeBootHandoffInfo (
  IN OUT UEFI_BOOT_LAB_BOOT_INFO  *BootInfo
  )
{
  // This lab returns to firmware after printing, so release the buffer. A real
  // bootloader would keep this memory valid and pass BootInfo to the kernel
  // instead of freeing it before the jump.
  if (BootInfo->MemoryMap != NULL) {
    FreePool(BootInfo->MemoryMap);
  }

  ZeroMem(BootInfo, sizeof(*BootInfo));
}
