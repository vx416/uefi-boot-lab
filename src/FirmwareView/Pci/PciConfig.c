#include <Uefi.h>
#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/MemoryMappedConfigurationSpaceAccessTable.h>
#include <Library/UefiLib.h>

#include "PciConfig.h"

//
// PCIe ECAM config-space reader.
//
// Mental model category:
// - PCIe and devices.
// - Device discovery through bus enumeration.
//
// Where this sits in the platform flow:
// - ACPI MCFG tells us the ECAM MMIO base for a PCI segment and bus range.
// - PCIe config space at that ECAM address exposes vendor/device ID, class,
//   header type, BARs, and capability lists for each BDF.
// - The kernel scans this config space to discover actual PCIe devices.
//
// Hardware information printed here:
// - BDF on the first bus covered by each MCFG entry.
// - vendor ID and device ID.
// - class/subclass/prog-if and header type.
// - BAR raw values and the basic I/O/MMIO/64-bit/prefetchable interpretation.
//
// Purpose:
// - Connect MCFG's ECAM resource window to real PCIe enumeration.
// - Show that ACPI tells the OS how to access config space, but PCIe config
//   space tells the OS which endpoints and bridges are actually present.
//
// What the kernel / OS loader can do with this information:
// - Detect host bridges, PCIe root ports, NICs, NVMe controllers, GPUs, etc.
// - Use class code and vendor/device ID to bind drivers.
// - Read BARs to assign or map device MMIO / I/O resources.
//
// What RD / validation can debug from this:
// - Device present in hardware but missing from PCI scan.
// - Wrong class code or vendor/device ID.
// - MCFG/ECAM window mismatch causing all reads to return 0xFFFF.
//

#define PCI_VENDOR_ID_OFFSET   0x00
#define PCI_DEVICE_ID_OFFSET   0x02
#define PCI_PROG_IF_OFFSET     0x09
#define PCI_SUBCLASS_OFFSET    0x0A
#define PCI_CLASS_CODE_OFFSET  0x0B
#define PCI_HEADER_TYPE_OFFSET 0x0E
#define PCI_BAR0_OFFSET        0x10
#define PCI_INVALID_VENDOR_ID  0xFFFF
#define PCI_HEADER_TYPE_MASK   0x7F
#define PCI_HEADER_TYPE_DEVICE 0x00
#define PCI_HEADER_TYPE_BRIDGE 0x01
#define PCI_BAR_IO_SPACE       0x00000001U
#define PCI_BAR_MEM_TYPE_MASK  0x00000006U
#define PCI_BAR_MEM_TYPE_64    0x00000004U
#define PCI_BAR_PREFETCHABLE   0x00000008U
#define PCI_BAR_MEM_ADDR_MASK  0xFFFFFFF0U
#define PCI_BAR_IO_ADDR_MASK   0xFFFFFFFCU

STATIC
BOOLEAN
ValidateMcfgEntry (
  IN EFI_ACPI_MEMORY_MAPPED_ENHANCED_CONFIGURATION_SPACE_BASE_ADDRESS_ALLOCATION_STRUCTURE  *Entry
  )
{
  if (Entry->BaseAddress == 0) {
    Print(L"PCI config scan skipped MCFG entry: ECAM base is zero\r\n");
    return FALSE;
  }

  if (Entry->StartBusNumber > Entry->EndBusNumber) {
    Print(L"PCI config scan skipped MCFG entry: invalid bus range %u-%u\r\n",
          Entry->StartBusNumber,
          Entry->EndBusNumber);
    return FALSE;
  }

  return TRUE;
}

STATIC
UINTN
PciEcamAddress (
  IN UINT64  BaseAddress,
  IN UINT8   StartBus,
  IN UINT8   Bus,
  IN UINT8   Device,
  IN UINT8   Function
  )
{
  // PCIe ECAM gives each function a 4 KiB config-space page. The address
  // formula is defined by PCI firmware/ECAM rules:
  // base + bus_offset + device_offset + function_offset.
  return (UINTN)BaseAddress +
         (((UINTN)Bus - StartBus) << 20) +
         ((UINTN)Device << 15) +
         ((UINTN)Function << 12);
}

STATIC
UINT8
PciRead8 (
  IN UINTN  ConfigBase,
  IN UINTN  Offset
  )
{
  return *(volatile UINT8 *)(ConfigBase + Offset);
}

STATIC
UINT16
PciRead16 (
  IN UINTN  ConfigBase,
  IN UINTN  Offset
  )
{
  return *(volatile UINT16 *)(ConfigBase + Offset);
}

STATIC
UINT32
PciRead32 (
  IN UINTN  ConfigBase,
  IN UINTN  Offset
  )
{
  return *(volatile UINT32 *)(ConfigBase + Offset);
}

STATIC
VOID
PrintPciBar (
  IN UINT8   BarIndex,
  IN UINT32  BarValue
  )
{
  if (BarValue == 0) {
    return;
  }

  if ((BarValue & PCI_BAR_IO_SPACE) != 0) {
    Print (
      L"      BAR%u io   raw=0x%08x base=0x%08x\r\n",
      BarIndex,
      BarValue,
      BarValue & PCI_BAR_IO_ADDR_MASK
      );
    return;
  }

  Print (
    L"      BAR%u mmio raw=0x%08x base=0x%08x type=%s prefetch=%s\r\n",
    BarIndex,
    BarValue,
    BarValue & PCI_BAR_MEM_ADDR_MASK,
    ((BarValue & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64) ? L"64" : L"32",
    ((BarValue & PCI_BAR_PREFETCHABLE) != 0) ? L"yes" : L"no"
    );
}

STATIC
VOID
PrintPciBars (
  IN UINTN  ConfigBase,
  IN UINT8  HeaderType
  )
{
  UINT8   HeaderLayout;
  UINT8   BarCount;
  UINT8   BarIndex;
  UINTN   Offset;
  UINT32  BarValue;

  HeaderLayout = HeaderType & PCI_HEADER_TYPE_MASK;
  if (HeaderLayout == PCI_HEADER_TYPE_DEVICE) {
    BarCount = 6;
  } else if (HeaderLayout == PCI_HEADER_TYPE_BRIDGE) {
    BarCount = 2;
  } else {
    return;
  }

  // BARs are resource request registers. The raw value gives basic information
  // about I/O vs MMIO and the assigned base address. Full sizing requires
  // writing all 1s to the BAR, which this read-only lab intentionally avoids.
  for (BarIndex = 0; BarIndex < BarCount; BarIndex++) {
    Offset   = PCI_BAR0_OFFSET + (BarIndex * sizeof(UINT32));
    BarValue = PciRead32(ConfigBase, Offset);

    PrintPciBar(BarIndex, BarValue);

    if (((BarValue & PCI_BAR_IO_SPACE) == 0) &&
        ((BarValue & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64)) {
      // A 64-bit memory BAR consumes the next BAR register for the high 32
      // bits. Skip it so BAR numbering matches PCI config-space layout.
      BarIndex++;
    }
  }
}

STATIC
VOID
PrintPciFunction (
  IN UINT16  Segment,
  IN UINT8   Bus,
  IN UINT8   Device,
  IN UINT8   Function,
  IN UINTN   ConfigBase
  )
{
  UINT16  VendorId;
  UINT16  DeviceId;
  UINT8   ClassCode;
  UINT8   Subclass;
  UINT8   ProgIf;
  UINT8   HeaderType;

  VendorId = PciRead16(ConfigBase, PCI_VENDOR_ID_OFFSET);
  if (VendorId == PCI_INVALID_VENDOR_ID) {
    return;
  }

  DeviceId   = PciRead16(ConfigBase, PCI_DEVICE_ID_OFFSET);
  ProgIf     = PciRead8(ConfigBase, PCI_PROG_IF_OFFSET);
  Subclass   = PciRead8(ConfigBase, PCI_SUBCLASS_OFFSET);
  ClassCode  = PciRead8(ConfigBase, PCI_CLASS_CODE_OFFSET);
  HeaderType = PciRead8(ConfigBase, PCI_HEADER_TYPE_OFFSET);

  Print (
    L"%04x:%02x:%02x.%u vendor=0x%04x device=0x%04x class=0x%02x subclass=0x%02x prog_if=0x%02x header=0x%02x\r\n",
    Segment,
    Bus,
    Device,
    Function,
    VendorId,
    DeviceId,
    ClassCode,
    Subclass,
    ProgIf,
    HeaderType
    );

  PrintPciBars(ConfigBase, HeaderType);
}

STATIC
VOID
ScanPciBus (
  IN EFI_ACPI_MEMORY_MAPPED_ENHANCED_CONFIGURATION_SPACE_BASE_ADDRESS_ALLOCATION_STRUCTURE  *Entry
  )
{
  UINT8  Bus;
  UINT8  Device;
  UINT8  Function;
  UINTN  ConfigBase;

  Bus = Entry->StartBusNumber;

  Print(L"PCI config scan: segment=%u bus=%u ECAM=0x%016lx\r\n",
        Entry->PciSegmentGroupNumber,
        Bus,
        Entry->BaseAddress);
  Print(L"BDF          IDs and class\r\n");

  // Keep the first lab scan intentionally small: scan only the first bus in
  // each ECAM range. Later labs can recurse through PCI bridges to find all
  // downstream buses.
  for (Device = 0; Device < 32; Device++) {
    for (Function = 0; Function < 8; Function++) {
      ConfigBase = PciEcamAddress(
                     Entry->BaseAddress,
                     Entry->StartBusNumber,
                     Bus,
                     Device,
                     Function
                     );
      PrintPciFunction(
        Entry->PciSegmentGroupNumber,
        Bus,
        Device,
        Function,
        ConfigBase
        );
    }
  }
}

VOID
PrintPciDevicesFromMcfg (
  IN EFI_ACPI_DESCRIPTION_HEADER  *Mcfg
  )
{
  EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE_HEADER                    *Header;
  EFI_ACPI_MEMORY_MAPPED_ENHANCED_CONFIGURATION_SPACE_BASE_ADDRESS_ALLOCATION_STRUCTURE  *Entry;
  UINT8                                                                             *End;
  UINTN                                                                             EntryCount;
  UINTN                                                                             Index;

  if (Mcfg == NULL) {
    Print(L"PCI config scan skipped: MCFG table not found\r\n");
    return;
  }

  if (Mcfg->Signature != EFI_ACPI_6_6_PCI_EXPRESS_MEMORY_MAPPED_CONFIGURATION_SPACE_BASE_ADDRESS_DESCRIPTION_TABLE_SIGNATURE) {
    Print(L"PCI config scan skipped: MCFG signature mismatch\r\n");
    return;
  }

  if (Mcfg->Length < sizeof(EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE_HEADER)) {
    Print(L"PCI config scan skipped: MCFG length too small (%u)\r\n", Mcfg->Length);
    return;
  }

  Header     = (EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE_HEADER *)Mcfg;
  Entry      = (EFI_ACPI_MEMORY_MAPPED_ENHANCED_CONFIGURATION_SPACE_BASE_ADDRESS_ALLOCATION_STRUCTURE *)((UINT8 *)Header + sizeof(*Header));
  End        = (UINT8 *)Header + Header->Header.Length;
  EntryCount = (Header->Header.Length - sizeof(*Header)) / sizeof(*Entry);

  if (((Header->Header.Length - sizeof(*Header)) % sizeof(*Entry)) != 0) {
    Print(L"PCI config scan warning: MCFG entry bytes are not an exact multiple of entry size\r\n");
  }

  for (Index = 0; (Index < EntryCount) && ((UINT8 *)Entry < End); Index++) {
    // This lab still performs direct volatile ECAM reads below. The checks here
    // only filter obviously bad MCFG records; they do not make arbitrary
    // firmware ECAM windows production-safe to dereference.
    if (ValidateMcfgEntry(Entry)) {
      ScanPciBus(Entry);
    }

    Entry++;
  }
}
