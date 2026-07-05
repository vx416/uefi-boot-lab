#include <Uefi.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiLib.h>

#include "MemoryMap.h"

//
// UEFI memory map printer.
//
// Mental model category:
// - CPU, memory, and physical address space.
//
// Where this sits in the platform flow:
// - UEFI Boot Services owns the machine before ExitBootServices().
// - GetMemoryMap() returns firmware's current descriptor table for physical
//   address ranges.
// - The OS loader uses this table to hand the kernel a safe map of usable RAM,
//   reserved ranges, runtime ranges, ACPI ranges, and MMIO ranges.
//
// Hardware information printed here:
// - Physical address ranges.
// - UEFI memory type for each range, such as Conventional, BootSvcData,
//   RunSvcCode, ACPIReclaim, Reserved, and MMIO.
// - Page count and memory attributes for each range.
//
// Purpose:
// - Show how firmware classifies the platform physical address space before
//   ExitBootServices().
// - Teach that the memory map is a descriptor table, not the RAM itself.
//
// What the kernel / OS loader can do with this information:
// - Use ConventionalMemory as the main pool of RAM after the firmware handoff.
// - Preserve RuntimeServicesCode/Data and runtime MMIO mappings.
// - Preserve or later reclaim ACPI-related ranges according to ACPI rules.
// - Avoid Reserved, Unusable, and MMIO ranges as normal RAM.
//
// What RD / validation can debug from this:
// - Missing or malformed usable RAM ranges can explain low memory capacity.
// - Large reserved or MMIO windows can explain why RAM appears missing.
// - Runtime ranges without expected attributes can break UEFI runtime services.
// - ACPI ranges help connect early firmware tables to later Linux acpidump
//   output.
//
STATIC
CONST CHAR16 *
MemoryTypeName (
  IN EFI_MEMORY_TYPE  Type
  )
{
  switch (Type) {
    case EfiReservedMemoryType:
      return L"Reserved";
    case EfiLoaderCode:
      return L"LoaderCode";
    case EfiLoaderData:
      return L"LoaderData";
    case EfiBootServicesCode:
      return L"BootSvcCode";
    case EfiBootServicesData:
      return L"BootSvcData";
    case EfiRuntimeServicesCode:
      return L"RunSvcCode";
    case EfiRuntimeServicesData:
      return L"RunSvcData";
    case EfiConventionalMemory:
      return L"Conventional";
    case EfiUnusableMemory:
      return L"Unusable";
    case EfiACPIReclaimMemory:
      return L"ACPIReclaim";
    case EfiACPIMemoryNVS:
      return L"ACPINVS";
    case EfiMemoryMappedIO:
      return L"MMIO";
    case EfiMemoryMappedIOPortSpace:
      return L"MMIOPort";
    case EfiPalCode:
      return L"PalCode";
    case EfiPersistentMemory:
      return L"Persistent";
    default:
      return L"Unknown";
  }
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
  // Phase 1 needs a compact firmware memory summary that can later be compared
  // with Linux /proc/iomem and dmesg e820 output. Keep the detailed descriptor
  // list below, but also aggregate the ownership classes humans debug first.
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
PrintPageSummary (
  IN CONST CHAR16  *Name,
  IN UINT64        Pages
  )
{
  Print(L"  %-12s pages=%8lu mib=%8lu\r\n", Name, Pages, Pages / 256);
}

EFI_STATUS
PrintMemoryMapSnapshot (
  IN EFI_MEMORY_DESCRIPTOR  *MemoryMap,
  IN UINTN                  MemoryMapSize,
  IN UINTN                  DescriptorSize
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

  if ((MemoryMap == NULL) || (DescriptorSize == 0)) {
    Print(L"Memory map snapshot is empty\r\n");
    return EFI_INVALID_PARAMETER;
  }

  // The caller owns this memory-map buffer. Do not call GetMemoryMap() again
  // here; allocations, file writes, and frees can change the UEFI memory map and
  // make console output disagree with firmware-view.json.
  DescriptorCount = MemoryMapSize / DescriptorSize;
  Print(L"Memory map descriptors: %u\r\n", DescriptorCount);

  Descriptor = MemoryMap;
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

    Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Descriptor + DescriptorSize);
  }

  Print(L"Memory map summary:\r\n");
  PrintPageSummary(L"Usable", UsablePages);
  PrintPageSummary(L"BootSvc", BootServicesPages);
  PrintPageSummary(L"Runtime", RuntimePages);
  PrintPageSummary(L"ACPI", AcpiPages);
  PrintPageSummary(L"Reserved", ReservedPages);
  PrintPageSummary(L"MMIO", MmioPages);

  Print(L"Idx Type            PhysicalStart       Pages      Attr\r\n");

  // Each descriptor is DescriptorSize bytes apart. Do not assume it is exactly
  // sizeof(EFI_MEMORY_DESCRIPTOR), because firmware may extend the structure.
  Descriptor = MemoryMap;
  for (Index = 0; Index < DescriptorCount; Index++) {
    Print (
      L"%03u %-15s 0x%016lx %8lu 0x%016lx\r\n",
      Index,
      MemoryTypeName(Descriptor->Type),
      Descriptor->PhysicalStart,
      Descriptor->NumberOfPages,
      Descriptor->Attribute
      );

    Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Descriptor + DescriptorSize);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
PrintMemoryMap (
  IN EFI_BOOT_SERVICES  *BootServices
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

  // Standalone helper path: query firmware for a fresh memory map. The main
  // FirmwareView flow uses PrintMemoryMapSnapshot() instead so JSON and console
  // output describe the same captured BootInfo buffer.
  Status = BootServices->GetMemoryMap (
                           &MemoryMapSize,
                           MemoryMap,
                           &MapKey,
                           &DescriptorSize,
                           &DescriptorVersion
                           );
  if (Status != EFI_BUFFER_TOO_SMALL) {
    Print(L"GetMemoryMap size query failed: %r\r\n", Status);
    return Status;
  }

  MemoryMapSize += DescriptorSize * 8;
  MemoryMap = AllocatePool(MemoryMapSize);
  if (MemoryMap == NULL) {
    Print(L"Failed to allocate memory map buffer\r\n");
    return EFI_OUT_OF_RESOURCES;
  }

  Status = BootServices->GetMemoryMap (
                           &MemoryMapSize,
                           MemoryMap,
                           &MapKey,
                           &DescriptorSize,
                           &DescriptorVersion
                           );
  if (EFI_ERROR(Status)) {
    Print(L"GetMemoryMap failed: %r\r\n", Status);
    FreePool(MemoryMap);
    return Status;
  }

  Status = PrintMemoryMapSnapshot(MemoryMap, MemoryMapSize, DescriptorSize);
  FreePool(MemoryMap);
  return Status;
}
