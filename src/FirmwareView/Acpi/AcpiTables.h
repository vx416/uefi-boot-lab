#ifndef UEFI_BOOT_LAB_ACPI_TABLES_H
#define UEFI_BOOT_LAB_ACPI_TABLES_H

#include <Uefi.h>
#include <IndustryStandard/Acpi.h>

// Mental model category: Hardware description layer / topology and control.
//
// This parser receives the ACPI RSDP pointer found by the ConfigurationTable
// layer, then walks XSDT/RSDT to list the ACPI tables that describe CPU,
// interrupt, NUMA, PCIe, power, thermal, IOMMU, and RAS data.
VOID
PrintAcpiTables (
  IN EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *Rsdp
  );

#endif
