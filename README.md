# 3080-ReBar-USB

Experimental USB EFI loader that enabled ReBAR on a modded RTX 3080 20GB in Windows, without flashing the motherboard BIOS.

## Install to USB

- **Check compatibility below first. The included binary is machine-specific.**
- Download this repository using **Code ? Download ZIP** and extract it.
- Use a FAT32 USB drive. Back up any existing `EFI/BOOT/BOOTX64.EFI` first.
- Copy the **contents of `usb/`** to the USB root: `EFI/BOOT/BOOTX64.EFI` must be present.
- Save your work, fully shut down, then select the UEFI USB in the one-time boot menu.
- Press **B** to enable the 32 GiB BAR1 and boot Windows. **P**, Enter, Escape or the 20-second timeout boots without resizing.
- Check GPU-Z for **Resizable BAR: Enabled** and **BAR1: 32768 MB**.
- If it hangs or resets, fully power off and use your usual boot entry. If B is disabled, retain the automatically saved `RH*.TXT` log.

## Compatibility / limitations

- Tested: **MSI B850 GAMING PLUS WIFI6E (MS-7E80), BIOS 1.A41, RTX 3080 20GB**, Windows 11 build 26300, NVIDIA 610.88.
- Requires **Above 4G Decoding** and **Re-Size BAR Support** enabled.
- Exact GPU/subsystem, PCI topology, existing addresses and display checks are enforced. Other systems may refuse to run.
- **The binary targets the author's Windows EFI partition GUID. It will not boot another Windows installation without changing `windows_guid` in `main.c` and rebuilding.**
- Unsigned EFI application: Secure Boot must permit it, through your own signing setup or by disabling Secure Boot.
- Uses a 32 GiB address window; physical VRAM remains 20GB.
- Applies only when booting through this loader. Does not flash BIOS/VBIOS or install a Windows driver.
- Boots Windows directly, bypassing the separate FP32 unlocker.
- Sleep and hibernate **do not work** and will require a hard reboot.

## Untested

- Repeated cold boots, long workloads, performance gains and full-VRAM integrity.
- Other GPUs, boards, BIOS versions, drivers and display configurations.
- Combined operation with the FP32 unlocker.

[How it works, test history and building from source](docs/how-it-works.md)
