#include <Uefi.h>
#include <Guid/UefiBootLabPlatformTable.h>
#include <Library/UefiBootServicesTableLib.h>

//
// Firmware producer simulation.
//
// Mental model category:
// - Firmware producer.
// - Platform-description publication.
//
// In a real platform, BIOS/UEFI firmware publishes hardware description data
// during DXE. This lab driver publishes only a small custom marker table so the
// rest of the project can validate producer -> consumer -> OS report flow.
//
STATIC UEFI_BOOT_LAB_PLATFORM_TABLE  mUefiBootLabPlatformTable = {
  UEFI_BOOT_LAB_PLATFORM_TABLE_MAGIC,
  UEFI_BOOT_LAB_PLATFORM_TABLE_VERSION,
  sizeof(UEFI_BOOT_LAB_PLATFORM_TABLE),
  "uefi-boot-lab-q35",
  "firmware-producer-v1"
};

EFI_STATUS
EFIAPI
FirmwareProducerDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  //
  // InstallConfigurationTable publishes a GUID-tagged pointer into
  // EFI_SYSTEM_TABLE.ConfigurationTable. FirmwareView.efi and OS loaders can
  // later discover this table by searching for gUefiBootLabPlatformTableGuid.
  //
  // Do not call Print() here. OVMF dispatches DXE drivers before every boot
  // console path is guaranteed to exist; UefiLib Print asserts if ConsoleOut is
  // still NULL. FirmwareView.efi is the later consumer layer that prints the
  // published table after the boot application environment is ready.
  //
  Status = gBS->InstallConfigurationTable(
                  &gUefiBootLabPlatformTableGuid,
                  &mUefiBootLabPlatformTable
                  );
  return Status;
}
