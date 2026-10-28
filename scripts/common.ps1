Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:HlaProductName = 'HLA FTP Patch'

function Get-HlaFileHash([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }

function Get-HlaPeInfo([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "DLL does not exist: $Path" }
    $file = Get-Item -LiteralPath $Path
    if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Refusing a DLL symbolic link/reparse point: $Path" }
    if ($file.Length -lt 64 -or $file.Length -gt 536870912) { throw "Invalid PE DLL size: $Path" }
    $bytes = [IO.File]::ReadAllBytes($file.FullName)
    try {
        if ([BitConverter]::ToUInt16($bytes,0) -ne 0x5a4d) { throw 'DOS signature' }
        $nt = [uint64][BitConverter]::ToUInt32($bytes,0x3c)
        if ($nt -lt 64 -or $nt + 24 -gt $bytes.Length) { throw 'NT header bounds' }
        if ([BitConverter]::ToUInt32($bytes,[int]$nt) -ne 0x4550 -or [BitConverter]::ToUInt16($bytes,[int]$nt+4) -ne 0x8664) { throw 'AMD64 PE signature' }
        if (([BitConverter]::ToUInt16($bytes,[int]$nt+22) -band 0x2000) -eq 0) { throw 'Not a DLL' }
        $count = [BitConverter]::ToUInt16($bytes,[int]$nt+6)
        $optionalSize = [BitConverter]::ToUInt16($bytes,[int]$nt+20)
        $optional = $nt + 24
        $table = $optional + $optionalSize
        if ($count -lt 1 -or $count -gt 96 -or $optionalSize -lt 112 -or $table + $count * 40 -gt $bytes.Length) { throw 'Optional header/section table bounds' }
        if ([BitConverter]::ToUInt16($bytes,[int]$optional) -ne 0x20b) { throw 'Expected PE32+' }
        $imageSize = [uint64][BitConverter]::ToUInt32($bytes,[int]$optional+56)
        $headerSize = [uint64][BitConverter]::ToUInt32($bytes,[int]$optional+60)
        if ($imageSize -eq 0 -or $imageSize -gt 536870912 -or $headerSize -lt $table + $count * 40 -or $headerSize -gt $imageSize -or $headerSize -gt $bytes.Length) { throw 'Image/header size' }
        $sections = @()
        for ($i=0; $i -lt $count; $i++) {
            $row = [int]($table + $i*40)
            $virtualSize = [uint64][BitConverter]::ToUInt32($bytes,$row+8)
            $rva = [uint64][BitConverter]::ToUInt32($bytes,$row+12)
            $rawSize = [uint64][BitConverter]::ToUInt32($bytes,$row+16)
            $raw = [uint64][BitConverter]::ToUInt32($bytes,$row+20)
            $size = [Math]::Max($virtualSize,$rawSize)
            if ($rva + $size -gt $imageSize -or ($size -gt 0 -and $rva -lt $headerSize)) { throw 'Virtual section bounds' }
            if ($rawSize -gt 0 -and ($raw -lt $headerSize -or $raw + $rawSize -gt $bytes.Length)) { throw 'Raw section bounds' }
            foreach ($prior in $sections) {
                if ($size -gt 0 -and $prior.Size -gt 0 -and $rva -lt $prior.Rva+$prior.Size -and $prior.Rva -lt $rva+$size) { throw 'Overlapping virtual sections' }
                if ($rawSize -gt 0 -and $prior.RawSize -gt 0 -and $raw -lt $prior.Raw+$prior.RawSize -and $prior.Raw -lt $raw+$rawSize) { throw 'Overlapping raw sections' }
            }
            $sections += @{ Rva=$rva; Size=$size; Raw=$raw; RawSize=$rawSize }
        }
    } catch { throw "Invalid AMD64 PE DLL '$Path': $($_.Exception.Message)" }
    return [pscustomobject]@{ Path=$file.FullName; ProductName=[Diagnostics.FileVersionInfo]::GetVersionInfo($file.FullName).ProductName; Hash=(Get-HlaFileHash $file.FullName) }
}

function Assert-HlaGameClosed {
    if (@(Get-Process -Name hlvr -ErrorAction SilentlyContinue).Count -gt 0) { throw 'Half-Life: Alyx (hlvr.exe) is running. Close the game before installing or uninstalling.' }
}

function ConvertFrom-HlaVdfString([string]$Token) {
    $text=$Token.Substring(1,$Token.Length-2)
    $result=New-Object Text.StringBuilder
    for ($i=0; $i -lt $text.Length; $i++) {
        if ($text[$i] -eq '\' -and $i+1 -lt $text.Length -and ($text[$i+1] -eq '\' -or $text[$i+1] -eq '"')) { $i++ }
        [void]$result.Append($text[$i])
    }
    return $result.ToString()
}

function Read-HlaVdfObject($State,[bool]$Nested) {
    $result=@{}
    while ($State.Index -lt $State.Tokens.Count) {
        $token=$State.Tokens[$State.Index]; $State.Index++
        if ($token -eq '}') { if ($Nested) { return $result }; throw 'Unexpected VDF closing brace' }
        if (-not $token.StartsWith('"')) { throw 'Expected quoted VDF key' }
        $key=ConvertFrom-HlaVdfString $token
        if ($result.ContainsKey($key) -or $State.Index -ge $State.Tokens.Count) { throw 'Duplicate/missing VDF value' }
        $value=$State.Tokens[$State.Index]; $State.Index++
        if ($value -eq '{') { $result[$key]=Read-HlaVdfObject $State $true }
        elseif ($value.StartsWith('"')) { $result[$key]=ConvertFrom-HlaVdfString $value }
        else { throw 'Invalid VDF value' }
    }
    if ($Nested) { throw 'Unclosed VDF object' }
    return $result
}

function Read-HlaVdf([string]$Path) {
    $text=[IO.File]::ReadAllText($Path)
    $tokens=New-Object 'Collections.Generic.List[string]'
    $position=0
    foreach ($match in [regex]::Matches($text,'"(?:\\.|[^"\\])*"|[{}]|//[^\r\n]*')) {
        if ($text.Substring($position,$match.Index-$position).Trim().Length -ne 0) { throw "Invalid VDF syntax: $Path" }
        if (-not $match.Value.StartsWith('//')) { $tokens.Add($match.Value) }
        $position=$match.Index+$match.Length
    }
    if ($text.Substring($position).Trim().Length -ne 0) { throw "Invalid trailing VDF syntax: $Path" }
    return Read-HlaVdfObject @{ Tokens=$tokens; Index=0 } $false
}

function Get-HlaSteamLibraries([string]$SteamRoot) {
    $libraries=New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    [void]$libraries.Add([IO.Path]::GetFullPath($SteamRoot))
    $file=Join-Path $SteamRoot 'steamapps\libraryfolders.vdf'
    if (Test-Path -LiteralPath $file -PathType Leaf) {
        $vdf=Read-HlaVdf $file
        if ($vdf.ContainsKey('libraryfolders')) {
            foreach ($key in $vdf.libraryfolders.Keys) {
                if ($key -notmatch '^\d+$') { continue }
                $entry=$vdf.libraryfolders[$key]
                $library=$null
                if ($entry -is [string]) { $library=$entry }
                elseif ($entry -is [hashtable] -and $entry.ContainsKey('path')) { $library=$entry.path }
                if ($library -is [string] -and [IO.Path]::IsPathRooted($library)) { [void]$libraries.Add([IO.Path]::GetFullPath($library)) }
            }
        }
    }
    foreach ($library in $libraries) { Write-Output $library }
}

function Test-HlaGameBin([string]$Path) {
    return (Test-Path -LiteralPath (Join-Path $Path 'hlvr.exe') -PathType Leaf) -and (Test-Path -LiteralPath (Join-Path $Path 'vr.dll') -PathType Leaf)
}

function Get-HlaGameBinsFromLibraries([string[]]$Libraries) {
    foreach ($library in $Libraries) {
        $manifest=Join-Path $library 'steamapps\appmanifest_546560.acf'
        if (-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { continue }
        try {
            $vdf=Read-HlaVdf $manifest
            if (-not $vdf.ContainsKey('AppState') -or -not $vdf.AppState.ContainsKey('installdir')) { continue }
            $directory=$vdf.AppState.installdir
            if ($directory -isnot [string] -or -not $directory -or [IO.Path]::IsPathRooted($directory) -or @($directory -split '[/\\]' | Where-Object { $_ -eq '..' -or $_ -eq '.' }).Count -gt 0) { continue }
            $common=[IO.Path]::GetFullPath((Join-Path $library 'steamapps\common'))
            $game=[IO.Path]::GetFullPath((Join-Path $common $directory))
            if (-not $game.StartsWith($common.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { continue }
            $bin=Join-Path $game 'game\bin\win64'
            if (Test-HlaGameBin $bin) { Write-Output $bin }
        } catch { Write-Warning "Cannot inspect Steam manifest '$manifest': $($_.Exception.Message)" }
    }
}

function Resolve-HlaGameBin([string]$GamePath) {
    if ($GamePath) {
        $path=[IO.Path]::GetFullPath($GamePath)
        foreach ($candidate in @($path,(Join-Path $path 'game\bin\win64'),(Join-Path $path 'bin\win64'))) {
            if (Test-HlaGameBin $candidate) { return [IO.Path]::GetFullPath($candidate) }
        }
        throw "Invalid Half-Life: Alyx game path '$GamePath'. Expected the game root or game\bin\win64 containing hlvr.exe and vr.dll."
    }
    $libraries=New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($registry in @(@{Key='HKCU:\Software\Valve\Steam';Value='SteamPath'},@{Key='HKLM:\SOFTWARE\WOW6432Node\Valve\Steam';Value='InstallPath'},@{Key='HKLM:\SOFTWARE\Valve\Steam';Value='InstallPath'})) {
        $properties=Get-ItemProperty -LiteralPath $registry.Key -ErrorAction SilentlyContinue
        if ($null -ne $properties -and $null -ne $properties.PSObject.Properties[$registry.Value]) {
            $steam=[string]$properties.($registry.Value)
            if ($steam -and (Test-Path -LiteralPath $steam -PathType Container)) { foreach ($library in Get-HlaSteamLibraries $steam) { [void]$libraries.Add($library) } }
        }
    }
    $bins=@(Get-HlaGameBinsFromLibraries @($libraries))
    if ($bins.Count -eq 1) { return $bins[0] }
    if ($bins.Count -gt 1) { throw 'Multiple Half-Life: Alyx installations found. Specify -GamePath explicitly.' }
    throw 'Half-Life: Alyx was not found in Steam libraries. Specify -GamePath with the game root or game\bin\win64.'
}

function Assert-HlaOriginalBackup([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Original backup is missing: $Path. Use Steam Verify integrity of game files before reinstalling; do not delete your backup files." }
    $info=Get-HlaPeInfo $Path
    if ($info.ProductName -ceq $script:HlaProductName) { throw "Original backup is another HLA FTP Patch proxy: $Path. Restore an original via Steam Verify integrity of game files." }
    return $info
}

function Invoke-HlaCompatibilityCheck([string]$PackagePath) {
    $checker=Join-Path $PackagePath 'ftp_check.exe'
    if (-not (Test-Path -LiteralPath $checker -PathType Leaf)) { throw "Package checker is missing: $checker. Extract the complete release ZIP." }
    Write-Host 'Checking installed SteamVR compatibility (read only)...'
    & $checker
    if ($LASTEXITCODE -ne 0) { throw "SteamVR compatibility checker failed (exit $LASTEXITCODE). Installation was not changed. Check the runtime installation or use a compatible patch release." }
}

function New-HlaTemporaryPath([string]$Directory,[string]$Name) { return Join-Path $Directory ($Name+'.hlaftp-'+[guid]::NewGuid().ToString('N')) }
function Remove-HlaTemporaryFile([string]$Path) {
    if ($Path -and (Test-Path -LiteralPath $Path -PathType Leaf)) {
        try { Remove-Item -LiteralPath $Path -Force } catch { Write-Warning "Temporary file could not be removed: $Path" }
    }
}

function Invoke-HlaInstallTransaction([string]$Source,[string]$Current,[string]$Backup,$CurrentInfo,$BackupInfo,[bool]$Updating,$SourceInfo) {
    $directory=Split-Path -Parent $Current
    $stage=New-HlaTemporaryPath $directory 'openvr_api.dll'
    $backupStage=New-HlaTemporaryPath $directory 'openvr_api_ftp_original.dll'
    $rollback=New-HlaTemporaryPath $directory 'openvr_api.dll.rollback'
    $archive=$null; $backupCreated=$false; $complete=$false
    try {
        Copy-Item -LiteralPath $Source -Destination $stage
        if ((Get-HlaFileHash $stage) -ne $SourceInfo.Hash -or (Get-HlaFileHash $Source) -ne $SourceInfo.Hash) { throw 'Package changed after validation; copy verification failed' }
        if (-not $Updating) {
            Copy-Item -LiteralPath $Current -Destination $backupStage
            if ((Get-HlaFileHash $backupStage) -ne $CurrentInfo.Hash) { throw 'Original changed while preparing backup' }
        }
        if ((Get-HlaFileHash $Current) -ne $CurrentInfo.Hash) { throw 'Current DLL changed during installation; retry after closing Steam/game updates' }
        if ($null -ne $BackupInfo) {
            if ((Get-HlaFileHash $Backup) -ne $BackupInfo.Hash) { throw 'Backup changed during installation' }
        } elseif (Test-Path -LiteralPath $Backup) { throw 'A backup appeared during installation; retry' }
        if (-not $Updating) {
            if ($null -ne $BackupInfo) {
                $archive=$Backup+'.archived-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')+'-'+[guid]::NewGuid().ToString('N')
                [IO.File]::Move($Backup,$archive)
            }
            [IO.File]::Move($backupStage,$Backup); $backupCreated=$true
        }
        [IO.File]::Replace($stage,$Current,$rollback,$true)
        $complete=$true
    } catch {
        $failure=$_.Exception.Message
        try {
            if (Test-Path -LiteralPath $rollback -PathType Leaf) {
                if (Test-Path -LiteralPath $Current -PathType Leaf) { [IO.File]::Replace($rollback,$Current,$null,$true) }
                else { [IO.File]::Move($rollback,$Current) }
            }
            if (-not (Test-Path -LiteralPath $Current -PathType Leaf) -or (Get-HlaFileHash $Current) -ne $CurrentInfo.Hash) { throw 'Current DLL could not be confirmed restored; preserving all recovery files' }
            if ($backupCreated) {
                if ((Get-HlaFileHash $Backup) -ne $CurrentInfo.Hash) { throw 'Backup changed during rollback; preserving recovery files' }
                Remove-Item -LiteralPath $Backup -Force
            }
            if ($archive -and (Test-Path -LiteralPath $archive -PathType Leaf)) { [IO.File]::Move($archive,$Backup) }
        } catch { throw "Installation transaction failed: $failure. Rollback also failed: $($_.Exception.Message). Recovery files were preserved in $directory; do not delete backup/archive/rollback files." }
        throw "Installation transaction failed; original files restored: $failure"
    } finally {
        Remove-HlaTemporaryFile $stage; Remove-HlaTemporaryFile $backupStage
        if ($complete) { Remove-HlaTemporaryFile $rollback }
    }
    if ($archive) { Write-Host "Archived previous backup: $archive" }
}

function Invoke-HlaUninstallTransaction([string]$Current,[string]$Backup,$CurrentInfo,$BackupInfo) {
    $directory=Split-Path -Parent $Current
    $stage=New-HlaTemporaryPath $directory 'openvr_api.dll'
    $rollback=New-HlaTemporaryPath $directory 'openvr_api.dll.rollback'
    $complete=$false
    try {
        Copy-Item -LiteralPath $Backup -Destination $stage
        if ((Get-HlaFileHash $stage) -ne $BackupInfo.Hash -or (Get-HlaFileHash $Backup) -ne $BackupInfo.Hash -or (Get-HlaFileHash $Current) -ne $CurrentInfo.Hash) { throw 'Files changed during uninstall; retry after closing Steam/game updates' }
        [IO.File]::Replace($stage,$Current,$rollback,$true)
        Remove-Item -LiteralPath $Backup -Force
        $complete=$true
    } catch {
        $failure=$_.Exception.Message
        try {
            if (Test-Path -LiteralPath $rollback -PathType Leaf) {
                if (Test-Path -LiteralPath $Current -PathType Leaf) { [IO.File]::Replace($rollback,$Current,$null,$true) }
                else { [IO.File]::Move($rollback,$Current) }
            }
        } catch { throw "Uninstall transaction failed: $failure. Rollback failed: $($_.Exception.Message). Preserve recovery files in $directory." }
        throw "Uninstall transaction failed; installed proxy and backup preserved: $failure"
    } finally {
        Remove-HlaTemporaryFile $stage
        if ($complete) { Remove-HlaTemporaryFile $rollback }
    }
}
