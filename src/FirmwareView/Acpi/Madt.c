#include <Uefi.h>
#include <IndustryStandard/Acpi.h>
#include <Library/UefiLib.h>

#include "Madt.h"

//
// MADT / APIC table printer.
//
// Mental model category:
// - Hardware description layer.
// - Topology / layout for CPU interrupt controllers.
//
// Where this sits in the platform flow:
// - ACPI table discovery finds the table with signature "APIC".
// - That table is the MADT: Multiple APIC Description Table.
// - MADT contains variable-length records for local APIC, I/O APIC,
//   interrupt source overrides, NMI configuration, and newer APIC variants.
//
// Hardware information printed here:
// - Local APIC base address and MADT flags.
// - Processor local APIC entries for logical processor/APIC IDs.
// - I/O APIC entries for interrupt controller MMIO address and GSI base.
// - Interrupt source override and local APIC NMI entries when present.
//
// Purpose:
// - Show how ACPI moves from "there is an APIC table" to concrete CPU and
//   interrupt topology records.
//
// What the kernel / OS loader can do with this information:
// - Discover logical processors and APIC IDs.
// - Discover I/O APIC controllers and their MMIO addresses.
// - Map legacy IRQ sources to global system interrupts.
//
// What RD / validation can debug from this:
// - CPU count or APIC ID mismatch.
// - Missing or wrong I/O APIC address.
// - Incorrect interrupt source override causing bad IRQ routing.
//

typedef struct {
  UINT8  Type;
  UINT8  Length;
} MADT_ENTRY_HEADER;

STATIC
CONST CHAR16 *
MadtEntryTypeName (
  IN UINT8  Type
  )
{
  switch (Type) {
    case EFI_ACPI_6_6_PROCESSOR_LOCAL_APIC:
      return L"Local APIC";
    case EFI_ACPI_6_6_IO_APIC:
      return L"I/O APIC";
    case EFI_ACPI_6_6_INTERRUPT_SOURCE_OVERRIDE:
      return L"IRQ Override";
    case EFI_ACPI_6_6_NON_MASKABLE_INTERRUPT_SOURCE:
      return L"NMI Source";
    case EFI_ACPI_6_6_LOCAL_APIC_NMI:
      return L"Local APIC NMI";
    case EFI_ACPI_6_6_LOCAL_APIC_ADDRESS_OVERRIDE:
      return L"LAPIC Addr Override";
    case EFI_ACPI_6_6_PROCESSOR_LOCAL_X2APIC:
      return L"Local x2APIC";
    case EFI_ACPI_6_6_LOCAL_X2APIC_NMI:
      return L"Local x2APIC NMI";
    default:
      return L"Unknown";
  }
}

STATIC
VOID
PrintLocalApic (
  IN UINTN  Index,
  IN VOID   *Entry
  )
{
  EFI_ACPI_6_6_PROCESSOR_LOCAL_APIC_STRUCTURE  *LocalApic;

  LocalApic = (EFI_ACPI_6_6_PROCESSOR_LOCAL_APIC_STRUCTURE *)Entry;
  Print (
    L"%03u Local APIC      uid=%u apic_id=%u flags=0x%08x\r\n",
    Index,
    LocalApic->AcpiProcessorUid,
    LocalApic->ApicId,
    LocalApic->Flags
    );
}

STATIC
VOID
PrintIoApic (
  IN UINTN  Index,
  IN VOID   *Entry
  )
{
  EFI_ACPI_6_6_IO_APIC_STRUCTURE  *IoApic;

  IoApic = (EFI_ACPI_6_6_IO_APIC_STRUCTURE *)Entry;
  Print (
    L"%03u I/O APIC        id=%u addr=0x%08x gsi_base=%u\r\n",
    Index,
    IoApic->IoApicId,
    IoApic->IoApicAddress,
    IoApic->GlobalSystemInterruptBase
    );
}

STATIC
VOID
PrintInterruptSourceOverride (
  IN UINTN  Index,
  IN VOID   *Entry
  )
{
  EFI_ACPI_6_6_INTERRUPT_SOURCE_OVERRIDE_STRUCTURE  *Override;

  Override = (EFI_ACPI_6_6_INTERRUPT_SOURCE_OVERRIDE_STRUCTURE *)Entry;
  Print (
    L"%03u IRQ Override    bus=%u source=%u gsi=%u flags=0x%04x\r\n",
    Index,
    Override->Bus,
    Override->Source,
    Override->GlobalSystemInterrupt,
    Override->Flags
    );
}

STATIC
VOID
PrintLocalApicNmi (
  IN UINTN  Index,
  IN VOID   *Entry
  )
{
  EFI_ACPI_6_6_LOCAL_APIC_NMI_STRUCTURE  *Nmi;

  Nmi = (EFI_ACPI_6_6_LOCAL_APIC_NMI_STRUCTURE *)Entry;
  Print (
    L"%03u Local APIC NMI  uid=%u lint=%u flags=0x%04x\r\n",
    Index,
    Nmi->AcpiProcessorUid,
    Nmi->LocalApicLint,
    Nmi->Flags
    );
}

STATIC
VOID
PrintLocalX2Apic (
  IN UINTN  Index,
  IN VOID   *Entry
  )
{
  EFI_ACPI_6_6_PROCESSOR_LOCAL_X2APIC_STRUCTURE  *LocalX2Apic;

  LocalX2Apic = (EFI_ACPI_6_6_PROCESSOR_LOCAL_X2APIC_STRUCTURE *)Entry;
  Print (
    L"%03u Local x2APIC    uid=%u x2apic_id=%u flags=0x%08x\r\n",
    Index,
    LocalX2Apic->AcpiProcessorUid,
    LocalX2Apic->X2ApicId,
    LocalX2Apic->Flags
    );
}

VOID
PrintMadt (
  IN EFI_ACPI_DESCRIPTION_HEADER  *Madt
  )
{
  EFI_ACPI_6_6_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER  *Header;
  MADT_ENTRY_HEADER                                    *Entry;
  UINT8                                                *End;
  UINTN                                                Index;

  if (Madt == NULL) {
    Print(L"MADT/APIC table not found\r\n");
    return;
  }

  if (Madt->Signature != EFI_ACPI_6_6_MULTIPLE_APIC_DESCRIPTION_TABLE_SIGNATURE) {
    Print(L"MADT signature mismatch\r\n");
    return;
  }

  Header = (EFI_ACPI_6_6_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER *)Madt;
  Entry  = (MADT_ENTRY_HEADER *)((UINT8 *)Header + sizeof(*Header));
  End    = (UINT8 *)Header + Header->Header.Length;
  Index  = 0;

  Print(L"MADT/APIC: 0x%016lx length=%u revision=%u\r\n",
        (UINTN)Header,
        Header->Header.Length,
        Header->Header.Revision);
  Print(L"  LocalApicAddress: 0x%08x\r\n", Header->LocalApicAddress);
  Print(L"  Flags:            0x%08x\r\n", Header->Flags);
  Print(L"Idx Type            Details\r\n");

  // MADT entries are variable-length records. Always advance by Entry->Length,
  // not by sizeof(struct), so newer ACPI entry types can be skipped safely.
  while ((UINT8 *)Entry < End) {
    if (Entry->Length < sizeof(MADT_ENTRY_HEADER)) {
      Print(L"%03u Invalid MADT entry length=%u\r\n", Index, Entry->Length);
      return;
    }

    switch (Entry->Type) {
      case EFI_ACPI_6_6_PROCESSOR_LOCAL_APIC:
        PrintLocalApic(Index, Entry);
        break;
      case EFI_ACPI_6_6_IO_APIC:
        PrintIoApic(Index, Entry);
        break;
      case EFI_ACPI_6_6_INTERRUPT_SOURCE_OVERRIDE:
        PrintInterruptSourceOverride(Index, Entry);
        break;
      case EFI_ACPI_6_6_LOCAL_APIC_NMI:
        PrintLocalApicNmi(Index, Entry);
        break;
      case EFI_ACPI_6_6_PROCESSOR_LOCAL_X2APIC:
        PrintLocalX2Apic(Index, Entry);
        break;
      default:
        Print (
          L"%03u %-15s type=%u length=%u\r\n",
          Index,
          MadtEntryTypeName(Entry->Type),
          Entry->Type,
          Entry->Length
          );
        break;
    }

    Entry = (MADT_ENTRY_HEADER *)((UINT8 *)Entry + Entry->Length);
    Index++;
  }
}
