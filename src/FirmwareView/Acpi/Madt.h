#ifndef UEFI_BOOT_LAB_MADT_H
#define UEFI_BOOT_LAB_MADT_H

#include <IndustryStandard/Acpi.h>

// Mental model category: Hardware description layer / topology.
//
// MADT, whose ACPI signature is "APIC", describes CPU local interrupt
// controllers and I/O interrupt controllers. Kernel code uses this table to
// build early CPU and interrupt topology before normal device drivers run.
VOID
PrintMadt (
  IN EFI_ACPI_DESCRIPTION_HEADER  *Madt
  );

#endif
