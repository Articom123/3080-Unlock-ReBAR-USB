"""Offline packaging regression checks; mock upstream builder, no GPU/disk access."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
def digest(data): return hashlib.sha256(data).hexdigest()
with tempfile.TemporaryDirectory(prefix='rebar-package-') as tmp:
    root = Path(tmp)
    for name in ('make-usbroot.ps1', 'sources/scripts/Get-Unlocker.ps1', 'README.md', 'THIRD-PARTY-NOTICES.md',
                 'sources/licenses/nv-uefi-unlocker-GPL-2.0.txt'):
        dest = root / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(repo / name, dest)
    cache = root / '.cache/unlocker/test'
    cache.mkdir(parents=True)
    builder = b'''param($Family,$AssetsExe,$BootEfi,$Out,$Work,$ProvenBlob,$UtilsSource,[switch]$NoInstallPrompt)
if(!$NoInstallPrompt){throw 'Install prompt must be disabled'}
New-Item -ItemType Directory -Path (Join-Path $Out 'EFI/BOOT') -Force | Out-Null
Copy-Item -LiteralPath $BootEfi -Destination (Join-Path $Out 'EFI/BOOT/BOOTX64.EFI') -Force
Set-Content -LiteralPath (Join-Path $Out 'firmware.bin') -Value 'fixture-firmware'
'''
    deps = {'BOOTX64.EFI': b'fixture-unlocker', 'assets-x86_64-windows.exe': b'unused',
            'make-usbroot.ps1': builder}
    for name, data in deps.items(): (cache / name).write_bytes(data)
    (root / 'sources/scripts/unlocker.lock.json').write_text(json.dumps(dict(
        version='test', base_url='https://invalid.example/',
        files={name: digest(data) for name, data in deps.items()})))
    package = root / 'sources/usb'
    (package / 'EFI/BOOT').mkdir(parents=True)
    efi = package / 'EFI/BOOT/BOOTX64.EFI'
    efi.write_bytes(b'fixture-rebar')
    (package / 'manifest.json').write_text(json.dumps(dict(Files=[dict(
        Path='EFI/BOOT/BOOTX64.EFI', SHA256=digest(efi.read_bytes()))])))
    for name in ('Collect-Windows.ps1', 'README.md', 'LICENSE.txt', 'EDK2-LICENSE.txt', 'NV_30_DXE-LICENSE.txt'):
        (package / name).write_text('fixture')
    def run(*args, ok=True, contains=None):
        result = subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass',
                                 '-File', str(root / 'make-usbroot.ps1'), *args],
                                capture_output=True, text=True, timeout=30)
        assert (result.returncode == 0) == ok, result.stdout + result.stderr
        if contains: assert contains in result.stdout + result.stderr
    for enabled in (False, True, False):
        run(*(['-Logging'] if enabled else []))
        out = root / 'usbroot'
        assert (out / 'REBAR-LOGGING.TXT').read_text().strip() == str(int(enabled))
        assert (out / 'EFI/BOOT/UNLOCKER.EFI').read_bytes() == deps['BOOTX64.EFI']
        assert (out / 'EFI/BOOT/BOOTX64.EFI').read_bytes() == b'fixture-rebar'
        manifest = json.loads((out / 'combined-manifest.json').read_text(encoding='utf-8-sig'))
        assert manifest['ReBarLogging'] == enabled
        for f in manifest['Files']:
            assert digest((out / f['Path']).read_bytes()).upper() == f['SHA256']
    run('-Out', str(root.anchor), ok=False, contains='-Out must be a folder')
    run('-InstallTo', str(root.anchor), ok=False, contains='only creates usbroot')
    efi.write_bytes(b'corrupt')
    run(ok=False, contains='does not match its build manifest')
    efi.write_bytes(b'fixture-rebar')
    (cache / 'BOOTX64.EFI').write_bytes(b'corrupt')
    run(ok=False, contains='Cached dependency hash mismatch')
print('Packaging passed: first/repeat generation, all hashes, disk-target rejection, ReBAR/cache corruption rejection.')
