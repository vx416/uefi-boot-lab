#include <Uefi.h>
#include <Guid/Acpi.h>
#include <Guid/UefiBootLabPlatformTable.h>
#include <IndustryStandard/Acpi.h>
#include <Library/UefiLib.h>

#include "Acpi/AcpiTables.h"
#include "Firmware/BootInfo.h"
#include "Firmware/ConfigTables.h"
#include "Firmware/ProducerLoader.h"
#include "Memory/MemoryMap.h"
#include "Report/FirmwareJson.h"
#include "Smbios/Smbios.h"

//
// Main lab entry point.
//
// Mental model category:
// - Firmware boot path.
// - Lab orchestration layer.
//
// Where this sits in the platform flow:
// - OVMF loads BOOTX64.EFI.
// - UEFI calls UefiMain() and passes EFI_SYSTEM_TABLE.
// - This file prints basic firmware identity, then routes to focused
//   hardware-description printers.
//
// Hardware information printed here:
// - Firmware vendor.
// - UEFI revision.
//
// Purpose:
// - Prove that OVMF loaded this UEFI application and passed a valid
//   EFI_SYSTEM_TABLE.
// - Route the lab flow to focused hardware-description printers.
//
// What the kernel / OS loader can do with this level of information:
// - Confirm the firmware interface version it is running under.
// - Decide whether expected UEFI services and table formats should exist.
//
// What RD / validation can debug from this:
// - Wrong firmware vendor or revision in logs can indicate the wrong OVMF,
//   BIOS image, or boot path was used.
// - If this entry point never prints, debug the boot chain first:
//   QEMU disk mapping, default BOOTX64.EFI path, OVMF boot manager, or shell.
//
EFI_STATUS
EFIAPI
UefiMain (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *AcpiRsdp;
  UEFI_BOOT_LAB_BOOT_INFO                       BootInfo;
  EFI_STATUS                                    Status;

  // UefiMain is the entry point. Keep it as the high-level lab flow and put
  // each hardware-description experiment in its own focused source file.
  Print(L"Firmware view collector for uefi-boot-lab\r\n");
  Print(L"Firmware vendor: %s\r\n", SystemTable->FirmwareVendor);
  Print(L"UEFI revision: %u.%u\r\n",
        SystemTable->Hdr.Revision >> 16,
        SystemTable->Hdr.Revision & 0xFFFF);
  Print(L"\r\n");

  // If a custom OVMF image already contains FirmwareProducerDxe, its DXE driver
  // should have published this table before FirmwareView.efi starts. If the
  // table is missing, load the standalone producer from the EFI volume so the
  // same producer/consumer flow still works with stock OVMF.
  if (FindConfigurationTable(SystemTable, &gUefiBootLabPlatformTableGuid) != NULL) {
    Print(L"Lab firmware producer table already present in firmware image.\r\n");
  } else {
    Status = LoadFirmwareProducerDriver(ImageHandle);
    if (EFI_ERROR(Status)) {
      Print(L"Continuing without lab firmware producer table.\r\n");
    }
  }
  Print(L"\r\n");

  PrintConfigurationTables(SystemTable);
  Print(L"\r\n");

  // Real bootloaders use ConfigurationTable as a directory: find the ACPI root
  // pointer first, then pass that pointer to the ACPI parser layer.
  AcpiRsdp = FindConfigurationTable(SystemTable, &gEfiAcpi20TableGuid);
  if (AcpiRsdp == NULL) {
    AcpiRsdp = FindConfigurationTable(SystemTable, &gEfiAcpi10TableGuid);
  }

  PrintAcpiTables(AcpiRsdp);
  Print(L"\r\n");

  // This is the transition point from "print what firmware exposes" to "build
  // the compact handoff object a kernel would receive from a bootloader".
  Status = BuildBootHandoffInfo(SystemTable, AcpiRsdp, &BootInfo);
  if (!EFI_ERROR(Status)) {
    PrintBootHandoffInfo(&BootInfo);
    Print(L"\r\n");

    PrintSmbiosBasicInfo(BootInfo.SmbiosEntryPoint32, BootInfo.SmbiosEntryPoint64);
    Print(L"\r\n");

    Status = WriteFirmwareViewJson(ImageHandle, SystemTable, AcpiRsdp, &BootInfo);
    if (EFI_ERROR(Status)) {
      Print(L"Failed to write firmware-view.json: %r\r\n", Status);
    } else {
      Print(L"Wrote EFI\\UEFI-BOOT-LAB\\firmware-view.json\r\n");
    }
    Print(L"\r\n");
  }

  // Print the same memory-map snapshot that was written to firmware-view.json.
  // Calling GetMemoryMap() again here would produce a newer map because file
  // writes and pool allocations can change Boot Services descriptors.
  if (!EFI_ERROR(Status)) {
    PrintMemoryMapSnapshot(
      BootInfo.MemoryMap,
      BootInfo.MemoryMapSize,
      BootInfo.MemoryMapDescriptorSize
      );
  }

  if (!EFI_ERROR(Status)) {
    FreeBootHandoffInfo(&BootInfo);
  }

  return EFI_SUCCESS;
}
