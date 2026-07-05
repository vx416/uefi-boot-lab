#include <Uefi.h>
#include <Guid/UefiBootLabPlatformTable.h>
#include <IndustryStandard/Acpi.h>
#include <IndustryStandard/MemoryMappedConfigurationSpaceAccessTable.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

#include "FirmwareJson.h"
#include "../Firmware/ConfigTables.h"

//
// Firmware JSON report writer.
//
// Mental model category:
// - Phase 1 UEFI firmware view.
// - Report output layer.
//
// Where this sits in the platform flow:
// - FirmwareView.efi is loaded from a FAT/EFI volume.
// - EFI_LOADED_IMAGE_PROTOCOL tells us which device handle loaded this image.
// - EFI_SIMPLE_FILE_SYSTEM_PROTOCOL opens that FAT volume.
// - EFI_FILE_PROTOCOL writes EFI\UEFI-BOOT-LAB\firmware-view.json.
//
// Hardware / platform information written here:
// - firmware vendor and UEFI revision.
// - ACPI RSDP pointer and XSDT/RSDT table signatures.
// - SMBIOS entry pointers.
// - UEFI memory map summary from the captured descriptor buffer.
//
// Purpose:
// - Move Phase 1 beyond serial-only Print() output.
// - Produce a machine-readable firmware view that Phase 2/3 Python tooling can
//   compare with Linux /proc/iomem, dmidecode, lspci, lscpu, and numactl.
//
// What RD / validation can debug from this:
// - If firmware-view.json is missing, debug loaded image handle, filesystem
//   protocol, FAT write permissions, or boot media mapping.
// - If JSON content differs from Linux view, compare firmware handoff vs Linux
//   kernel interpretation rather than relying only on serial logs.
//

#define FIRMWARE_JSON_CAPACITY  32768

typedef struct {
  CHAR8    *Buffer;
  UINTN    Capacity;
  UINTN    Length;
  BOOLEAN  Truncated;
} JSON_WRITER;

STATIC
VOID
JsonAppend (
  IN OUT JSON_WRITER  *Writer,
  IN CONST CHAR8      *Text
  )
{
  UINTN  TextLength;
  UINTN  CopyLength;

  if (Writer->Length >= Writer->Capacity) {
    Writer->Truncated = TRUE;
    return;
  }

  TextLength  = AsciiStrLen(Text);
  CopyLength  = TextLength;
  if ((Writer->Length + CopyLength + 1) > Writer->Capacity) {
    CopyLength        = Writer->Capacity - Writer->Length - 1;
    Writer->Truncated = TRUE;
  }

  CopyMem(Writer->Buffer + Writer->Length, Text, CopyLength);
  Writer->Length += CopyLength;
  Writer->Buffer[Writer->Length] = '\0';
}

STATIC
VOID
JsonAppendEscapedChar16 (
  IN OUT JSON_WRITER  *Writer,
  IN CONST CHAR16     *Text
  )
{
  CHAR8  Temp[8];

  JsonAppend(Writer, "\"");
  if (Text != NULL) {
    while (*Text != L'\0') {
      if (*Text == L'\"') {
        JsonAppend(Writer, "\\\"");
      } else if (*Text == L'\\') {
        JsonAppend(Writer, "\\\\");
      } else if ((*Text >= 0x20) && (*Text <= 0x7E)) {
        Temp[0] = (CHAR8)*Text;
        Temp[1] = '\0';
        JsonAppend(Writer, Temp);
      } else {
        JsonAppend(Writer, "?");
      }

      Text++;
    }
  }

  JsonAppend(Writer, "\"");
}

STATIC
VOID
JsonAppendEscapedAsciiFixed (
  IN OUT JSON_WRITER  *Writer,
  IN CONST CHAR8      *Text,
  IN UINTN            MaxBytes
  )
{
  CHAR8  Temp[2];
  UINTN  Index;

  JsonAppend(Writer, "\"");
  if (Text != NULL) {
    for (Index = 0; Index < MaxBytes && Text[Index] != '\0'; Index++) {
      if (Text[Index] == '"') {
        JsonAppend(Writer, "\\\"");
      } else if (Text[Index] == '\\') {
        JsonAppend(Writer, "\\\\");
      } else if (((UINT8)Text[Index] >= 0x20) && ((UINT8)Text[Index] <= 0x7E)) {
        Temp[0] = Text[Index];
        Temp[1] = '\0';
        JsonAppend(Writer, Temp);
      } else {
        JsonAppend(Writer, "?");
      }
    }
  }

  JsonAppend(Writer, "\"");
}

STATIC
VOID
JsonAppendUint64 (
  IN OUT JSON_WRITER  *Writer,
  IN UINT64           Value
  )
{
  CHAR8  Digits[32];
  UINTN  Index;

  Index = sizeof(Digits);
  Digits[--Index] = '\0';

  if (Value == 0) {
    Digits[--Index] = '0';
  } else {
    while ((Value != 0) && (Index > 0)) {
      Digits[--Index] = (CHAR8)('0' + (Value % 10));
      Value /= 10;
    }
  }

  JsonAppend(Writer, &Digits[Index]);
}

STATIC
VOID
JsonAppendHex (
  IN OUT JSON_WRITER  *Writer,
  IN UINT64           Value,
  IN UINTN            Nibbles
  )
{
  STATIC CONST CHAR8  Hex[] = "0123456789ABCDEF";
  CHAR8               Text[19];
  UINTN               Index;

  if (Nibbles > 16) {
    Nibbles = 16;
  }

  Text[0] = '0';
  Text[1] = 'x';
  for (Index = 0; Index < Nibbles; Index++) {
    Text[2 + Index] = Hex[(Value >> ((Nibbles - Index - 1) * 4)) & 0xF];
  }

  Text[2 + Nibbles] = '\0';
  JsonAppend(Writer, Text);
}

STATIC
VOID
JsonAppendQuotedPointer (
  IN OUT JSON_WRITER  *Writer,
  IN UINTN            Value
  )
{
  JsonAppend(Writer, "\"");
  JsonAppendHex(Writer, Value, 16);
  JsonAppend(Writer, "\"");
}

STATIC
VOID
JsonAppendAcpiSignature (
  IN OUT JSON_WRITER  *Writer,
  IN UINT32           Signature
  )
{
  CHAR8  Text[7];

  Text[0] = '"';
  Text[1] = (CHAR8)(Signature & 0xFF);
  Text[2] = (CHAR8)((Signature >> 8) & 0xFF);
  Text[3] = (CHAR8)((Signature >> 16) & 0xFF);
  Text[4] = (CHAR8)((Signature >> 24) & 0xFF);
  Text[5] = '"';
  Text[6] = '\0';
  JsonAppend(Writer, Text);
}

STATIC
VOID
AddPagesByCategory (
  IN EFI_MEMORY_DESCRIPTOR  *Descriptor,
  IN OUT UINT64             *UsablePages,
  IN OUT UINT64             *BootServicesPages,
  IN OUT UINT64             *RuntimePages,
  IN OUT UINT64             *AcpiPages,
  IN OUT UINT64             *ReservedPages,
  IN OUT UINT64             *MmioPages
  )
{
  switch (Descriptor->Type) {
    case EfiConventionalMemory:
      *UsablePages += Descriptor->NumberOfPages;
      break;
    case EfiBootServicesCode:
    case EfiBootServicesData:
      *BootServicesPages += Descriptor->NumberOfPages;
      break;
    case EfiRuntimeServicesCode:
    case EfiRuntimeServicesData:
      *RuntimePages += Descriptor->NumberOfPages;
      break;
    case EfiACPIReclaimMemory:
    case EfiACPIMemoryNVS:
      *AcpiPages += Descriptor->NumberOfPages;
      break;
    case EfiMemoryMappedIO:
    case EfiMemoryMappedIOPortSpace:
      *MmioPages += Descriptor->NumberOfPages;
      break;
    default:
      *ReservedPages += Descriptor->NumberOfPages;
      break;
  }
}

STATIC
VOID
JsonAppendAcpiTables (
  IN OUT JSON_WRITER                             *Writer,
  IN EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *AcpiRsdp
  )
{
  EFI_ACPI_DESCRIPTION_HEADER                   *Xsdt;
  EFI_ACPI_DESCRIPTION_HEADER                   *Rsdt;
  EFI_ACPI_DESCRIPTION_HEADER                   *Header;
  UINT64                                        *XsdtEntry;
  UINT32                                        *RsdtEntry;
  UINTN                                         EntryCount;
  UINTN                                         Index;

  JsonAppend(Writer, "    \"tables\": [");
  if (AcpiRsdp == NULL) {
    JsonAppend(Writer, "]");
    return;
  }

  if ((AcpiRsdp->Revision >= 2) && (AcpiRsdp->XsdtAddress != 0)) {
    Xsdt       = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)AcpiRsdp->XsdtAddress;
    XsdtEntry  = (UINT64 *)((UINT8 *)Xsdt + sizeof(EFI_ACPI_DESCRIPTION_HEADER));
    EntryCount = (Xsdt->Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) / sizeof(UINT64);

    for (Index = 0; Index < EntryCount; Index++) {
      Header = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)XsdtEntry[Index];
      if (Index != 0) {
        JsonAppend(Writer, ", ");
      }

      JsonAppendAcpiSignature(Writer, Header->Signature);
    }
  } else if (AcpiRsdp->RsdtAddress != 0) {
    Rsdt       = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)AcpiRsdp->RsdtAddress;
    RsdtEntry  = (UINT32 *)((UINT8 *)Rsdt + sizeof(EFI_ACPI_DESCRIPTION_HEADER));
    EntryCount = (Rsdt->Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) / sizeof(UINT32);

    for (Index = 0; Index < EntryCount; Index++) {
      Header = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)RsdtEntry[Index];
      if (Index != 0) {
        JsonAppend(Writer, ", ");
      }

      JsonAppendAcpiSignature(Writer, Header->Signature);
    }
  }

  JsonAppend(Writer, "]");
}

STATIC
EFI_ACPI_DESCRIPTION_HEADER *
FindAcpiTableForJson (
  IN EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *AcpiRsdp,
  IN UINT32                                         Signature
  )
{
  EFI_ACPI_DESCRIPTION_HEADER                   *Root;
  EFI_ACPI_DESCRIPTION_HEADER                   *Header;
  UINT64                                        *XsdtEntry;
  UINT32                                        *RsdtEntry;
  UINTN                                         EntryCount;
  UINTN                                         Index;

  if (AcpiRsdp == NULL) {
    return NULL;
  }

  if ((AcpiRsdp->Revision >= 2) && (AcpiRsdp->XsdtAddress != 0)) {
    Root = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)AcpiRsdp->XsdtAddress;
    if ((Root->Length < sizeof(EFI_ACPI_DESCRIPTION_HEADER)) ||
        (Root->Signature != EFI_ACPI_2_0_EXTENDED_SYSTEM_DESCRIPTION_TABLE_SIGNATURE)) {
      return NULL;
    }

    XsdtEntry  = (UINT64 *)((UINT8 *)Root + sizeof(EFI_ACPI_DESCRIPTION_HEADER));
    EntryCount = (Root->Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) / sizeof(UINT64);
    for (Index = 0; Index < EntryCount; Index++) {
      if (XsdtEntry[Index] == 0) {
        continue;
      }

      Header = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)XsdtEntry[Index];
      if ((Header->Length >= sizeof(EFI_ACPI_DESCRIPTION_HEADER)) &&
          (Header->Signature == Signature)) {
        return Header;
      }
    }
  }

  if (AcpiRsdp->RsdtAddress == 0) {
    return NULL;
  }

  Root = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)AcpiRsdp->RsdtAddress;
  if ((Root->Length < sizeof(EFI_ACPI_DESCRIPTION_HEADER)) ||
      (Root->Signature != EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_TABLE_SIGNATURE)) {
    return NULL;
  }

  RsdtEntry  = (UINT32 *)((UINT8 *)Root + sizeof(EFI_ACPI_DESCRIPTION_HEADER));
  EntryCount = (Root->Length - sizeof(EFI_ACPI_DESCRIPTION_HEADER)) / sizeof(UINT32);
  for (Index = 0; Index < EntryCount; Index++) {
    if (RsdtEntry[Index] == 0) {
      continue;
    }

    Header = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)RsdtEntry[Index];
    if ((Header->Length >= sizeof(EFI_ACPI_DESCRIPTION_HEADER)) &&
        (Header->Signature == Signature)) {
      return Header;
    }
  }

  return NULL;
}

STATIC
VOID
JsonAppendMadtSummary (
  IN OUT JSON_WRITER                             *Writer,
  IN EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *AcpiRsdp
  )
{
  EFI_ACPI_6_6_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER  *Madt;

  Madt = (EFI_ACPI_6_6_MULTIPLE_APIC_DESCRIPTION_TABLE_HEADER *)FindAcpiTableForJson(
                                                               AcpiRsdp,
                                                               EFI_ACPI_6_6_MULTIPLE_APIC_DESCRIPTION_TABLE_SIGNATURE
                                                               );

  JsonAppend(Writer, "    \"madt\": {\n");
  JsonAppend(Writer, "      \"present\": ");
  JsonAppend(Writer, (Madt != NULL) ? "true" : "false");
  if (Madt != NULL) {
    JsonAppend(Writer, ",\n");
    JsonAppend(Writer, "      \"length\": ");
    JsonAppendUint64(Writer, Madt->Header.Length);
    JsonAppend(Writer, ",\n");
    JsonAppend(Writer, "      \"localApicAddress\": \"");
    JsonAppendHex(Writer, Madt->LocalApicAddress, 8);
    JsonAppend(Writer, "\",\n");
    JsonAppend(Writer, "      \"flags\": \"");
    JsonAppendHex(Writer, Madt->Flags, 8);
    JsonAppend(Writer, "\"\n");
  } else {
    JsonAppend(Writer, "\n");
  }

  JsonAppend(Writer, "    }");
}

STATIC
VOID
JsonAppendMcfgSummary (
  IN OUT JSON_WRITER                             *Writer,
  IN EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *AcpiRsdp
  )
{
  EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE_HEADER  *Mcfg;
  UINTN                                                           EntryCount;

  Mcfg = (EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE_HEADER *)FindAcpiTableForJson(
                                                                       AcpiRsdp,
                                                                       EFI_ACPI_6_6_PCI_EXPRESS_MEMORY_MAPPED_CONFIGURATION_SPACE_BASE_ADDRESS_DESCRIPTION_TABLE_SIGNATURE
                                                                       );
  EntryCount = 0;
  if ((Mcfg != NULL) && (Mcfg->Header.Length >= sizeof(*Mcfg))) {
    EntryCount = (Mcfg->Header.Length - sizeof(*Mcfg)) /
                 sizeof(EFI_ACPI_MEMORY_MAPPED_ENHANCED_CONFIGURATION_SPACE_BASE_ADDRESS_ALLOCATION_STRUCTURE);
  }

  JsonAppend(Writer, "    \"mcfg\": {\n");
  JsonAppend(Writer, "      \"present\": ");
  JsonAppend(Writer, (Mcfg != NULL) ? "true" : "false");
  JsonAppend(Writer, ",\n");
  JsonAppend(Writer, "      \"entryCount\": ");
  JsonAppendUint64(Writer, EntryCount);
  JsonAppend(Writer, "\n");
  JsonAppend(Writer, "    }");
}

STATIC
VOID
JsonAppendPciSummary (
  IN OUT JSON_WRITER                             *Writer,
  IN EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *AcpiRsdp
  )
{
  EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE_HEADER                         *Mcfg;
  EFI_ACPI_MEMORY_MAPPED_ENHANCED_CONFIGURATION_SPACE_BASE_ADDRESS_ALLOCATION_STRUCTURE  *Entry;
  UINTN                                                                                  EntryCount;
  UINTN                                                                                  Index;

  Mcfg = (EFI_ACPI_MEMORY_MAPPED_CONFIGURATION_BASE_ADDRESS_TABLE_HEADER *)FindAcpiTableForJson(
                                                                       AcpiRsdp,
                                                                       EFI_ACPI_6_6_PCI_EXPRESS_MEMORY_MAPPED_CONFIGURATION_SPACE_BASE_ADDRESS_DESCRIPTION_TABLE_SIGNATURE
                                                                       );
  EntryCount = 0;
  Entry      = NULL;
  if ((Mcfg != NULL) && (Mcfg->Header.Length >= sizeof(*Mcfg))) {
    Entry      = (EFI_ACPI_MEMORY_MAPPED_ENHANCED_CONFIGURATION_SPACE_BASE_ADDRESS_ALLOCATION_STRUCTURE *)((UINT8 *)Mcfg + sizeof(*Mcfg));
    EntryCount = (Mcfg->Header.Length - sizeof(*Mcfg)) / sizeof(*Entry);
  }

  JsonAppend(Writer, "  \"pci\": {\n");
  JsonAppend(Writer, "    \"ecamWindows\": [");
  for (Index = 0; Index < EntryCount; Index++) {
    if (Index != 0) {
      JsonAppend(Writer, ",");
    }

    JsonAppend(Writer, "\n");
    JsonAppend(Writer, "      {\n");
    JsonAppend(Writer, "        \"base\": \"");
    JsonAppendHex(Writer, Entry[Index].BaseAddress, 16);
    JsonAppend(Writer, "\",\n");
    JsonAppend(Writer, "        \"segment\": ");
    JsonAppendUint64(Writer, Entry[Index].PciSegmentGroupNumber);
    JsonAppend(Writer, ",\n");
    JsonAppend(Writer, "        \"startBus\": ");
    JsonAppendUint64(Writer, Entry[Index].StartBusNumber);
    JsonAppend(Writer, ",\n");
    JsonAppend(Writer, "        \"endBus\": ");
    JsonAppendUint64(Writer, Entry[Index].EndBusNumber);
    JsonAppend(Writer, "\n");
    JsonAppend(Writer, "      }");
  }

  if (EntryCount != 0) {
    JsonAppend(Writer, "\n");
  }

  JsonAppend(Writer, "    ]\n");
  JsonAppend(Writer, "  }");
}

STATIC
VOID
JsonAppendUefiBootLabTable (
  IN OUT JSON_WRITER   *Writer,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UEFI_BOOT_LAB_PLATFORM_TABLE  *LabTable;
  BOOLEAN                       Valid;

  LabTable = (UEFI_BOOT_LAB_PLATFORM_TABLE *)FindConfigurationTable(
                                             SystemTable,
                                             &gUefiBootLabPlatformTableGuid
                                             );
  Valid = (LabTable != NULL) &&
          (LabTable->Magic == UEFI_BOOT_LAB_PLATFORM_TABLE_MAGIC) &&
          (LabTable->Size >= sizeof(UEFI_BOOT_LAB_PLATFORM_TABLE));

  JsonAppend(Writer, "  \"uefiBootLab\": {\n");
  JsonAppend(Writer, "    \"present\": ");
  JsonAppend(Writer, Valid ? "true" : "false");
  if (Valid) {
    JsonAppend(Writer, ",\n");
    JsonAppend(Writer, "    \"version\": ");
    JsonAppendUint64(Writer, LabTable->Version);
    JsonAppend(Writer, ",\n");
    JsonAppend(Writer, "    \"platformId\": ");
    JsonAppendEscapedAsciiFixed(
      Writer,
      LabTable->PlatformId,
      sizeof(LabTable->PlatformId)
      );
    JsonAppend(Writer, ",\n");
    JsonAppend(Writer, "    \"buildMarker\": ");
    JsonAppendEscapedAsciiFixed(
      Writer,
      LabTable->BuildMarker,
      sizeof(LabTable->BuildMarker)
      );
    JsonAppend(Writer, "\n");
  } else {
    JsonAppend(Writer, "\n");
  }

  JsonAppend(Writer, "  }");
}

STATIC
VOID
JsonAppendMemorySummary (
  IN OUT JSON_WRITER                     *Writer,
  IN CONST UEFI_BOOT_LAB_BOOT_INFO       *BootInfo
  )
{
  EFI_MEMORY_DESCRIPTOR  *Descriptor;
  UINTN                  DescriptorCount;
  UINTN                  Index;
  UINT64                 UsablePages;
  UINT64                 BootServicesPages;
  UINT64                 RuntimePages;
  UINT64                 AcpiPages;
  UINT64                 ReservedPages;
  UINT64                 MmioPages;

  UsablePages       = 0;
  BootServicesPages = 0;
  RuntimePages      = 0;
  AcpiPages         = 0;
  ReservedPages     = 0;
  MmioPages         = 0;

  DescriptorCount = 0;
  if ((BootInfo->MemoryMap != NULL) && (BootInfo->MemoryMapDescriptorSize != 0)) {
    DescriptorCount = BootInfo->MemoryMapSize / BootInfo->MemoryMapDescriptorSize;
    Descriptor = BootInfo->MemoryMap;
    for (Index = 0; Index < DescriptorCount; Index++) {
      AddPagesByCategory(
        Descriptor,
        &UsablePages,
        &BootServicesPages,
        &RuntimePages,
        &AcpiPages,
        &ReservedPages,
        &MmioPages
        );
      Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Descriptor + BootInfo->MemoryMapDescriptorSize);
    }
  }

  JsonAppend(Writer, "    \"descriptorCount\": ");
  JsonAppendUint64(Writer, DescriptorCount);
  JsonAppend(Writer, ",\n");
  JsonAppend(Writer, "    \"descriptorSize\": ");
  JsonAppendUint64(Writer, BootInfo->MemoryMapDescriptorSize);
  JsonAppend(Writer, ",\n");
  JsonAppend(Writer, "    \"usablePages\": ");
  JsonAppendUint64(Writer, UsablePages);
  JsonAppend(Writer, ",\n");
  JsonAppend(Writer, "    \"bootServicesPages\": ");
  JsonAppendUint64(Writer, BootServicesPages);
  JsonAppend(Writer, ",\n");
  JsonAppend(Writer, "    \"runtimePages\": ");
  JsonAppendUint64(Writer, RuntimePages);
  JsonAppend(Writer, ",\n");
  JsonAppend(Writer, "    \"acpiPages\": ");
  JsonAppendUint64(Writer, AcpiPages);
  JsonAppend(Writer, ",\n");
  JsonAppend(Writer, "    \"reservedPages\": ");
  JsonAppendUint64(Writer, ReservedPages);
  JsonAppend(Writer, ",\n");
  JsonAppend(Writer, "    \"mmioPages\": ");
  JsonAppendUint64(Writer, MmioPages);
  JsonAppend(Writer, "\n");
}

STATIC
VOID
BuildFirmwareJson (
  IN OUT JSON_WRITER                                 *Writer,
  IN EFI_SYSTEM_TABLE                                *SystemTable,
  IN EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER    *AcpiRsdp,
  IN CONST UEFI_BOOT_LAB_BOOT_INFO                   *BootInfo
  )
{
  JsonAppend(Writer, "{\n");
  JsonAppend(Writer, "  \"schemaVersion\": 1,\n");
  JsonAppend(Writer, "  \"firmware\": {\n");
  JsonAppend(Writer, "    \"vendor\": ");
  JsonAppendEscapedChar16(Writer, SystemTable->FirmwareVendor);
  JsonAppend(Writer, ",\n");
  JsonAppend(Writer, "    \"uefiRevision\": \"");
  JsonAppendUint64(Writer, SystemTable->Hdr.Revision >> 16);
  JsonAppend(Writer, ".");
  JsonAppendUint64(Writer, SystemTable->Hdr.Revision & 0xFFFF);
  JsonAppend(Writer, "\"\n");
  JsonAppend(Writer, "  },\n");

  JsonAppend(Writer, "  \"acpi\": {\n");
  JsonAppend(Writer, "    \"rsdp\": ");
  JsonAppendQuotedPointer(Writer, (UINTN)AcpiRsdp);
  JsonAppend(Writer, ",\n");
  if (AcpiRsdp != NULL) {
    JsonAppend(Writer, "    \"revision\": ");
    JsonAppendUint64(Writer, AcpiRsdp->Revision);
    JsonAppend(Writer, ",\n");
    JsonAppend(Writer, "    \"rsdt\": \"");
    JsonAppendHex(Writer, AcpiRsdp->RsdtAddress, 8);
    JsonAppend(Writer, "\",\n");
    JsonAppend(Writer, "    \"xsdt\": \"");
    JsonAppendHex(Writer, AcpiRsdp->XsdtAddress, 16);
    JsonAppend(Writer, "\",\n");
  }

  JsonAppendAcpiTables(Writer, AcpiRsdp);
  JsonAppend(Writer, ",\n");
  JsonAppendMadtSummary(Writer, AcpiRsdp);
  JsonAppend(Writer, ",\n");
  JsonAppendMcfgSummary(Writer, AcpiRsdp);
  JsonAppend(Writer, "\n");
  JsonAppend(Writer, "  },\n");

  JsonAppendUefiBootLabTable(Writer, SystemTable);
  JsonAppend(Writer, ",\n");

  JsonAppendPciSummary(Writer, AcpiRsdp);
  JsonAppend(Writer, ",\n");

  JsonAppend(Writer, "  \"smbios\": {\n");
  JsonAppend(Writer, "    \"entryPoint32\": ");
  JsonAppendQuotedPointer(Writer, (UINTN)BootInfo->SmbiosEntryPoint32);
  JsonAppend(Writer, ",\n");
  JsonAppend(Writer, "    \"entryPoint64\": ");
  JsonAppendQuotedPointer(Writer, (UINTN)BootInfo->SmbiosEntryPoint64);
  JsonAppend(Writer, "\n");
  JsonAppend(Writer, "  },\n");

  JsonAppend(Writer, "  \"memoryMap\": {\n");
  JsonAppendMemorySummary(Writer, BootInfo);
  JsonAppend(Writer, "  }\n");
  JsonAppend(Writer, "}\n");
}

STATIC
EFI_STATUS
OpenReportDirectory (
  IN EFI_FILE_PROTOCOL   *Root,
  OUT EFI_FILE_PROTOCOL  **ReportDir
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *EfiDir;

  EfiDir = NULL;
  Status = Root->Open(
                   Root,
                   &EfiDir,
                   L"EFI",
                   EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                   EFI_FILE_DIRECTORY
                   );
  if (EFI_ERROR(Status)) {
    return Status;
  }

  Status = EfiDir->Open(
                     EfiDir,
                     ReportDir,
                     L"UEFI-BOOT-LAB",
                     EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                     EFI_FILE_DIRECTORY
                     );
  EfiDir->Close(EfiDir);
  return Status;
}

STATIC
EFI_STATUS
WriteJsonToLoadedImageVolume (
  IN EFI_HANDLE    ImageHandle,
  IN CHAR8         *Json,
  IN UINTN         JsonLength
  )
{
  EFI_STATUS                       Status;
  EFI_LOADED_IMAGE_PROTOCOL        *LoadedImage;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;
  EFI_FILE_PROTOCOL                *Root;
  EFI_FILE_PROTOCOL                *ReportDir;
  EFI_FILE_PROTOCOL                *ExistingFile;
  EFI_FILE_PROTOCOL                *File;
  UINTN                            WriteSize;

  LoadedImage = NULL;
  FileSystem  = NULL;
  Root        = NULL;
  ReportDir   = NULL;
  ExistingFile = NULL;
  File        = NULL;

  Status = gBS->HandleProtocol(
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR(Status)) {
    return Status;
  }

  Status = gBS->HandleProtocol(
                  LoadedImage->DeviceHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&FileSystem
                  );
  if (EFI_ERROR(Status)) {
    return Status;
  }

  Status = FileSystem->OpenVolume(FileSystem, &Root);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  Status = OpenReportDirectory(Root, &ReportDir);
  Root->Close(Root);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  // Delete any previous report first. EFI_FILE_PROTOCOL does not guarantee
  // truncation when an existing file is opened with CREATE, and stale JSON
  // bytes at the end would make the report invalid.
  Status = ReportDir->Open(
                        ReportDir,
                        &ExistingFile,
                        L"firmware-view.json",
                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
                        0
                        );
  if (!EFI_ERROR(Status)) {
    ExistingFile->Delete(ExistingFile);
  }

  Status = ReportDir->Open(
                        ReportDir,
                        &File,
                        L"firmware-view.json",
                        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                        0
                        );
  if (!EFI_ERROR(Status)) {
    File->SetPosition(File, 0);
    WriteSize = JsonLength;
    Status = File->Write(File, &WriteSize, Json);
    File->Flush(File);
    File->Close(File);
  }

  ReportDir->Close(ReportDir);
  return Status;
}

EFI_STATUS
WriteFirmwareViewJson (
  IN EFI_HANDLE                                      ImageHandle,
  IN EFI_SYSTEM_TABLE                                *SystemTable,
  IN EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER    *AcpiRsdp,
  IN CONST UEFI_BOOT_LAB_BOOT_INFO                   *BootInfo
  )
{
  EFI_STATUS   Status;
  JSON_WRITER  Writer;

  ZeroMem(&Writer, sizeof(Writer));
  Writer.Capacity = FIRMWARE_JSON_CAPACITY;
  Writer.Buffer   = AllocateZeroPool(Writer.Capacity);
  if (Writer.Buffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  BuildFirmwareJson(&Writer, SystemTable, AcpiRsdp, BootInfo);
  if (Writer.Truncated) {
    FreePool(Writer.Buffer);
    return EFI_BUFFER_TOO_SMALL;
  }

  Status = WriteJsonToLoadedImageVolume(ImageHandle, Writer.Buffer, Writer.Length);
  FreePool(Writer.Buffer);
  return Status;
}
