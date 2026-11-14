# Assemble nv-uefi-unlocker + ReBarUSB-Universal locally. Never install to a disk.
param(
    [ValidateSet('ga102','rtx30','30')][string]$Family='ga102',
    [string]$ReBarRoot=(Join-Path $PSScriptRoot 'sources'),
    [string]$Out=(Join-Path $PSScriptRoot 'usbroot'),
    [string]$Work=(Join-Path $PSScriptRoot '.cache\firmware'),
    [string]$AssetsExe='', [string]$BootEfi='', [string]$ProvenBlob='',
    [ValidateSet('auto','arch','run','cachyos','cachyos4','local')][string]$UtilsSource='auto',
    [switch]$NoInstallPrompt,
    [switch]$Logging,
    [string]$InstallTo=''
)
$ErrorActionPreference='Stop'
if($InstallTo){throw 'This packager only creates usbroot. Copy its contents manually.'}
$base=[IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\')+'\'
$Out=[IO.Path]::GetFullPath($Out)
if(!$Out.StartsWith($base,[StringComparison]::OrdinalIgnoreCase)){throw '-Out must be a folder inside this repository, not a disk target.'}
for($check=$Out;$check -and $check.Length -ge $base.TrimEnd('\').Length;$check=Split-Path $check -Parent){
    if((Test-Path -LiteralPath $check) -and ((Get-Item -LiteralPath $check).Attributes -band [IO.FileAttributes]::ReparsePoint)){throw 'Output cannot pass through a junction or symbolic link.'}
}
$package=Join-Path $ReBarRoot 'usb'
$rebar=Join-Path $package 'EFI\BOOT\BOOTX64.EFI'
. (Join-Path $PSScriptRoot 'sources/scripts/Get-Unlocker.ps1')
$dependency=Get-PinnedUnlocker $PSScriptRoot
if(!$BootEfi){$BootEfi=Join-Path $dependency 'BOOTX64.EFI'}
if(!$AssetsExe){$AssetsExe=Join-Path $dependency 'assets-x86_64-windows.exe'}
$BootEfi=(Resolve-Path -LiteralPath $BootEfi).Path
if($BootEfi.StartsWith($Out.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Use the original nv-uefi BOOTX64.EFI outside usbroot as -BootEfi.'}
$manifest=Get-Content -LiteralPath (Join-Path $package 'manifest.json') -Raw | ConvertFrom-Json
$entry=@($manifest.Files | Where-Object {$_.Path.Replace('/','\') -eq 'EFI\BOOT\BOOTX64.EFI'})
$rebarHash=(Get-FileHash -LiteralPath $rebar).Hash
$unlockHash=(Get-FileHash -LiteralPath $BootEfi).Hash
if($entry.Count -ne 1 -or $entry[0].SHA256 -ne $rebarHash){throw 'ReBAR binary does not match its build manifest. Rebuild ReBarUSB-Universal first.'}
if($rebarHash -eq $unlockHash){throw 'The unlocker and ReBAR loader must be different binaries.'}
# Validate dependencies before any firmware downloads or output changes.
foreach($name in @('Collect-Windows.ps1','README.md','LICENSE.txt','EDK2-LICENSE.txt')){
    if(!(Test-Path -LiteralPath (Join-Path $package $name))){throw "Missing ReBAR package file: $name"}
}
& (Join-Path $dependency 'make-usbroot.ps1') -Family $Family -AssetsExe $AssetsExe -BootEfi $BootEfi -Out $Out -Work $Work -ProvenBlob $ProvenBlob -UtilsSource $UtilsSource -NoInstallPrompt
if(!$?){throw 'Unlocker firmware generation failed.'}
# Always emit 0 or 1: copying a new package overwrites an old enabled setting.
Set-Content -LiteralPath (Join-Path $Out 'REBAR-LOGGING.TXT') -Value ([int][bool]$Logging) -Encoding Ascii
$boot=Join-Path $Out 'EFI\BOOT'
# Copy from the original input on every run; never rename yesterday's ReBAR loader.
Copy-Item -LiteralPath $BootEfi -Destination (Join-Path $boot 'UNLOCKER.EFI') -Force
foreach($relative in @('EFI\BOOT\BOOTX64.EFI','BOOTX64.EFI')){
    Copy-Item -LiteralPath $rebar -Destination (Join-Path $Out $relative) -Force
    if((Get-FileHash -LiteralPath (Join-Path $Out $relative)).Hash -ne $rebarHash){throw 'ReBAR copy verification failed.'}
}
if((Get-FileHash -LiteralPath (Join-Path $boot 'UNLOCKER.EFI')).Hash -ne $unlockHash){throw 'Unlocker copy verification failed.'}
Copy-Item -LiteralPath (Join-Path $package 'Collect-Windows.ps1') -Destination $Out -Force
$info=Join-Path $Out 'ReBAR'
New-Item -ItemType Directory -Path $info -Force | Out-Null
foreach($name in @('README.md','LICENSE.txt','EDK2-LICENSE.txt','manifest.json')){Copy-Item -LiteralPath (Join-Path $package $name) -Destination $info -Force}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'sources/licenses/nv-uefi-unlocker-GPL-2.0.txt') -Destination (Join-Path $Out 'UNLOCKER-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md') -Destination (Join-Path $Out 'README.md') -Force
[ordered]@{
    Mode='nv-uefi-unlocker-then-rebar-before-windows'
    ReBarLogging=[bool]$Logging
    UpstreamVersion=(Get-Content (Join-Path $PSScriptRoot 'sources/scripts/unlocker.lock.json') -Raw | ConvertFrom-Json).version
    ReBarSha256=$rebarHash;UnlockerSha256=$unlockHash
    BuiltUtc=[DateTime]::UtcNow.ToString('o')
    Files=@(Get-ChildItem -LiteralPath $Out -File -Recurse | Where-Object {$_.Name -ne 'combined-manifest.json'} | ForEach-Object {
        [ordered]@{Path=$_.FullName.Substring($Out.TrimEnd('\').Length+1);SHA256=(Get-FileHash -LiteralPath $_.FullName).Hash}
    })
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Out 'combined-manifest.json') -Encoding UTF8
Write-Host "Combined package ready: $Out"
Write-Host 'Copy the contents to your FAT32 USB yourself; boot the USB and press B.'
