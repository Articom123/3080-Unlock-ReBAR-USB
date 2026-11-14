#!/bin/sh
# Linux build: clang + lld-link + an EDK II checkout (headers only). Runs the host tests natively.
# usage: ./build.sh /path/to/edk2
set -e
E=${1:?usage: ./build.sh /path/to/edk2}; cd "$(dirname "$0")"; mkdir -p build usb/EFI/BOOT
F="-std=c11 -ffreestanding -fno-builtin -fshort-wchar -mno-red-zone -Wall -Wextra -Werror -Wno-microsoft-static-assert -I $E/MdePkg/Include -I $E/MdePkg/Include/X64"
clang -target x86_64-pc-windows-msvc $F -fno-stack-protector -Os -c main.c -o build/main.obj
clang -target x86_64-pc-windows-msvc $F -fno-stack-protector -Os -c runtime.c -o build/runtime.obj
lld-link /nologo /nodefaultlib /subsystem:efi_application /entry:efi_main /machine:x64 /base:0 /dynamicbase /nxcompat \
  /timestamp:0 /opt:ref /opt:icf /out:usb/EFI/BOOT/BOOTX64.EFI build/main.obj build/runtime.obj
clang $F -fPIC -shared -fdeclspec -O1 -Wno-ignored-attributes test_probe.c runtime.c -o build/test_probe.so
python3 -I test_probe.py build/test_probe.so
python3 -I verify_image.py usb/EFI/BOOT/BOOTX64.EFI build/pe-verification.json
