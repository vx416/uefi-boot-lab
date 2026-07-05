#ifndef UEFI_BOOT_LAB_PCI_CONFIG_H
#define UEFI_BOOT_LAB_PCI_CONFIG_H

#include <IndustryStandard/Acpi.h>

// Mental model category: PCIe and devices / device discovery.
//
// MCFG gives the ECAM base address. This reader uses that ECAM window to read
// PCI configuration headers, which is how the OS discovers actual PCIe
// endpoints and bridges before it reads BARs or binds drivers.
VOID
PrintPciDevicesFromMcfg (
  IN EFI_ACPI_DESCRIPTION_HEADER  *Mcfg
  );

#endif
