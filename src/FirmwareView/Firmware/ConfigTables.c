#include <Uefi.h>
#include <Guid/Acpi.h>
#include <Guid/DebugImageInfoTable.h>
#include <Guid/Fdt.h>
#include <Guid/HobList.h>
#include <Guid/JsonCapsule.h>
#include <Guid/MemoryAttributesTable.h>
#include <Guid/Mps.h>
#include <Guid/RtPropertiesTable.h>
#include <Guid/SmBios.h>
#include <Guid/SystemResourceTable.h>
#include <Guid/UefiBootLabPlatformTable.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiLib.h>

#include "ConfigTables.h"

// Some OVMF configuration-table entries use GUIDs that are not hardware
// descriptions. Naming them still helps separate firmware-internal entries
// from real platform-description roots such as ACPI and SMBIOS.
STATIC CONST EFI_GUID  mLzmaCustomDecompressGuid = {
  0xEE4E5898,
  0x3914,
  0x4259,
  { 0x9D, 0x6E, 0xDC, 0x7B, 0xD7, 0x94, 0x03, 0xCF }
};

//
// UEFI configuration table printer.
//
// Mental model category:
// - Boundary between Firmware boot path and Hardware description layer.
//
// Where this sits in the platform flow:
// - EFI_SYSTEM_TABLE comes from the UEFI boot path.
// - EFI_SYSTEM_TABLE.ConfigurationTable[] is a GUID-indexed directory of
//   firmware-published description interfaces.
// - Entries here point to roots such as ACPI RSDP, SMBIOS, memory attributes,
//   runtime properties, or firmware-internal/vendor tables.
//
// Hardware information printed here:
// - GUID-tagged firmware table entries from EFI_SYSTEM_TABLE.
// - VendorTable pointers for important hardware-description roots.
// - Readable names for ACPI RSDP and SMBIOS entry points.
// - Readable names for other common UEFI configuration / description tables
//   when this firmware publishes them.
//
// Purpose:
// - Show where firmware publishes root pointers for platform description
//   data structures such as ACPI and SMBIOS.
// - Bridge this UEFI lab to Linux tools such as acpidump and dmidecode.
//
// What the kernel / OS loader can do with this information:
// - Locate ACPI RSDP, then find RSDT/XSDT and tables such as MADT, SRAT,
//   SLIT, DMAR, HEST, and BERT.
// - Locate SMBIOS entry points and parse BIOS, system, board, chassis,
//   processor, and DIMM inventory structures.
// - Discover optional interfaces such as FDT, memory attributes, runtime
//   properties, or firmware debug/resource tables when present.
//
// What RD / validation can debug from this:
// - Missing ACPI entries can explain why Linux cannot discover topology,
//   interrupt routing, IOMMU, NUMA, or power-management data.
// - Missing SMBIOS entries can explain bad dmidecode or fleet inventory data.
// - Wrong VendorTable addresses suggest firmware table publication issues
//   before debugging OS parser behavior.
//
STATIC
CONST CHAR16 *
ConfigurationTableName (
  IN EFI_GUID  *VendorGuid
  )
{
  // gEfiAcpiTableGuid and gEfiAcpi20TableGuid can resolve to the same ACPI
  // 2.0+ table GUID in EDK II, so check the explicit RSDP name first.
  if (CompareGuid(VendorGuid, &gEfiAcpi20TableGuid)) {
    return L"ACPI 2.0+ RSDP";
  }

  if (CompareGuid(VendorGuid, &gEfiAcpi10TableGuid)) {
    return L"ACPI 1.0 RSDP";
  }

  if (CompareGuid(VendorGuid, &gEfiAcpiTableGuid)) {
    return L"ACPI Table";
  }

  if (CompareGuid(VendorGuid, &gEfiSmbios3TableGuid)) {
    return L"SMBIOS 3.x Entry";
  }

  if (CompareGuid(VendorGuid, &gEfiSmbiosTableGuid)) {
    return L"SMBIOS Entry";
  }

  if (CompareGuid(VendorGuid, &gFdtTableGuid)) {
    return L"Flattened Device Tree";
  }

  if (CompareGuid(VendorGuid, &gEfiMpsTableGuid)) {
    return L"Intel MPS Table";
  }

  if (CompareGuid(VendorGuid, &gEfiMemoryAttributesTableGuid)) {
    return L"UEFI Memory Attributes Table";
  }

  if (CompareGuid(VendorGuid, &gEfiRtPropertiesTableGuid)) {
    return L"UEFI Runtime Properties Table";
  }

  if (CompareGuid(VendorGuid, &gEfiSystemResourceTableGuid)) {
    return L"EFI System Resource Table";
  }

  if (CompareGuid(VendorGuid, &gEfiDebugImageInfoTableGuid)) {
    return L"Debug Image Info Table";
  }

  if (CompareGuid(VendorGuid, &gEfiHobListGuid)) {
    return L"HOB List";
  }

  if (CompareGuid(VendorGuid, &gEfiJsonConfigDataTableGuid)) {
    return L"JSON Config Data Table";
  }

  if (CompareGuid(VendorGuid, &gEfiJsonCapsuleDataTableGuid)) {
    return L"JSON Capsule Data Table";
  }

  if (CompareGuid(VendorGuid, &gEfiJsonCapsuleResultTableGuid)) {
    return L"JSON Capsule Result Table";
  }

  if (CompareGuid(VendorGuid, &mLzmaCustomDecompressGuid)) {
    return L"LZMA Custom Decompress";
  }

  if (CompareGuid(VendorGuid, &gUefiBootLabPlatformTableGuid)) {
    return L"UEFI Boot Lab Platform Table";
  }

  return L"Unknown";
}

STATIC
VOID
PrintGuid (
  IN EFI_GUID  *Guid
  )
{
  // Print GUID fields in the standard 8-4-4-4-12 layout so the output can be
  // compared with UEFI spec names or EDK II header definitions.
  Print (
    L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    Guid->Data1,
    Guid->Data2,
    Guid->Data3,
    Guid->Data4[0],
    Guid->Data4[1],
    Guid->Data4[2],
    Guid->Data4[3],
    Guid->Data4[4],
    Guid->Data4[5],
    Guid->Data4[6],
    Guid->Data4[7]
    );
}

VOID *
FindConfigurationTable (
  IN EFI_SYSTEM_TABLE  *SystemTable,
  IN EFI_GUID          *VendorGuid
  )
{
  UINTN                    Index;
  EFI_CONFIGURATION_TABLE  *Table;

  // ConfigurationTable is a GUID-tagged directory. The GUID identifies the
  // kind of root interface we want, and VendorTable is the pointer we pass to
  // the parser for that interface.
  for (Index = 0; Index < SystemTable->NumberOfTableEntries; Index++) {
    Table = &SystemTable->ConfigurationTable[Index];

    if (CompareGuid(&Table->VendorGuid, VendorGuid)) {
      return Table->VendorTable;
    }
  }

  return NULL;
}

VOID
PrintConfigurationTables (
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINTN                    Index;
  EFI_CONFIGURATION_TABLE  *Table;

  Print(L"Configuration tables: %u\r\n", SystemTable->NumberOfTableEntries);
  Print(L"Idx VendorGuid                           VendorTable        Name\r\n");

  // The system table exposes an array of GUID-tagged pointers. ACPI, SMBIOS,
  // and other firmware descriptions are discovered by matching these GUIDs.
  for (Index = 0; Index < SystemTable->NumberOfTableEntries; Index++) {
    Table = &SystemTable->ConfigurationTable[Index];

    Print(L"%03u ", Index);
    PrintGuid(&Table->VendorGuid);
    Print (
      L" 0x%016lx %s\r\n",
      (UINTN)Table->VendorTable,
      ConfigurationTableName(&Table->VendorGuid)
      );
  }
}
