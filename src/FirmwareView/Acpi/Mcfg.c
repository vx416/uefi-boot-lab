#include <Uefi.h>
#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/MemoryMappedConfigurationSpaceAccessTable.h>
#include <Library/UefiLib.h>

#include "Mcfg.h"

//
// MCFG / PCIe ECAM table printer.
//
// Mental model category:
// - Hardware description layer.
// - Resource ownership / PCIe configuration-space access.
//
// Where this sits in the platform flow:
// - ACPI table discovery finds the table with signature "MCFG".
// - MCFG contains one or more ECAM base address allocation records.
// - Each record tells the kernel which PCI segment and bus range can be
//   accessed through a memory-mapped PCIe configuration-space window.
//
// Hardware information printed here:
// - ECAM base address.
// - PCI segment group number.
// - start/end PCI bus number covered by that ECAM window.
//
// Purpose:
// - Connect ACPI platform description to PCIe enumeration.
// - Show where kernel PCI code learns how to access PCIe config space before
//   reading vendor/device IDs, BDFs, BARs, and PCIe capability lists.
//
// What the kernel / OS loader can do with this information:
// - Map ECAM MMIO and scan PCIe bus/device/function config space.
// - Discover PCIe endpoints such as NIC, NVMe, GPU, and bridge devices.
// - Read BARs to learn device MMIO resource requirements.
//
// What RD / validation can debug from this:
// - PCIe devices missing from lspci.
// - ECAM access faults or wrong segment/bus range.
// - BAR/resource allocation mismatches caused by bad PCIe root description.
//

VOID
PrintMcfg (
  IN EFI_ACPI_DESCRIPTION_HEADER  *Mcfg
  )
{
  EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE_HEADER                    *Header;
  EFI_ACPI_MEMORY_MAPPED_ENHANCED_CONFIGURATION_SPACE_BASE_ADDRESS_ALLOCATION_STRUCTURE  *Entry;
  UINT8                                                                             *End;
  UINTN                                                                             EntryCount;
  UINTN                                                                             Index;

  if (Mcfg == NULL) {
    Print(L"MCFG table not found\r\n");
    return;
  }

  if (Mcfg->Signature != EFI_ACPI_6_6_PCI_EXPRESS_MEMORY_MAPPED_CONFIGURATION_SPACE_BASE_ADDRESS_DESCRIPTION_TABLE_SIGNATURE) {
    Print(L"MCFG signature mismatch\r\n");
    return;
  }

  if (Mcfg->Length < sizeof(EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE_HEADER)) {
    Print(L"MCFG length too small: %u\r\n", Mcfg->Length);
    return;
  }

  Header     = (EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE_HEADER *)Mcfg;
  Entry      = (EFI_ACPI_MEMORY_MAPPED_ENHANCED_CONFIGURATION_SPACE_BASE_ADDRESS_ALLOCATION_STRUCTURE *)((UINT8 *)Header + sizeof(*Header));
  End        = (UINT8 *)Header + Header->Header.Length;
  EntryCount = (Header->Header.Length - sizeof(*Header)) / sizeof(*Entry);

  if (((Header->Header.Length - sizeof(*Header)) % sizeof(*Entry)) != 0) {
    Print(L"MCFG warning: trailing bytes after ECAM allocation entries\r\n");
  }

  Print(L"MCFG: 0x%016lx length=%u revision=%u entries=%u\r\n",
        (UINTN)Header,
        Header->Header.Length,
        Header->Header.Revision,
        EntryCount);
  Print(L"Idx ECAM Base          Segment StartBus EndBus\r\n");

  // Each MCFG allocation record describes one PCI segment's ECAM MMIO window.
  // Bus/device/function config-space addresses are calculated from this base.
  for (Index = 0; (Index < EntryCount) && ((UINT8 *)Entry < End); Index++) {
    if ((Entry->BaseAddress == 0) || (Entry->StartBusNumber > Entry->EndBusNumber)) {
      Print (
        L"%03u invalid entry base=0x%016lx segment=%u start=%u end=%u\r\n",
        Index,
        Entry->BaseAddress,
        Entry->PciSegmentGroupNumber,
        Entry->StartBusNumber,
        Entry->EndBusNumber
        );
      Entry++;
      continue;
    }

    Print (
      L"%03u 0x%016lx %7u %8u %6u\r\n",
      Index,
      Entry->BaseAddress,
      Entry->PciSegmentGroupNumber,
      Entry->StartBusNumber,
      Entry->EndBusNumber
      );

    Entry++;
  }
}
