param(
    [Parameter(Mandatory=$true)][string]$Edk2,
    [string]$Clang='clang.exe',
    [string]$LldLink='lld-link.exe',
    [string]$Python='python'
)
$ErrorActionPreference='Stop'
$build=Join-Path $PSScriptRoot 'build'
$package=Join-Path $PSScriptRoot 'usb'
$boot=Join-Path $package 'EFI\BOOT'
New-Item -ItemType Directory -Path $build,$boot -Force | Out-Null
$flags=@('-target','x86_64-pc-windows-msvc','-std=c11','-ffreestanding','-fno-builtin',
    '-fno-stack-protector','-mno-red-zone','-fshort-wchar','-Os','-Wall','-Wextra','-Werror',
    '-I',(Join-Path $Edk2 'MdePkg\Include'),'-I',(Join-Path $Edk2 'MdePkg\Include\X64'))
foreach($name in @('main','runtime','test_probe')) {
    & $Clang @flags -c (Join-Path $PSScriptRoot ($name+'.c')) -o (Join-Path $build ($name+'.obj'))
    if($LASTEXITCODE){throw "Compilation failed: $name"}
}
$efi=Join-Path $boot 'BOOTX64.EFI'
& $LldLink /nologo /nodefaultlib /subsystem:efi_application /entry:efi_main /machine:x64 /base:0 /dynamicbase /nxcompat /timestamp:0 /opt:ref /opt:icf ("/out:"+$efi) (Join-Path $build 'main.obj') (Join-Path $build 'runtime.obj')
if($LASTEXITCODE){throw 'EFI link failed'}
$dll=Join-Path $build 'test_probe.dll'
& $LldLink /nologo /dll /noentry /nodefaultlib /machine:x64 /dynamicbase /nxcompat /opt:ref ("/out:"+$dll) (Join-Path $build 'test_probe.obj') (Join-Path $build 'runtime.obj')
if($LASTEXITCODE){throw 'Test DLL link failed'}
& $Python -I (Join-Path $PSScriptRoot 'test_probe.py') $dll
if($LASTEXITCODE){throw 'Mock tests failed'}
& $Python -I (Join-Path $PSScriptRoot 'verify_image.py') $efi (Join-Path $build 'pe-verification.json')
if($LASTEXITCODE){throw 'EFI validation failed'}
foreach($name in @('README.md','Collect-Windows.ps1')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $package -Force}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'LICENSE') -Destination (Join-Path $package 'LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $Edk2 'License.txt') -Destination (Join-Path $package 'EDK2-LICENSE.txt') -Force
$files=@(Get-ChildItem -LiteralPath $package -Recurse -File | Where-Object {$_.Name -ne 'manifest.json'} | Sort-Object FullName | ForEach-Object {
    [ordered]@{Path=$_.FullName.Substring($package.Length+1);Bytes=$_.Length;SHA256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
})
$inputs=@('main.c','inventory.c','acpi.c','capture.c','allocation.c','relocation.c','handoff.c','runtime.c','test_probe.c','test_handoff.c','test_probe.py','Build.ps1','verify_image.py','Collect-Windows.ps1','README.md') | ForEach-Object {
    [ordered]@{Path=$_;SHA256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $_) -Algorithm SHA256).Hash}
}
[ordered]@{Version='0.6.1';Mode='windows-32g-handoff';Board='B850 GAMING PLUS WIFI6E (MS-7E80)';Bios='1.A41';Gpu='0000:01:00.0 10de:2206 10de:146d';WindowsEsp='bcfca77e-5cf5-4208-b962-4cbb03a03177';HardwareTested=$false;BuiltUtc=[DateTime]::UtcNow.ToString('o');Sources=$inputs;Files=$files} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $package 'manifest.json') -Encoding UTF8
$zip=Join-Path $PSScriptRoot '3080-ReBar-USB.zip'
Compress-Archive -Path (Join-Path $package '*') -DestinationPath $zip -Force
Write-Output "Prepared USB package: $zip"
