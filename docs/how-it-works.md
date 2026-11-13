# How it works

## A USB boot stage instead of a BIOS flash

The USB contains a standalone x64 UEFI application. Firmware starts it from
`EFI/BOOT/BOOTX64.EFI`; it configures the GPU in RAM and then starts Windows
Boot Manager from the configured Windows EFI partition. No motherboard or
GPU firmware is flashed. The configuration must be applied again on subsequent
boots through this loader.

The tested card has 20 GiB VRAM but originally exposed a 256 MiB BAR1 window.
The loader selects a 32 GiB aperture large enough to cover that VRAM. This
changes the CPU-visible address window, not the amount of physical memory.

## Boot sequence

1. **Validate the machine.** Check the board/BIOS, GPU identity, PCI bridge,
   original register values, graphics mode and writable protocol storage.
   Capture diagnostics to the USB. Failed checks disable B.
2. **Reserve address space.** Use UEFI DXE Global Coherency Domain services to
   reserve 32 GiB plus 32 MiB at `0x2000000000`. Verify ownership before use.
3. **Prepare display access.** Install temporary RAM adapters for the public
   Graphics Output Protocol (GOP) and GPU PCI I/O protocol. Test a small pixel
   write/read/restore before changing the GPU mapping.
4. **Resize with decoding disabled.** Temporarily mask firmware callbacks and
   disable memory decoding on the GPU and upstream bridge. Change the GA102
   framebuffer-size override at PCI configuration offset `0x610` from `0x1001`
   to `0x1C81`. Verify the enlarged supported-size mask, then select size index
   15 at `0xBC0` (`0xF01`), corresponding to 32 GiB.
5. **Verify and relocate.** Probe the hardware BAR mask, assign BAR1 to
   `0x2000000000`, BAR3 to `0x2800000000`, and update the bridge's prefetchable
   window. Restore decoding and compare a framebuffer sample at the new address.
6. **Keep firmware display access working.** Update the exposed framebuffer
   address before callbacks resume. GOP drawing uses the new mapping and keeps
   the existing resolution. PCI memory access and resource descriptions also
   use the relocated addresses.
7. **Hand off to Windows.** Retain the address reservation, repeat the display
   self-test, load Windows Boot Manager and verify GPU/bridge state again.
   Windows and the NVIDIA driver then perform their normal initialization.

The override follows the research in
[geforce-bar1-resize](https://github.com/noon-at-cgn/geforce-bar1-resize).
Linux can reassign PCI resources after firmware boot; this implementation does
its allocation and display adaptation before starting Windows.

## Why the resource mapping matters

On the tested MSI firmware, PCI I/O resource indices are compact: physical
BAR0, BAR1 and BAR3 appear as indices 0, 1 and 2. Index 3 describes I/O ports.
Version 0.6 assumed index 3 meant physical BAR3 and correctly stopped when that
check failed. Version 0.6.1 discovers memory resources by their original bases,
sizes and descriptor fields, then uses those indices consistently.

The MSI resource descriptor's maximum is an end address. The wrappers preserve
that convention; they also support the alignment-mask convention used by EDK II.
No private firmware driver structures are patched.

## Failure handling and persistence

Failures before commitment use the verified shrink-and-restore path where
possible. If restoration or the retained configuration cannot be verified,
the loader stops for a full power-off. A watchdog is armed during the trial,
but manual power-off can still be necessary.

These protocol adapters run during UEFI boot services. There is no Windows
kernel patch, persistent EFI-variable change, ACPI modification or S3 resume
script. Sleep/resume and integration with the separate FP32 unlocker are not
implemented or validated.

## What was tested

- Capability override alone: Windows black-screened and rebooted.
- Native firmware reconnect: Windows booted, but resources were not resized.
- Address reservation, temporary relocation and temporary resize: passed,
  including restoration to the original configuration.
- **v0.6.1 retained handoff: succeeded on November 13, 2026.**
- NVIDIA 610.88 reported **32768 MiB BAR1**, **20480 MiB physical VRAM**;
  Windows recorded the 32 GiB resource and GPU status OK / error code 0.
- The user reported GPU-Z **Resizable BAR Enabled**. This establishes one
  successful boot, not workload performance or long-term stability.
- 38 host mock tests passed for allocation, rollback, display operations,
  resource mapping and protocol behavior.

Included tested EFI SHA-256:

```text
1926f7d6d3d15732c494ef459f9095cf76e5264ef2ead040786ebff07e5a70fb
```

## Building

- Use Windows, Python, LLVM/Clang with lld-link (tested with LLVM 20), and an
  EDK II source checkout for its headers.
- Set `windows_guid` in `main.c` to the target Windows EFI partition GUID.
  This is the partition GUID, not the disk GUID or filesystem volume ID.
- Other machines require reviewing the hardware/address checks as well;
  changing the GUID alone does not make this a universal loader.
- Build and run the host tests:

```powershell
.\Build.ps1 -Edk2 C:\path\to\edk2 -Clang C:\path\to\clang.exe -LldLink C:\path\to\lld-link.exe
```

The build updates `usb/` and creates `3080-ReBar-USB.zip`. A rebuild produces a
new artifact; the successful physical result above applies to the included
binary's hash. The build manifest's HardwareTested=false is intentional for
new builds until they are tested.

## Optional diagnostics

With the USB mounted as E:, run administrator PowerShell after a direct test boot:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File E:\Collect-Windows.ps1 -Label handoff -BootOutcome test-reached-windows
```

The collector saves Windows resource assignments, NVIDIA diagnostics and the
latest EFI log. Review logs before sharing: they can contain machine identifiers.
