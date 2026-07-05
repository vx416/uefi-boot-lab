#ifndef UEFI_BOOT_LAB_PRODUCER_LOADER_H
#define UEFI_BOOT_LAB_PRODUCER_LOADER_H

#include <Uefi.h>

EFI_STATUS
LoadFirmwareProducerDriver (
  IN EFI_HANDLE  ImageHandle
  );

#endif
