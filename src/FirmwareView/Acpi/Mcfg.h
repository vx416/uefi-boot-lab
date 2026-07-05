#ifndef UEFI_BOOT_LAB_MCFG_H
#define UEFI_BOOT_LAB_MCFG_H

#include <IndustryStandard/Acpi.h>

// Mental model category: Hardware description layer / PCIe resource access.
//
// MCFG describes PCIe ECAM windows. Kernel PCI code uses these windows to read
// PCIe configuration space, discover BDFs, read BARs, and bind drivers.
VOID
PrintMcfg (
  IN EFI_ACPI_DESCRIPTION_HEADER  *Mcfg
  );

#endif
