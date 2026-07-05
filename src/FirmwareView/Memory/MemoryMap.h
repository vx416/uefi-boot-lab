#ifndef UEFI_BOOT_LAB_MEMORY_MAP_H
#define UEFI_BOOT_LAB_MEMORY_MAP_H

#include <Uefi.h>

// Mental model category: CPU, memory, and physical address space.
//
// This printer uses Boot Services to query the UEFI memory map descriptor table
// before the OS loader would normally call ExitBootServices().
EFI_STATUS
PrintMemoryMap (
  IN EFI_BOOT_SERVICES  *BootServices
  );

// Print an already-captured memory map. This keeps JSON and console output on
// the same snapshot when the caller captured the map for BootInfo first.
EFI_STATUS
PrintMemoryMapSnapshot (
  IN EFI_MEMORY_DESCRIPTOR  *MemoryMap,
  IN UINTN                  MemoryMapSize,
  IN UINTN                  DescriptorSize
  );

#endif
