#ifndef UEFI_BOOT_LAB_SMBIOS_H
#define UEFI_BOOT_LAB_SMBIOS_H

#include <Uefi.h>
#include <IndustryStandard/SmBios.h>

// Mental model category: Inventory / identity.
//
// SMBIOS is firmware-published inventory data. In Phase 1, this module prints
// the UEFI firmware view that Linux userspace later exposes through dmidecode.
VOID
PrintSmbiosBasicInfo (
  IN SMBIOS_TABLE_ENTRY_POINT      *SmbiosEntryPoint32,
  IN SMBIOS_TABLE_3_0_ENTRY_POINT  *SmbiosEntryPoint64
  );

#endif
