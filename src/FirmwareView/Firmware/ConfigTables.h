#ifndef UEFI_BOOT_LAB_CONFIG_TABLES_H
#define UEFI_BOOT_LAB_CONFIG_TABLES_H

#include <Uefi.h>

// Mental model category: Hardware description layer.
//
// This module owns UEFI ConfigurationTable discovery. It can either print the
// directory for learning/debugging, or return a VendorTable pointer for the next
// parser layer such as ACPI or SMBIOS.
VOID *
FindConfigurationTable (
  IN EFI_SYSTEM_TABLE  *SystemTable,
  IN EFI_GUID          *VendorGuid
  );

VOID
PrintConfigurationTables (
  IN EFI_SYSTEM_TABLE  *SystemTable
  );

#endif
