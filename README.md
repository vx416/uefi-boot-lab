# uefi-boot-lab

## Description

`uefi-boot-lab` is a learning project for understanding how server hardware is
described by firmware, interpreted by Linux, and validated by platform
engineering tools.

The goal is not to build a full bootloader. The goal is to build a small
diagnostics lab that answers this question:

```text
Can we trace how server hardware is described by firmware, interpreted by Linux, and validated against an expected platform design?
```

This repo uses QEMU + OVMF so the whole flow can be practiced locally.

## Why Learn This

The kernel is the manager of hardware resources. It decides how memory is used,
how interrupts are routed, how PCIe devices are discovered, which drivers bind
to which devices, and what abstractions userspace processes can safely use.

To manage those resources, the kernel first needs to understand the machine's
hardware layout and capabilities. It cannot safely manage RAM, CPUs, timers,
interrupt controllers, PCIe devices, or firmware runtime services if it does not
know where they are and how they are described.

That is why BIOS / UEFI does more than put a kernel image into memory and jump
to its entry point. Before the kernel can run correctly, firmware has to prepare
a usable execution environment and publish enough hardware description for the
loader and kernel to understand the machine.

The loader / pre-OS phase needs answers such as:

- where usable RAM is
- which physical address ranges are reserved
- where ACPI and SMBIOS entry points are
- how CPU, interrupt, PCIe, timer, and power topology are described
- where PCIe configuration space can be accessed
- what firmware services are still available before `ExitBootServices()`

Without this layer, the kernel entry point is just code running with very little
context. The kernel still needs a map of the platform before it can safely manage
memory, enumerate devices, configure interrupts, and bind drivers.

## What This Demonstrates

- basic EDK II / OVMF build flow
- a tiny DXE firmware producer publishing a GUID table
- a UEFI pre-OS consumer collecting firmware handoff data
- Linux-side collection of OS-visible platform state
- `expected-platform.yaml` validation with PASS / WARN / FAIL checks

## Table Of Contents

- [Boot And Hardware Description Mental Model](#boot-and-hardware-description-mental-model)
- [Handoff Data And Description Sources](#handoff-data-and-description-sources)
- [Project Scope](#project-scope)
- [Project Layout](#project-layout)
- [Quick Start](#quick-start)
- [Build A Custom OVMF BIOS Image](#build-a-custom-ovmf-bios-image)
- [Smoke Test](#smoke-test)
- [End-To-End Test](#end-to-end-test)
- [Make Targets](#make-targets)
- [Native EDK II Layout](#native-edk-ii-layout)

## Boot And Hardware Description Mental Model

The general boot and hardware-description model is:

```text
Physical hardware
  -> BIOS / UEFI initializes the platform
  -> BIOS / UEFI publishes hardware description entry points
  -> loader / pre-OS UEFI program is loaded by firmware
  -> loader or pre-OS diagnostics can read firmware handoff data
  -> loader builds the **boot handoff model** it needs
  -> loader passes the **handoff model** to the kernel entry point
  -> loader exits firmware boot services when ready
  -> loader transfers control to the kernel
  -> kernel parses descriptors and builds its own hardware model
  -> kernel starts drivers and userspace
```

The key idea is that boot is not only "load kernel bytes and jump." Firmware
first prepares the execution environment and publishes hardware descriptions.
The loader passes the kernel the **handoff model** it needs. The kernel then parses
those descriptors and builds the hardware model it will use to manage memory,
interrupts, devices, drivers, and userspace.

The handoff data is the bridge between firmware and the kernel. It tells the
kernel which memory ranges are safe to use, where firmware-owned or reserved
regions are, where ACPI and SMBIOS descriptions start, how interrupts and PCIe
roots are wired, and how device configuration space can be reached. Without that
context, the kernel would enter C code but would not yet know how to safely
manage the machine.

The next section breaks the **handoff model** into the concrete data sources a
loader or kernel depends on.

## Handoff Data And Description Sources

The **handoff model** is not one single table. It is built from several firmware
interfaces and hardware description sources. Each source answers a different
question the loader or kernel needs before the OS can safely manage the machine.

```text
firmware interface
  -> description entry points
  -> memory ownership
  -> platform topology and control
  -> device configuration
  -> hardware identity
  -> out-of-band management view
```

| Context Layer | What The Loader / Kernel Needs To Know | Provided By | Why Important / Linux View |
| --- | --- | --- | --- |
| Firmware interface | How does a loader talk to firmware? | UEFI `EFI_SYSTEM_TABLE`, Boot Services, Runtime Services | proves the boot path and firmware interface; visible in early boot logs and UEFI variables |
| Description entry points | Where do ACPI, SMBIOS, and custom tables start? | UEFI `ConfigurationTable` GUID-to-pointer entries | wrong pointers mean the OS cannot find platform descriptions; captured in firmware JSON |
| Memory ownership | Which physical address ranges are usable or reserved? | UEFI memory map, e820 | wrong ownership can break boot, runtime services, or device resources; visible in `/proc/iomem` and `dmesg` |
| Platform topology | How are CPUs, interrupts, PCIe roots, NUMA, IOMMU, timers, and power described? | ACPI RSDP, XSDT/RSDT, FACP, MADT/APIC, MCFG, SRAT, SLIT, DMAR | wrong topology breaks CPU, IRQ, NUMA, PCIe, or power behavior; visible through kernel ACPI parsing, `lscpu`, `numactl -H`, `lspci` |
| Device configuration | Which PCIe devices exist and how are they configured? | ACPI MCFG + PCIe config space | maps devices to slots and debugs BARs, link speed, and driver binding; visible in `lspci -vvv` and `lspci -tv` |
| Hardware identity | Which machine, BIOS, board, CPU socket, and DIMMs are present? | SMBIOS Type 0/1/2/3/4/17 | confirms actual hardware identity and FRUs; visible in `dmidecode` |
| Management view | What can the management controller see out-of-band? | BMC sensors, FRU, SEL, Redfish/IPMI | validates hardware even when host OS is down; visible through Redfish and `ipmitool` |

In a datacenter bring-up or hardware replacement case, these questions become a
debugging path. If the observed platform does not match the expected design,
start from the OS-visible symptom and walk backward through the layers:

```text
Linux sees the wrong platform state
  -> did Linux parse the expected descriptor?
  -> did the loader or pre-OS collector see the descriptor?
  -> did firmware publish the right table or memory map?
  -> did BIOS setup enable the expected platform feature?
  -> does the installed hardware match the design?
```

The point is not to memorize table names first. The point is to know which
source answers which hardware question, then use that path to locate where the
firmware-to-OS view started to diverge.

Short version:

```text
UEFI table entries = pointers to firmware-provided description data
Memory map         = who owns each physical address range
ACPI               = platform topology and control model
SMBIOS             = machine identity and hardware inventory
PCIe config        = device self-description
BMC / Redfish      = out-of-band management view
```

## Project Scope

The project currently models a mini server-platform validation flow.

It has four main modules:

- `expected-platform.yaml`: the expected platform contract used by diagnostics.
- `FirmwareProducerDxe`: a small OVMF DXE driver that simulates BIOS firmware
  publishing a platform identity table through UEFI `ConfigurationTable`.
- `FirmwareView.efi`: a pre-OS UEFI application that observes what hardware
  description data a loader receives from BIOS / UEFI, then writes
  `firmware-view.json`. It collects firmware vendor, UEFI revision, memory map
  summary, UEFI configuration table entries, ACPI table list, MADT/MCFG details,
  SMBIOS entry point, PCIe config-space scan results, and the custom lab
  firmware producer table.
- `tools/platform_report.py`: a typed Python Linux-side collector and report
  generator. It collects `/proc/iomem`, `dmesg` e820/EFI lines, `dmidecode`,
  `lspci -vvv`, `lspci -tv`, `lscpu`, and `numactl -H`, then compares the
  observed state against the expected platform contract.

`FirmwareView.efi` is a learning snapshot of the pre-OS firmware view. It shows
what a loader-like UEFI program can receive from BIOS / UEFI before Linux
starts. Real platform validation is done from the Linux OS view and diagnostics
report, because that is where the kernel has consumed the firmware descriptions,
enumerated devices, bound drivers, and exposed the final interpreted state.

The firmware-to-JSON path in this lab is:

```text
OVMF / BIOS image
  -> FirmwareProducerDxe publishes a custom platform identity table
  -> UEFI ConfigurationTable exposes the table pointer by GUID
  -> FirmwareView.efi runs as BOOTX64.EFI
  -> FirmwareView observes how a loader receives firmware-provided descriptors
  -> FirmwareView captures descriptor data
  -> EFI/UEFI-BOOT-LAB/firmware-view.json
```

`FirmwareProducerDxe` is a small OVMF DXE driver that simulates one narrow class
of BIOS-side producer behavior. In a real BIOS image, DXE drivers initialize
devices and publish platform data before the OS loader starts. This lab's
producer publishes a small custom GUID-tagged platform identity table so the
diagnostics path can verify that firmware, the pre-OS collector, and the
expected platform contract agree on the platform ID.

`FirmwareView.efi` represents a loader-like pre-OS UEFI program. It is not the
production telemetry path and it is not a kernel loader. Its job is to observe
what BIOS / UEFI provides to a loader: the UEFI system table, memory map, ACPI
entry point, SMBIOS entry point, PCIe configuration path, and the lab's custom
platform identity table.

The descriptor view is structured metadata about the machine:

```text
UEFI SystemTable
  -> firmware vendor / UEFI revision / ConfigurationTable root directory

UEFI memory map
  -> physical address ranges and ownership

ACPI
  -> CPU, interrupt, PCIe, timer, power, and platform topology

SMBIOS
  -> BIOS, system, board, chassis, CPU socket, and DIMM inventory

PCIe config space
  -> BDF, vendor/device ID, class, BAR, capability, link information
```

The validation flow is:

| Step | Role | Component | What It Does | Output |
| --- | --- | --- | --- | --- |
| Expected contract | PM / hardware design expectation | `expected-platform.yaml` | declares what the platform should expose | required ACPI tables, firmware vendors, memory floor, SMBIOS and Linux artifact requirements |
| BIOS / firmware producer | firmware-side source of truth | OVMF + `FirmwareProducerDxe` | simulates BIOS publishing platform identity during DXE | custom platform identity table in UEFI `ConfigurationTable` |
| Phase 1: firmware view | pre-OS learning snapshot | `FirmwareView.efi` | observes how a loader receives UEFI system table, memory map, ACPI, SMBIOS, PCIe, and custom platform data | `EFI/UEFI-BOOT-LAB/firmware-view.json` |
| Phase 2: Linux view | OS-side validation collector | `tools/platform_report.py` | collects Linux interpretation after the kernel consumes firmware descriptions | `/proc/iomem`, dmesg, dmidecode, lspci, lscpu, numactl artifacts |
| Phase 3: diagnostics | expected vs observed comparison | `platform_report.py` diagnostics | compares expected contract, firmware view, and Linux view | `platform-report.json` and `platform-report.md` |

The current expected platform contract is:

```yaml
platform_id: uefi-boot-lab-q35
expected_firmware_vendors:
  - Ubuntu distribution of EDK II
  - EDK II
required_acpi_tables:
  - FACP
  - APIC
  - MCFG
min_memory_mib: 512
require_smbios: true
required_linux_artifacts:
  - dmidecode
  - lspci_vvv
  - lscpu
```

## Project Layout

```text
.
├── Include/                  # EDK II package headers
├── src/FirmwareProducerDxe/  # lab firmware producer DXE driver
├── src/FirmwareView/         # UEFI pre-OS collector
├── tools/                    # Linux collector and report writer
├── scripts/                  # build, OVMF, smoke, and e2e helpers
├── expected-platform.yaml    # expected platform validation contract
├── UefiBootLabPkg.dec        # EDK II package declaration
├── UefiBootLabPkg.dsc        # EDK II package build description
├── Vagrantfile               # Ubuntu/QEMU development VM
└── Makefile                  # convenience targets
```

## Quick Start

Run the lab inside the provided Vagrant VM.

On the host:

```bash
vagrant up
vagrant ssh
```

Inside the VM:

```bash
cd ~/uefi-boot-lab
EDK2_TOOLCHAIN=GCCNOLTO GCCNOLTO_BIN=x86_64-linux-gnu- make build
```

Build outputs:

```text
/home/vagrant/workspace/edk2/Build/UefiBootLabPkg/DEBUG_GCCNOLTO/X64/FirmwareView.efi
/home/vagrant/workspace/edk2/Build/UefiBootLabPkg/DEBUG_GCCNOLTO/X64/FirmwareProducerDxe.efi
```

## Build A Custom OVMF BIOS Image

The producer can run in two modes:

- stock OVMF mode: `FirmwareView.efi` loads `FirmwareProducerDxe.efi` from the
  EFI FAT volume.
- custom BIOS image mode: OVMF dispatches `FirmwareProducerDxe` during DXE, so
  the table is already present before `FirmwareView.efi` starts.

Build the custom OVMF image:

```bash
cd ~/uefi-boot-lab
EDK2_TOOLCHAIN=GCCNOLTO GCCNOLTO_BIN=x86_64-linux-gnu- make ovmf
```

Output:

```text
/home/vagrant/workspace/edk2/Build/OvmfX64/DEBUG_GCCNOLTO/FV/OVMF_CODE.fd
```

`make ovmf` patches the VM's edk2 checkout by adding
`UefiBootLabPkg/src/FirmwareProducerDxe/FirmwareProducerDxe.inf` to OVMF's DSC
and FDF files, then rebuilds OVMF. It also adds
`-Wno-error=maybe-uninitialized` for current distro GCC compatibility.

## Smoke Test

Stock OVMF smoke test:

```bash
rm -rf ~/fat-dir
mkdir -p ~/fat-dir/EFI/BOOT ~/fat-dir/EFI/UEFI-BOOT-LAB

cp /home/vagrant/workspace/edk2/Build/UefiBootLabPkg/DEBUG_GCCNOLTO/X64/FirmwareView.efi \
  ~/fat-dir/EFI/BOOT/BOOTX64.EFI

cp /home/vagrant/workspace/edk2/Build/UefiBootLabPkg/DEBUG_GCCNOLTO/X64/FirmwareProducerDxe.efi \
  ~/fat-dir/EFI/UEFI-BOOT-LAB/FirmwareProducerDxe.efi

./scripts/smoke-ovmf.sh /usr/share/OVMF/OVMF_CODE_4M.fd ~/fat-dir
```

Custom OVMF smoke test:

```bash
./scripts/smoke-ovmf.sh \
  /home/vagrant/workspace/edk2/Build/OvmfX64/DEBUG_GCCNOLTO/FV/OVMF_CODE.fd \
  ~/fat-dir \
  /home/vagrant/workspace/edk2/Build/OvmfX64/DEBUG_GCCNOLTO/FV/OVMF_VARS.fd
```

Expected serial output includes:

```text
Firmware view collector for uefi-boot-lab
Firmware vendor: EDK II
UEFI revision: 2.70
Configuration tables: 11
Memory map descriptors: ...
Smoke test passed.
```

In custom OVMF mode, the log should also include:

```text
Lab firmware producer table already present in firmware image.
```

The firmware JSON is written to:

```text
~/fat-dir/EFI/UEFI-BOOT-LAB/firmware-view.json
```

## End-To-End Test

Run the full firmware-to-Linux flow:

```bash
cd ~/uefi-boot-lab
make e2e-linux
```

Run the same e2e flow with the custom OVMF image:

```bash
OVMF_CODE=/home/vagrant/workspace/edk2/Build/OvmfX64/DEBUG_GCCNOLTO/FV/OVMF_CODE.fd \
  E2E_FIRMWARE_TIMEOUT=20 \
  make e2e-linux
```

The first run downloads an Ubuntu cloud image. Successful output ends with:

```text
E2E report JSON: /home/vagrant/uefi-boot-lab/artifacts/e2e-linux/esp/EFI/UEFI-BOOT-LAB/linux-report/platform-report.json
E2E report Markdown: /home/vagrant/uefi-boot-lab/artifacts/e2e-linux/esp/EFI/UEFI-BOOT-LAB/linux-report/platform-report.md
```

Expected checks should look like:

```text
PASS platform_id firmware producer platform_id is uefi-boot-lab-q35
PASS expected_firmware_vendor firmware vendor is EDK II
PASS required_acpi_table.FACP required ACPI table FACP present
PASS required_acpi_table.APIC required ACPI table APIC present
PASS required_acpi_table.MCFG required ACPI table MCFG present
PASS required_linux_artifact.dmidecode Linux dmidecode collected
PASS required_linux_artifact.lspci_vvv Linux lspci_vvv collected
PASS required_linux_artifact.lscpu Linux lscpu collected
PASS require_smbios Linux SMBIOS identity parsed from dmidecode
```

Generated `artifacts/` are ignored by git.

## Make Targets

```text
make build          Build FirmwareView.efi and FirmwareProducerDxe.efi
make ovmf           Build custom OVMF with FirmwareProducerDxe in DXEFV
make e2e-linux      Run firmware view, boot Linux, and generate platform report
make collect-linux  Run the Linux collector wrapper on the current Linux system
make run            Print OVMF run-script usage
```

## Native EDK II Layout

Without Vagrant, this repository must appear inside an EDK II workspace as
`UefiBootLabPkg`:

```text
edk2/
  BaseTools/
  MdePkg/
  MdeModulePkg/
  UefiBootLabPkg/   # this repository
```

Example:

```bash
export EDK2_WORKSPACE=/path/to/edk2
export EDK2_TOOLCHAIN=GCCNOLTO
export GCCNOLTO_BIN=x86_64-linux-gnu-

ln -s /path/to/uefi-boot-lab "$EDK2_WORKSPACE/UefiBootLabPkg"
cd /path/to/uefi-boot-lab
make build
```

On ARM64 hosts building X64 EFI binaries, `GCCNOLTO_BIN` should point at an
x86_64 cross compiler.
