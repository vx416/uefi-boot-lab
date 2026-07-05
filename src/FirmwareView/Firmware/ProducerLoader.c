#include <Uefi.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>

#include "ProducerLoader.h"

//
// Lab producer loader.
//
// Mental model category:
// - Firmware producer simulation.
// - Pre-OS dependency loading.
//
// A production DXE driver would be built into the firmware volume and run
// during OVMF's DXE phase. This lab keeps the flow reproducible without
// rebuilding OVMF by loading the producer driver from the same EFI volume before
// FirmwareView consumes ConfigurationTable entries.
//
EFI_STATUS
LoadFirmwareProducerDriver (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS                 Status;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL   *DriverDevicePath;
  EFI_HANDLE                 DriverImageHandle;

  LoadedImage       = NULL;
  DriverDevicePath  = NULL;
  DriverImageHandle = NULL;

  Status = gBS->HandleProtocol(
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR(Status)) {
    Print(L"Producer loader could not locate loaded-image protocol: %r\r\n", Status);
    return Status;
  }

  //
  // FirmwareProducerDxe.efi is placed beside the report directory instead of at
  // BOOTX64.EFI. FileDevicePath builds a device path rooted at the device that
  // loaded FirmwareView.efi, so the lab works with both directory-backed FAT and
  // real FAT images.
  //
  DriverDevicePath = FileDevicePath(
                       LoadedImage->DeviceHandle,
                       L"\\EFI\\UEFI-BOOT-LAB\\FirmwareProducerDxe.efi"
                       );
  if (DriverDevicePath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gBS->LoadImage(
                  FALSE,
                  ImageHandle,
                  DriverDevicePath,
                  NULL,
                  0,
                  &DriverImageHandle
                  );
  if (EFI_ERROR(Status)) {
    Print(L"Firmware producer driver not loaded: %r\r\n", Status);
    FreePool(DriverDevicePath);
    return Status;
  }

  Status = gBS->StartImage(DriverImageHandle, NULL, NULL);
  if (EFI_ERROR(Status)) {
    Print(L"Firmware producer driver failed to start: %r\r\n", Status);
  }

  FreePool(DriverDevicePath);
  return Status;
}
