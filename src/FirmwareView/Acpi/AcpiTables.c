#include <Uefi.h>
#include <IndustryStandard/Acpi.h>
#include <Library/UefiLib.h>

#include "AcpiTables.h"
#include "Madt.h"
#include "Mcfg.h"
#include "../Pci/PciConfig.h"

//
// ACPI table discovery printer.
//
// Mental model category:
// - Hardware description layer.
// - Topology / layout, device control method, and platform resource
//   description.
//
// Where this sits in the platform flow:
// - The ConfigurationTable layer finds the ACPI RSDP root pointer.
// - This ACPI layer receives that RSDP pointer instead of scanning
//   EFI_SYSTEM_TABLE again.
// - RSDP points to XSDT/RSDT.
// - XSDT/RSDT points to individual ACPI tables such as MADT, FACP, DSDT,
//   MCFG, SRAT, DMAR, HEST, and BERT.
//
// Hardware information printed here:
// - ACPI RSDP address and basic version fields.
// - RSDT/XSDT addresses published by firmware.
// - ACPI table signatures, lengths, revisions, and physical addresses.
//
// Purpose:
// - Move from "ConfigTable has an ACPI entry" to "which ACPI tables did
//   firmware publish?".
// - Build the next step toward parsing platform topology and control data.
//
// What the kernel / OS loader can do with this information:
// - Locate MADT for CPU/interrupt topology.
// - Locate SRAT/SLIT for NUMA topology.
// - Locate MCFG for PCIe ECAM config space.
// - Locate DMAR/IVRS/IORT for IOMMU/DMA remapping.
// - Locate DSDT/SSDT/FADT for ACPI namespace, power, and platform methods.
//
// What RD / validation can debug from this:
// - Missing MADT can explain broken CPU or interrupt discovery.
// - Missing MCFG can explain PCIe ECAM/config-space discovery issues.
// - Missing SRAT/SLIT can explain NUMA topology mismatch.
// - Missing DMAR/IVRS/IORT can explain IOMMU or DMA remapping issues.
//

STATIC
VOID
PrintAcpiSignature (
  IN UINT32  Signature
  )
{
  // ACPI signatures are four ASCII bytes packed into a UINT32, for example
  // XSDT, FACP, APIC, MCFG, SRAT, DMAR, or SSDT.
  Print (
    L"%c%c%c%c",
    (CHAR16)(Signature & 0xFF),
    (CHAR16)((Signature >> 8) & 0xFF),
    (CHAR16)((Signature >> 16) & 0xFF),
    (CHAR16)((Signature >> 24) & 0xFF)
    );
}

STATIC
VOID
PrintAcpiTableHeader (
  IN UINTN                        Index,
  IN EFI_ACPI_DESCRIPTION_HEADER  *Header
  )
{
  Print(L"%03u ", Index);
  PrintAcpiSignature(Header->Signature);
  Print (
    L" 0x%016lx %8u %3u\r\n",
    (UINTN)Header,
    Header->Length,
    Header->Revision
    );
}

STATIC
UINT8
AcpiChecksum (
  IN CONST UINT8  *Bytes,
  IN UINT32       Length
  )
{
  UINT8   Sum;
  UINT32  Index;

  Sum = 0;
  for (Index = 0; Index < Length; Index++) {
    Sum = (UINT8)(Sum + Bytes[Index]);
  }

  return Sum;
}

STATIC
BOOLEAN
ValidateAcpiTable (
  IN EFI_ACPI_DESCRIPTION_HEADER  *Header,
  IN UINT32                       ExpectedSignature
  )
{
  if (Header == NULL) {
    Print(L"ACPI table validation failed: NULL pointer\r\n");
    return FALSE;
  }

  if (Header->Length < sizeof(EFI_ACPI_DESCRIPTION_HEADER)) {
    Print(L"ACPI table validation failed at 0x%016lx: length too small (%u)\r\n",
          (UINTN)Header,
          Header->Length);
    return FALSE;
  }

  if ((ExpectedSignature != 0) && (Header->Signature != ExpectedSignature)) {
    Print(L"ACPI table validation failed at 0x%016lx: expected ", (UINTN)Header);
    PrintAcpiSignature(ExpectedSignature);
    Print(L" got ");
    PrintAcpiSignature(Header->Signature);
    Print(L"\r\n");
    return FALSE;
  }

  // ACPI table checksum is the 8-bit sum of every table byte. A valid table
  // sums to zero. This catches truncated or corrupt tables before deeper
  // parsers trust Length and variable records.
  if (AcpiChecksum((CONST UINT8 *)Header, Header->Length) != 0) {
    Print(L"ACPI table validation failed at 0x%016lx: checksum mismatch for ",
          (UINTN)Header);
    PrintAcpiSignature(Header->Signature);
    Print(L"\r\n");
    return FALSE;
  }

  return TRUE;
}

STATIC
EFI_ACPI_DESCRIPTION_HEADER *
FindAcpiTableInXsdt (
  IN EFI_ACPI_DESCRIPTION_HEADER  *Xsdt,
  IN UINT32                       Signature
  )
{
  UINTN   EntryCount;
  UINTN   Index;
  UINT64  *Entries;
  EFI_ACPI_DESCRIPTION_HEADER  *Header;

  if (!ValidateAcpiTable(
         Xsdt,
         EFI_ACPI_2_0_EXTENDED_SYSTEM_DESCRIPTION_TABLE_SIGNATURE
         )) {
    return NULL;
  }

  EntryCount = (Xsdt->Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) / sizeof(UINT64);
  Entries    = (UINT64 *)((UINT8 *)Xsdt + sizeof(EFI_ACPI_DESCRIPTION_HEADER));

  // XSDT is the ACPI table directory. This helper lets later parsers receive
  // a concrete table pointer instead of rescanning or reparsing the whole root.
  for (Index = 0; Index < EntryCount; Index++) {
    if (Entries[Index] == 0) {
      continue;
    }

    Header = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Entries[Index];
    if (ValidateAcpiTable(Header, 0) && (Header->Signature == Signature)) {
      return Header;
    }
  }

  return NULL;
}

STATIC
EFI_ACPI_DESCRIPTION_HEADER *
FindAcpiTableInRsdt (
  IN EFI_ACPI_DESCRIPTION_HEADER  *Rsdt,
  IN UINT32                       Signature
  )
{
  UINTN   EntryCount;
  UINTN   Index;
  UINT32  *Entries;
  EFI_ACPI_DESCRIPTION_HEADER  *Header;

  if (!ValidateAcpiTable(
         Rsdt,
         EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_TABLE_SIGNATURE
         )) {
    return NULL;
  }

  EntryCount = (Rsdt->Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) / sizeof(UINT32);
  Entries    = (UINT32 *)((UINT8 *)Rsdt + sizeof(EFI_ACPI_DESCRIPTION_HEADER));

  for (Index = 0; Index < EntryCount; Index++) {
    if (Entries[Index] == 0) {
      continue;
    }

    Header = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Entries[Index];
    if (ValidateAcpiTable(Header, 0) && (Header->Signature == Signature)) {
      return Header;
    }
  }

  return NULL;
}

STATIC
VOID
PrintXsdtTables (
  IN EFI_ACPI_DESCRIPTION_HEADER  *Xsdt
  )
{
  UINTN   EntryCount;
  UINTN   Index;
  UINT64  *Entries;

  if (Xsdt == NULL) {
    Print(L"XSDT not available\r\n");
    return;
  }

  if (!ValidateAcpiTable(
         Xsdt,
         EFI_ACPI_2_0_EXTENDED_SYSTEM_DESCRIPTION_TABLE_SIGNATURE
         )) {
    return;
  }

  EntryCount = (Xsdt->Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) / sizeof(UINT64);
  Entries    = (UINT64 *)((UINT8 *)Xsdt + sizeof(EFI_ACPI_DESCRIPTION_HEADER));

  Print(L"XSDT: 0x%016lx tables=%u length=%u revision=%u\r\n",
        (UINTN)Xsdt,
        EntryCount,
        Xsdt->Length,
        Xsdt->Revision);
  Print(L"Idx Sig  Address            Length   Rev\r\n");

  // Each XSDT entry is a 64-bit physical address of another ACPI table. UEFI
  // identity maps these addresses for us while the application is running.
  for (Index = 0; Index < EntryCount; Index++) {
    if (Entries[Index] == 0) {
      Print(L"%03u <NULL ACPI table entry>\r\n", Index);
      continue;
    }

    if (!ValidateAcpiTable((EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Entries[Index], 0)) {
      Print(L"%03u <invalid ACPI table at 0x%016lx>\r\n", Index, Entries[Index]);
      continue;
    }

    PrintAcpiTableHeader(
      Index,
      (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Entries[Index]
      );
  }
}

STATIC
VOID
PrintRsdtTables (
  IN EFI_ACPI_DESCRIPTION_HEADER  *Rsdt
  )
{
  UINTN   EntryCount;
  UINTN   Index;
  UINT32  *Entries;

  if (Rsdt == NULL) {
    Print(L"RSDT not available\r\n");
    return;
  }

  if (!ValidateAcpiTable(
         Rsdt,
         EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_TABLE_SIGNATURE
         )) {
    return;
  }

  EntryCount = (Rsdt->Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) / sizeof(UINT32);
  Entries    = (UINT32 *)((UINT8 *)Rsdt + sizeof(EFI_ACPI_DESCRIPTION_HEADER));

  Print(L"RSDT: 0x%016lx tables=%u length=%u revision=%u\r\n",
        (UINTN)Rsdt,
        EntryCount,
        Rsdt->Length,
        Rsdt->Revision);
  Print(L"Idx Sig  Address            Length   Rev\r\n");

  // RSDT is the legacy form of the ACPI root table. Entries are 32-bit
  // physical addresses, so it cannot describe tables above the 4 GiB boundary.
  for (Index = 0; Index < EntryCount; Index++) {
    if (Entries[Index] == 0) {
      Print(L"%03u <NULL ACPI table entry>\r\n", Index);
      continue;
    }

    if (!ValidateAcpiTable((EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Entries[Index], 0)) {
      Print(L"%03u <invalid ACPI table at 0x%08x>\r\n", Index, Entries[Index]);
      continue;
    }

    PrintAcpiTableHeader(
      Index,
      (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Entries[Index]
      );
  }
}

VOID
PrintAcpiTables (
  IN EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *Rsdp
  )
{
  EFI_ACPI_DESCRIPTION_HEADER  *RootTable;
  EFI_ACPI_DESCRIPTION_HEADER  *Madt;
  EFI_ACPI_DESCRIPTION_HEADER  *Mcfg;

  if (Rsdp == NULL) {
    Print(L"ACPI RSDP pointer is NULL\r\n");
    return;
  }

  Print(L"ACPI RSDP: 0x%016lx\r\n", (UINTN)Rsdp);
  Print(L"  Revision: %u\r\n", Rsdp->Revision);
  Print(L"  RSDT:     0x%08x\r\n", Rsdp->RsdtAddress);

  if (Rsdp->Revision >= EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER_REVISION) {
    Print(L"  Length:   %u\r\n", Rsdp->Length);
    Print(L"  XSDT:     0x%016lx\r\n", Rsdp->XsdtAddress);
    Print(L"\r\n");

    RootTable = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->XsdtAddress;
    PrintXsdtTables(RootTable);

    Madt = FindAcpiTableInXsdt(
             RootTable,
             EFI_ACPI_6_6_MULTIPLE_APIC_DESCRIPTION_TABLE_SIGNATURE
             );
    Print(L"\r\n");
    PrintMadt(Madt);

    Mcfg = FindAcpiTableInXsdt(
             RootTable,
             EFI_ACPI_6_6_PCI_EXPRESS_MEMORY_MAPPED_CONFIGURATION_SPACE_BASE_ADDRESS_DESCRIPTION_TABLE_SIGNATURE
             );
    Print(L"\r\n");
    PrintMcfg(Mcfg);
    Print(L"\r\n");
    PrintPciDevicesFromMcfg(Mcfg);
    return;
  }

  Print(L"\r\n");
  RootTable = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->RsdtAddress;
  PrintRsdtTables(RootTable);

  Madt = FindAcpiTableInRsdt(
           RootTable,
           EFI_ACPI_6_6_MULTIPLE_APIC_DESCRIPTION_TABLE_SIGNATURE
           );
  Print(L"\r\n");
  PrintMadt(Madt);

  Mcfg = FindAcpiTableInRsdt(
           RootTable,
           EFI_ACPI_6_6_PCI_EXPRESS_MEMORY_MAPPED_CONFIGURATION_SPACE_BASE_ADDRESS_DESCRIPTION_TABLE_SIGNATURE
           );
  Print(L"\r\n");
  PrintMcfg(Mcfg);
  Print(L"\r\n");
  PrintPciDevicesFromMcfg(Mcfg);
}
