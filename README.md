# 3080-Unlock-ReBAR-USB

Combined USB package for **nv-uefi-unlocker tensor/FP32-accumulation unlock + working ReBAR** on modded RTX 3080 20GB cards without having to flash the BIOS on Windows 11. 

Prerequisites:
- The BIOS must already have ReBAR support and Above 4G Decoding enabled.
- Secure Boot must be turned off.
- Fast Startup must be turned off in Windows.
- Your 3080 20GB must have the PCI ID 10DE:2206 and Subsystem ID 146D:10DE (PCI\VEN_10DE&DEV_2206&SUBSYS_146D10DE). If yours differs, you will have to modify the source and build it manually.
- USB drive with at least 2GB formatted as FAT32 and MBR
- Visual C++ Redistributable x64 for Nvidia's firmware extraction

## Contents

- [Create the USB files (Windows)](#create-the-usb-files-windows)
- [Compatibility / limits](#compatibility--limits)
- [Tested / untested](#tested--untested)
- [How it works and optional building from source](sources/docs/how-it-works.md) 
- [Credits/licenses](THIRD-PARTY-NOTICES.md)
- [Optional ReBAR logging](#optional-rebar-logging)
---

## Create the USB files (Windows)

1. Download/extract this repository. Run **run.bat** 
    - Alternatively, run it directly in PowerShell: `powershell -NoProfile -ExecutionPolicy Bypass -File .\make-usbroot.ps1`
    

2. Copy the **contents of `usbroot/`** to a USB drive formatted with FAT32 and MBR (uses around 350MB). The script never installs to a disk or changes boot entries.
3. Save your work, fully shut down, select the UEFI USB in the boot menu, and **press B**. You will then be prompted again shortly after to **press Enter**. Your screen may go black for a few seconds during the BAR1 resize. Once finished, it will boot into Windows automatically.
4. To confirm if ReBAR is enabled afterwards, use a program like GPU-Z. Device Manager will also show "Large Memory Range" in the GPU properties.
5. To confirm the tensor unlock worked, run **`sources/tests/tensortest.py`**. You will need PyTorch compiled with CUDA to run it. For a 3080, you should go from around ~59-64 TFLOPS to ~100+ TFLOPS after the unlock.

Notes:
- This script does not survive reboots or shutdowns. Every reboot or shutdown will require booting the USB again and following the same steps for the unlocks.
- The script will need the Internet to both download the hash-verified unlocker tool and to download and generate firmware from the Nvidia kernel module 610.43.03, then creates **`usbroot/`**. LLVM and EDK are not necessary for this.

## Compatibility / limits

- Targets **RTX 3080 20GB, NVIDIA device ID `10de:2206`**. Other cards are not supported by the ReBAR stage. The FP32 accumulation unlock only works on Ampere.
- Requires Above 4G Decoding and Re-Size BAR Support enabled, UEFI boot (CSM disabled), Secure Boot disabled, and Fast Startup disabled.
- GPU, bridge, BAR addresses and Windows partition are discovered at boot. Unsupported topology/display/resource checks can disable resizing; "universal" does not guarantee every motherboard works.
- Setup requires Windows PowerShell 5.1+, internet access and space for firmware downloads. Inputs are cached in `.cache/`.
- If boot fails, fully power off and select your normal Windows entry (do not not boot from the USB, use Windows Boot Manager).

## Tested / untested

- So far, it has been tested only on an AMD motherboard, the MSI MS-7E80 / B850 Gaming Plus WiFi 6E with BIOS 1.A41.
- Only the 3080 20GB has been tested and supported. While it is probably also possible for the 4080 32GB to work due to the 32GB BAR1 resize, it is not supported or tested here. Moreover, the FP32 accumulation unlock this repo uses only works on Ampere.
- This automated package pins **nv-uefi-unlocker v2.1.11**.
- Sleep/hibernate specifically **do not work**. The PC will not wake after trying to sleep or hibernate and will require a hard reboot.
- Other USB formats like exFAT and GPT have not been tested. If your USB drive is bigger than 32GB, make a new partition less than 32GB and format it as FAT32. USB 3 and 2 work fine.

## Optional ReBAR logging

- ReBAR file logs and firmware dumps are **disabled by default**. Boot menus and error messages stay visible.
- To enable diagnostics, run `powershell -ExecutionPolicy Bypass -File .\make-usbroot.ps1 -Logging` and replace any existing files on the USB
- Regenerate without `-Logging` and copy the updated files to disable them again. Existing logs are preserved.
- This controls ReBAR diagnostics only. Only necessary for debugging purposes.
