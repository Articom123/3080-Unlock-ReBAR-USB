# 3080-ReBar-USB

Combined USB package for **nv-uefi-unlocker tensor/FP32-accumulation unlock + working ReBAR** on modded RTX 3080 20GB cards without having to flash the BIOS. The BIOS must already have ReBAR support and Above 4G Decoding enabled. Secure Boot must also be turned off. Fast Startup must also be turned off in Windows.

## Create the USB files (Windows)

- Download/extract this repository. Open PowerShell in its folder.
- Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\make-usbroot.ps1
```

- The script will need the Internet to both download the hash-verified unlocker tool and to download and generate firmware from the Nvidia kernel module 610.43.03, then creates **`usbroot/` with the final product**. LLVM and EDK are not necessary for this.
- Copy the **contents of `usbroot/`** to a USB drive formatted with FAT32 and MBR. The script never installs to a disk or changes boot entries.
- Save your work, fully shut down, select the UEFI USB in the boot menu, and **press B**. You will then be prompted shortly to **press Enter**. Your screen may go black for a few seconds during the BAR1 resize. Once finished, it will boot into Windows automatically.
- To confirm if ReBAR is enabled afterwards, use a program like GPU-Z. Device Manager will also show "Large Memory Range" in the GPU properties.
- To confirm the tensor unlock worked, run **`tests/tensortest.py`**. You will need PyTorch compiled with CUDA to run it. For a 3080 you should go from around 59-64 TFLOPS to 100+ TFLOPS after the unlock.
- This script does not survive reboots or shutdowns. Every reboot or shutdown will require booting off the USB again and following the same steps for the unlocks.

## Optional ReBAR logging

- ReBAR file logs and firmware dumps are **disabled by default**. Boot menus and error messages stay visible.
- To enable diagnostics, generate the package with `powershell -ExecutionPolicy Bypass -File .\make-usbroot.ps1 -Logging`.
- Regenerate without `-Logging` and copy the updated files to disable them again. Existing logs are preserved.
- This controls ReBAR diagnostics only; nv-uefi-unlocker still writes its own `loader.log`.

## Compatibility / limits

- Targets **RTX 3080 20GB, NVIDIA device ID `10de:2206`**. Other cards are not supported by the ReBAR stage.
- Requires Above 4G Decoding and Re-Size BAR Support enabled, UEFI boot (CSM disabled), Secure Boot disabled, and Fast Startup disabled.
- GPU, bridge, BAR addresses and Windows partition are discovered at boot. Unsupported topology/display/resource checks can disable resizing; "universal" does not guarantee every motherboard works.
- Setup requires Windows PowerShell 5.1+, internet access and space for firmware downloads. Inputs are cached in `.cache/`.
- If boot fails, fully power off and select your normal Windows entry by not booting from the USB and using Windows Boot Manager.

## Tested / untested

- So far, it has been tested only on an AMD motherboard, the MSI MS-7E80 / B850 Gaming Plus WiFi 6E with Bios 1.A41.
- Only the 3080 20GB has been tested and supported. While it is probably also possible for the 4080 32GB to work due to the 32GB BAR1 resize, it is not supported or tested here. Moreover, the FP32 accumulation unlock this repo uses only works on Ampere.
- This automated package pins **nv-uefi-unlocker v2.1.11**.
- Sleep/hibernate specifically **do not work**. The PC will not wake after trying to sleep or hibernate and will require a hard reboot.
- Other USB formats like exFAT and GPT have not been tested. If your USB drive is bigger than 32GB, make a new partition that's 32GB or less to format it as FAT32.

[How it works and building from source](docs/how-it-works.md) ? [Credits/licenses](THIRD-PARTY-NOTICES.md)
