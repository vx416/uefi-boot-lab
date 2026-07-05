#ifndef UEFI_BOOT_LAB_PLATFORM_TABLE_H
#define UEFI_BOOT_LAB_PLATFORM_TABLE_H

#include <Uefi.h>

//
// Lab-specific firmware producer table.
//
// This is intentionally small: it demonstrates the producer/consumer path
// without pretending to describe real CPU, memory, or PCIe hardware policy.
//
#define UEFI_BOOT_LAB_PLATFORM_TABLE_MAGIC  SIGNATURE_32('U', 'B', 'L', 'B')
#define UEFI_BOOT_LAB_PLATFORM_TABLE_VERSION  1
#define UEFI_BOOT_LAB_PLATFORM_ID_MAX_BYTES  32
#define UEFI_BOOT_LAB_BUILD_MARKER_MAX_BYTES  32

typedef struct {
  UINT32  Magic;
  UINT16  Version;
  UINT16  Size;
  CHAR8   PlatformId[UEFI_BOOT_LAB_PLATFORM_ID_MAX_BYTES];
  CHAR8   BuildMarker[UEFI_BOOT_LAB_BUILD_MARKER_MAX_BYTES];
} UEFI_BOOT_LAB_PLATFORM_TABLE;

extern EFI_GUID  gUefiBootLabPlatformTableGuid;

#endif
