#include <Uefi.h>
#include <IndustryStandard/SmBios.h>
#include <Library/UefiLib.h>

#include "Smbios.h"

//
// SMBIOS basic inventory printer.
//
// Mental model category:
// - Inventory / identity.
// - Phase 1 UEFI firmware view.
//
// Where this sits in the platform flow:
// - UEFI ConfigurationTable points to the SMBIOS entry point.
// - The entry point describes where the SMBIOS structure table lives.
// - Linux userspace later exposes the same inventory class through dmidecode.
//
// Hardware / platform information printed here:
// - Type 0: BIOS vendor, version, release date.
// - Type 1: system manufacturer, product, version, serial.
// - Type 2: baseboard manufacturer, product, version, serial.
// - Type 3: chassis manufacturer, type, version, serial.
// - Type 4: processor socket, manufacturer, version, core/thread counts.
// - Type 17: DIMM locator, bank, size, manufacturer, serial, part number.
//
// Purpose:
// - Complete Phase 1's firmware inventory view.
// - Teach that SMBIOS is inventory/asset metadata, not ACPI topology control.
//
// What the kernel / OS loader can do with this information:
// - Kernel boot does not primarily depend on SMBIOS for device operation.
// - OS/userspace uses SMBIOS for inventory, asset tracking, support bundles,
//   and comparing firmware identity against expected server configuration.
//
// What RD / validation can debug from this:
// - Wrong BIOS version or board serial can indicate the wrong firmware image.
// - Missing or wrong DIMM locator data causes inventory and service issues.
// - Processor/socket strings help compare firmware inventory with Linux lscpu.
//

STATIC
CONST CHAR8 *
SmbiosString (
  IN CONST SMBIOS_STRUCTURE  *Header,
  IN CONST UINT8             *TableEnd,
  IN SMBIOS_TABLE_STRING     StringIndex
  )
{
  CONST UINT8  *String;
  UINTN        Index;

  if (StringIndex == 0) {
    return "";
  }

  // SMBIOS strings live after the formatted structure. String fields store a
  // 1-based index into this trailing string-set rather than a direct pointer.
  String = (CONST UINT8 *)Header + Header->Length;
  for (Index = 1; String < TableEnd && *String != '\0'; Index++) {
    if (Index == StringIndex) {
      return (CONST CHAR8 *)String;
    }

    while (String < TableEnd && *String != '\0') {
      String++;
    }

    if (String < TableEnd) {
      String++;
    }
  }

  if (String >= TableEnd) {
    return "<truncated>";
  }

  return "<missing>";
}

STATIC
CONST SMBIOS_STRUCTURE *
NextSmbiosStructure (
  IN CONST SMBIOS_STRUCTURE  *Header,
  IN CONST UINT8             *TableEnd
  )
{
  CONST UINT8  *Walker;

  Walker = (CONST UINT8 *)Header + Header->Length;

  // The formatted area is followed by zero or more strings and a double-NUL
  // terminator. Walk to that terminator to find the next structure.
  while ((Walker + 1) < TableEnd) {
    if ((Walker[0] == 0) && (Walker[1] == 0)) {
      return (CONST SMBIOS_STRUCTURE *)(Walker + 2);
    }

    Walker++;
  }

  return (CONST SMBIOS_STRUCTURE *)TableEnd;
}

STATIC
BOOLEAN
FieldPresent (
  IN CONST SMBIOS_STRUCTURE  *Header,
  IN UINTN                   FieldOffset,
  IN UINTN                   FieldSize
  )
{
  return Header->Length >= (FieldOffset + FieldSize);
}

STATIC
UINT16
MemoryDeviceSizeMb (
  IN CONST SMBIOS_TABLE_TYPE17  *Type17
  )
{
  if (Type17->Size == 0) {
    return 0;
  }

  if (Type17->Size == 0xFFFF) {
    return 0xFFFF;
  }

  if ((Type17->Size & 0x8000) != 0) {
    return (UINT16)((Type17->Size & 0x7FFF) / 1024);
  }

  return Type17->Size;
}

STATIC
VOID
PrintType0Bios (
  IN CONST SMBIOS_TABLE_TYPE0  *Type0,
  IN CONST UINT8               *TableEnd
  )
{
  Print(L"  BIOS: vendor=\"%a\" version=\"%a\" release=\"%a\"\r\n",
        SmbiosString(&Type0->Hdr, TableEnd, Type0->Vendor),
        SmbiosString(&Type0->Hdr, TableEnd, Type0->BiosVersion),
        SmbiosString(&Type0->Hdr, TableEnd, Type0->BiosReleaseDate));
}

STATIC
VOID
PrintType1System (
  IN CONST SMBIOS_TABLE_TYPE1  *Type1,
  IN CONST UINT8               *TableEnd
  )
{
  Print(L"  System: manufacturer=\"%a\" product=\"%a\" version=\"%a\" serial=\"%a\"\r\n",
        SmbiosString(&Type1->Hdr, TableEnd, Type1->Manufacturer),
        SmbiosString(&Type1->Hdr, TableEnd, Type1->ProductName),
        SmbiosString(&Type1->Hdr, TableEnd, Type1->Version),
        SmbiosString(&Type1->Hdr, TableEnd, Type1->SerialNumber));
}

STATIC
VOID
PrintType2Baseboard (
  IN CONST SMBIOS_TABLE_TYPE2  *Type2,
  IN CONST UINT8               *TableEnd
  )
{
  Print(L"  Baseboard: manufacturer=\"%a\" product=\"%a\" version=\"%a\" serial=\"%a\"\r\n",
        SmbiosString(&Type2->Hdr, TableEnd, Type2->Manufacturer),
        SmbiosString(&Type2->Hdr, TableEnd, Type2->ProductName),
        SmbiosString(&Type2->Hdr, TableEnd, Type2->Version),
        SmbiosString(&Type2->Hdr, TableEnd, Type2->SerialNumber));
}

STATIC
VOID
PrintType3Chassis (
  IN CONST SMBIOS_TABLE_TYPE3  *Type3,
  IN CONST UINT8               *TableEnd
  )
{
  Print(L"  Chassis: manufacturer=\"%a\" type=0x%02x version=\"%a\" serial=\"%a\"\r\n",
        SmbiosString(&Type3->Hdr, TableEnd, Type3->Manufacturer),
        Type3->Type,
        SmbiosString(&Type3->Hdr, TableEnd, Type3->Version),
        SmbiosString(&Type3->Hdr, TableEnd, Type3->SerialNumber));
}

STATIC
VOID
PrintType4Processor (
  IN CONST SMBIOS_TABLE_TYPE4  *Type4,
  IN CONST UINT8               *TableEnd
  )
{
  UINT16  CoreCount;
  UINT16  EnabledCoreCount;
  UINT16  ThreadCount;

  CoreCount        = 0;
  EnabledCoreCount = 0;
  ThreadCount      = 0;

  // Newer SMBIOS versions extend 8-bit core/thread counts with 16-bit fields.
  // Check the structure length before touching version-dependent fields.
  if (FieldPresent(&Type4->Hdr, OFFSET_OF(SMBIOS_TABLE_TYPE4, CoreCount), sizeof(Type4->CoreCount))) {
    CoreCount        = Type4->CoreCount;
    EnabledCoreCount = Type4->EnabledCoreCount;
    ThreadCount      = Type4->ThreadCount;
  }

  if ((CoreCount == 0xFF) &&
      FieldPresent(&Type4->Hdr, OFFSET_OF(SMBIOS_TABLE_TYPE4, CoreCount2), sizeof(Type4->CoreCount2))) {
    CoreCount        = Type4->CoreCount2;
    EnabledCoreCount = Type4->EnabledCoreCount2;
    ThreadCount      = Type4->ThreadCount2;
  }

  Print(L"  Processor: socket=\"%a\" manufacturer=\"%a\" version=\"%a\" cores=%u enabled=%u threads=%u current_mhz=%u\r\n",
        SmbiosString(&Type4->Hdr, TableEnd, Type4->Socket),
        SmbiosString(&Type4->Hdr, TableEnd, Type4->ProcessorManufacturer),
        SmbiosString(&Type4->Hdr, TableEnd, Type4->ProcessorVersion),
        CoreCount,
        EnabledCoreCount,
        ThreadCount,
        Type4->CurrentSpeed);
}

STATIC
VOID
PrintType17MemoryDevice (
  IN CONST SMBIOS_TABLE_TYPE17  *Type17,
  IN CONST UINT8                *TableEnd
  )
{
  UINT16  SizeMb;

  SizeMb = MemoryDeviceSizeMb(Type17);
  if (SizeMb == 0) {
    return;
  }

  Print(L"  MemoryDevice: locator=\"%a\" bank=\"%a\" size_mb=%u speed_mhz=%u manufacturer=\"%a\" serial=\"%a\" part=\"%a\"\r\n",
        SmbiosString(&Type17->Hdr, TableEnd, Type17->DeviceLocator),
        SmbiosString(&Type17->Hdr, TableEnd, Type17->BankLocator),
        SizeMb,
        Type17->Speed,
        SmbiosString(&Type17->Hdr, TableEnd, Type17->Manufacturer),
        SmbiosString(&Type17->Hdr, TableEnd, Type17->SerialNumber),
        SmbiosString(&Type17->Hdr, TableEnd, Type17->PartNumber));
}

STATIC
VOID
PrintSmbiosTable (
  IN EFI_PHYSICAL_ADDRESS  TableAddress,
  IN UINTN                 TableLength,
  IN UINTN                 StructureCount
  )
{
  CONST SMBIOS_STRUCTURE  *Header;
  CONST UINT8             *TableEnd;
  UINTN                   Index;

  Header   = (CONST SMBIOS_STRUCTURE *)(UINTN)TableAddress;
  TableEnd = (CONST UINT8 *)(UINTN)TableAddress + TableLength;

  Print(L"SMBIOS structures:\r\n");

  // Walk the raw SMBIOS structure table. For SMBIOS 2.x we also have a count;
  // for SMBIOS 3.x the entry point gives a maximum table size, so type 127 or
  // the table end stops the walk.
  for (Index = 0;
       ((CONST UINT8 *)Header + sizeof(SMBIOS_STRUCTURE)) <= TableEnd;
       Index++) {
    if ((StructureCount != 0) && (Index >= StructureCount)) {
      break;
    }

    if ((Header->Type == SMBIOS_TYPE_END_OF_TABLE) || (Header->Length < sizeof(SMBIOS_STRUCTURE))) {
      break;
    }

    switch (Header->Type) {
      case SMBIOS_TYPE_BIOS_INFORMATION:
        if (Header->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE0, BiosReleaseDate) + sizeof(((SMBIOS_TABLE_TYPE0 *)0)->BiosReleaseDate)) {
          PrintType0Bios((CONST SMBIOS_TABLE_TYPE0 *)Header, TableEnd);
        }
        break;
      case SMBIOS_TYPE_SYSTEM_INFORMATION:
        if (Header->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE1, SerialNumber) + sizeof(((SMBIOS_TABLE_TYPE1 *)0)->SerialNumber)) {
          PrintType1System((CONST SMBIOS_TABLE_TYPE1 *)Header, TableEnd);
        }
        break;
      case SMBIOS_TYPE_BASEBOARD_INFORMATION:
        if (Header->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE2, SerialNumber) + sizeof(((SMBIOS_TABLE_TYPE2 *)0)->SerialNumber)) {
          PrintType2Baseboard((CONST SMBIOS_TABLE_TYPE2 *)Header, TableEnd);
        }
        break;
      case SMBIOS_TYPE_SYSTEM_ENCLOSURE:
        if (Header->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE3, SerialNumber) + sizeof(((SMBIOS_TABLE_TYPE3 *)0)->SerialNumber)) {
          PrintType3Chassis((CONST SMBIOS_TABLE_TYPE3 *)Header, TableEnd);
        }
        break;
      case SMBIOS_TYPE_PROCESSOR_INFORMATION:
        if (Header->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE4, CurrentSpeed) + sizeof(((SMBIOS_TABLE_TYPE4 *)0)->CurrentSpeed)) {
          PrintType4Processor((CONST SMBIOS_TABLE_TYPE4 *)Header, TableEnd);
        }
        break;
      case SMBIOS_TYPE_MEMORY_DEVICE:
        if (Header->Length >= OFFSET_OF(SMBIOS_TABLE_TYPE17, PartNumber) + sizeof(((SMBIOS_TABLE_TYPE17 *)0)->PartNumber)) {
          PrintType17MemoryDevice((CONST SMBIOS_TABLE_TYPE17 *)Header, TableEnd);
        }
        break;
      default:
        break;
    }

    Header = NextSmbiosStructure(Header, TableEnd);
  }
}

VOID
PrintSmbiosBasicInfo (
  IN SMBIOS_TABLE_ENTRY_POINT      *SmbiosEntryPoint32,
  IN SMBIOS_TABLE_3_0_ENTRY_POINT  *SmbiosEntryPoint64
  )
{
  SMBIOS_TABLE_ENTRY_POINT      *Entry32;
  SMBIOS_TABLE_3_0_ENTRY_POINT  *Entry64;

  Print(L"SMBIOS basic system info:\r\n");

  if (SmbiosEntryPoint64 != NULL) {
    Entry64 = SmbiosEntryPoint64;
    Print(L"  Entry 64: version=%u.%u docrev=%u table=0x%016lx max_size=%u\r\n",
          Entry64->MajorVersion,
          Entry64->MinorVersion,
          Entry64->DocRev,
          Entry64->TableAddress,
          Entry64->TableMaximumSize);
    PrintSmbiosTable(Entry64->TableAddress, Entry64->TableMaximumSize, 0);
    return;
  }

  if (SmbiosEntryPoint32 != NULL) {
    Entry32 = SmbiosEntryPoint32;
    Print(L"  Entry 32: version=%u.%u table=0x%08x length=%u structures=%u\r\n",
          Entry32->MajorVersion,
          Entry32->MinorVersion,
          Entry32->TableAddress,
          Entry32->TableLength,
          Entry32->NumberOfSmbiosStructures);
    PrintSmbiosTable(
      Entry32->TableAddress,
      Entry32->TableLength,
      Entry32->NumberOfSmbiosStructures
      );
    return;
  }

  Print(L"  No SMBIOS entry point found.\r\n");
}
