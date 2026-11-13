# Read current Windows GPU resources and NVIDIA diagnostics; no GPU writes.
param(
    [ValidateSet('inventory','handoff','normal')][string]$Label='handoff',
    [ValidateSet('test-reached-windows','recovered-after-failure','unsure')][string]$BootOutcome='unsure'
)
$ErrorActionPreference='Stop'
$directory=Join-Path $PSScriptRoot ('Results\'+$Label+'-'+[DateTime]::Now.ToString('yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $directory -Force | Out-Null
Get-CimInstance Win32_BaseBoard | Select-Object Manufacturer,Product | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $directory 'board.json') -Encoding UTF8
Get-CimInstance Win32_BIOS | Select-Object Manufacturer,SMBIOSBIOSVersion | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $directory 'bios.json') -Encoding UTF8
$gpus=@(Get-CimInstance Win32_PnPEntity | Where-Object {$_.DeviceID -like 'PCI\VEN_10DE&DEV_2206*'})
$records=@()
foreach($gpu in $gpus) {
    $ranges=@()
    try {
        $ranges=@(Get-CimAssociatedInstance -InputObject $gpu -Association Win32_PnPAllocatedResource -ResultClassName Win32_DeviceMemoryAddress | ForEach-Object {
            [ordered]@{Start=('0x{0:X}' -f [uint64]$_.StartingAddress);End=('0x{0:X}' -f [uint64]$_.EndingAddress);Bytes=([uint64]$_.EndingAddress-[uint64]$_.StartingAddress+1);GiB=([double]([uint64]$_.EndingAddress-[uint64]$_.StartingAddress+1)/1GB)}
        })
    } catch { $_ | Out-String | Set-Content -LiteralPath (Join-Path $directory 'resource-query-error.txt') }
    $records += [ordered]@{Name=$gpu.Name;DeviceID=$gpu.DeviceID;Status=$gpu.Status;ConfigManagerErrorCode=$gpu.ConfigManagerErrorCode;MemoryRanges=$ranges}
}
ConvertTo-Json -InputObject $records -Depth 5 | Set-Content -LiteralPath (Join-Path $directory 'gpu-resources.json') -Encoding UTF8
# WMI can omit CmResourceTypeMemoryLarge ranges. Retain the raw boot resource
# lists too; these describe boot assignments and are not a live PCI config read.
# RegistryKey.GetValue does not return REG_RESOURCE_LIST bytes on .NET Framework;
# use the read-only Win32 registry API to preserve that value type.
if(-not ('Ga102ReadOnlyReg.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace Ga102ReadOnlyReg {
    public static class Native {
        [DllImport("advapi32.dll", CharSet=CharSet.Unicode, EntryPoint="RegOpenKeyExW")]
        static extern int Open(IntPtr root, string key, uint options, uint access, out IntPtr handle);
        [DllImport("advapi32.dll", CharSet=CharSet.Unicode, EntryPoint="RegQueryValueExW")]
        static extern int Query(IntPtr handle, string value, IntPtr reserved, out uint kind, byte[] data, ref uint size);
        [DllImport("advapi32.dll", EntryPoint="RegCloseKey")]
        static extern int Close(IntPtr handle);
        public static byte[] Read(string key, out uint kind) {
            kind=0;IntPtr handle;
            int error=Open(new IntPtr(unchecked((int)0x80000002)),key,0,0x20119,out handle);
            if(error!=0)throw new Win32Exception(error);
            try {
                uint size=0;error=Query(handle,"BootConfig",IntPtr.Zero,out kind,null,ref size);
                if(error!=0)throw new Win32Exception(error);
                if(kind!=8 || size==0 || size>1048576)throw new InvalidOperationException("Unexpected BootConfig type/size.");
                byte[] bytes=new byte[size];
                error=Query(handle,"BootConfig",IntPtr.Zero,out kind,bytes,ref size);
                if(error!=0)throw new Win32Exception(error);
                if(kind!=8)throw new InvalidOperationException("BootConfig type changed.");
                if(size!=bytes.Length)Array.Resize(ref bytes,(int)size);
                return bytes;
            } finally { Close(handle); }
        }
    }
}
'@
}
$bootDevices=@([pscustomobject]@{Label='pci-root';DeviceID='ACPI\PNP0A08\0'})
$gpuIndex=0
foreach($gpu in $gpus){$bootDevices+=[pscustomobject]@{Label=('gpu-'+$gpuIndex);DeviceID=$gpu.DeviceID};$gpuIndex++}
$bootRecords=@()
foreach($device in $bootDevices) {
    try {
        $regPath='SYSTEM\CurrentControlSet\Enum\'+$device.DeviceID+'\LogConf'
        [uint32]$bootKind=0
        [byte[]]$bootBytes=[Ga102ReadOnlyReg.Native]::Read($regPath,[ref]$bootKind)
        $file=$device.Label+'-BootConfig.bin'
        [IO.File]::WriteAllBytes((Join-Path $directory $file),$bootBytes)
        $bootRecords+=[ordered]@{DeviceID=$device.DeviceID;File=$file;Bytes=$bootBytes.Length;Kind=$bootKind}
    } catch {
        $bootRecords+=[ordered]@{DeviceID=$device.DeviceID;Error=$_.Exception.Message}
    }
}
ConvertTo-Json -InputObject $bootRecords -Depth 4 | Set-Content -LiteralPath (Join-Path $directory 'boot-resources.json') -Encoding UTF8
$smi=Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
if($smi) {
    $previousPreference=$ErrorActionPreference
    $ErrorActionPreference='Continue'
    & $smi.Source -q 2>&1 | Out-File -LiteralPath (Join-Path $directory 'nvidia-smi.txt') -Encoding UTF8
    $smiExit=$LASTEXITCODE
    $ErrorActionPreference=$previousPreference
    "Exit code: $smiExit" | Add-Content -LiteralPath (Join-Path $directory 'nvidia-smi.txt')
} else { 'nvidia-smi.exe was not found on PATH.' | Set-Content -LiteralPath (Join-Path $directory 'nvidia-smi.txt') }
Get-CimInstance Win32_OperatingSystem | Select-Object Caption,Version,BuildNumber,LastBootUpTime |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'windows.json') -Encoding UTF8
[ordered]@{Label=$Label;BootOutcome=$BootOutcome;CollectedUtc=[DateTime]::UtcNow.ToString('o');Note='The label does not prove the test reached Windows. Use the EFI log and boot times.'} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'boot-outcome.json') -Encoding UTF8
$latest=Get-ChildItem -LiteralPath $PSScriptRoot -Filter 'RH????.TXT' -File | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if($latest){Copy-Item -LiteralPath $latest.FullName -Destination $directory}
try {
    Get-WinEvent -FilterHashtable @{LogName='System';ProviderName='Microsoft-Windows-WER-SystemErrorReporting';StartTime=(Get-Date).AddHours(-2)} -ErrorAction Stop |
        Select-Object TimeCreated,Id,Message | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (Join-Path $directory 'recent-crash-events.json') -Encoding UTF8
} catch { $_.Exception.Message | Set-Content -LiteralPath (Join-Path $directory 'crash-query-note.txt') }
Write-Output "Results saved in $directory"
Write-Output "Recorded boot outcome: $BootOutcome"
Write-Output 'This boot test attempts a 32 GiB BAR1. Preserve the EFI log and GPU resource results.'
