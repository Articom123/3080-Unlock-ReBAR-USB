function Get-PinnedUnlocker([string]$Repo) {
    $lock=Get-Content -LiteralPath (Join-Path $Repo 'sources/scripts/unlocker.lock.json') -Raw | ConvertFrom-Json
    $cache=Join-Path $Repo ('.cache\unlocker\'+$lock.version)
    New-Item -ItemType Directory -Path $cache -Force | Out-Null
    foreach($entry in $lock.files.PSObject.Properties) {
        $dest=Join-Path $cache $entry.Name
        if(Test-Path -LiteralPath $dest) {
            if((Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash -ieq $entry.Value){continue}
            throw "Cached dependency hash mismatch: $dest. Remove this cached file and rerun."
        }
        $part=$dest+'.part'
        $url=$lock.base_url+$entry.Name
        Write-Host "Downloading pinned unlocker $($lock.version): $($entry.Name)"
        $curl=Get-Command curl.exe -ErrorAction SilentlyContinue
        if($curl){
            & $curl.Source --fail --location --retry 3 --output $part $url
            if($LASTEXITCODE){throw "Download failed: $url"}
        } else {
            Invoke-WebRequest -Uri $url -OutFile $part -UseBasicParsing
        }
        if((Get-FileHash -LiteralPath $part -Algorithm SHA256).Hash -ine $entry.Value){throw "Downloaded dependency hash mismatch: $url"}
        Move-Item -LiteralPath $part -Destination $dest -Force
    }
    return $cache
}
