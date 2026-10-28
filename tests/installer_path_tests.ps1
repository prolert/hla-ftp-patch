param(
    [string]$Repo = (Split-Path -Parent $PSScriptRoot),
    [string]$Proxy = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\openvr_api.dll'),
    [string]$Original = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\openvr_api_ftp_original.dll'),
    [string]$Checker = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\Release\ftp_check.exe')
)
$ErrorActionPreference = 'Stop'
$utf8 = New-Object Text.UTF8Encoding($false)
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$identifier = [guid]::NewGuid().ToString('N')
$cRoot = Join-Path ([IO.Path]::GetTempPath()) ('hla-ftp-path-audit-' + $identifier)
$dRoot = 'D:\hla-ftp-path-audit-' + $identifier
$createdRoots = New-Object 'Collections.Generic.List[string]'
$results = New-Object 'Collections.Generic.List[object]'
$savedConfig = $env:HLA_AUDIT_CONFIG_FILE
$unicode = -join [char[]]@(0x418,0x433,0x440,0x44b,0x20,0x410,0x43b,0x438,0x43a,0x441)
$special = $unicode + ' [Quest 2] & 100% (VR)!'
function Require($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Hash([string]$Path) { return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Write-Text([string]$Path, [string]$Text) { [IO.File]::WriteAllText($Path, $Text, $utf8) }
function New-Game([string]$Directory) {
    $bin = Join-Path $Directory 'game\bin\win64'
    New-Item -ItemType Directory -Path $bin -Force | Out-Null
    Write-Text (Join-Path $bin 'hlvr.exe') 'controlled fixture'
    Write-Text (Join-Path $bin 'vr.dll') 'controlled fixture'
    Copy-Item -LiteralPath $Original -Destination (Join-Path $bin 'openvr_api.dll')
    return $bin
}
function Run-Production([string]$Operation, [string]$GamePath, [string]$SteamRoot = '', [int]$Expected = 0) {
    $config = @{ Scripts = (Join-Path $Repo 'scripts'); Operation = $Operation; GamePath = $GamePath; SteamRoot = $SteamRoot; Package = $package; Local = $local }
    Write-Text $configFile ($config | ConvertTo-Json -Compress)
    $env:HLA_AUDIT_CONFIG_FILE = $configFile
    $previous = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = (& $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $runner 2>&1 | Out-String)
        $exitCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previous }
    Require ($exitCode -eq $Expected) "$Operation exit $exitCode, expected $Expected. $output"
    return $output
}
function Run-BatchProduction([string]$Operation, [string]$GamePath) {
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = Join-Path $env:SystemRoot 'System32\cmd.exe'
    $start.WorkingDirectory = $cRoot
    $start.Arguments = '/D /V:OFF /C ""%HLA_AUDIT_BATCH_PATH%" -GamePath "%HLA_AUDIT_GAME_PATH%" <nul"'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables['HLA_AUDIT_BATCH_PATH'] = Join-Path $package ($Operation + '.bat')
    $start.EnvironmentVariables['HLA_AUDIT_GAME_PATH'] = $GamePath
    $start.EnvironmentVariables['LOCALAPPDATA'] = $local
    $process = [Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEnd() + $process.StandardError.ReadToEnd()
        $process.WaitForExit()
        Require ($process.ExitCode -eq 0) "Batch $Operation exit $($process.ExitCode): $output"
    } finally { $process.Dispose() }
}
function Roundtrip([string]$Name, [string]$Root, [string]$Argument, [string]$SteamRoot = '', [switch]$Batch) {
    $bin = Join-Path $Root 'game\bin\win64'
    $current = Join-Path $bin 'openvr_api.dll'
    $backup = Join-Path $bin 'openvr_api_ftp_original.dll'
    $before = Hash $current
    if ($Batch) { Run-BatchProduction install $Argument }
    else { [void](Run-Production install $Argument $SteamRoot) }
    Require ((Hash $current) -eq (Hash $Proxy)) "${Name}: proxy missing"
    Require ((Hash $backup) -eq $before) "${Name}: original backup wrong"
    if ($Batch) { Run-BatchProduction uninstall $Argument }
    else { [void](Run-Production uninstall $Argument $SteamRoot) }
    Require ((Hash $current) -eq $before) "${Name}: original not restored"
    Require (-not (Test-Path -LiteralPath $backup)) "${Name}: backup not consumed"
    $results.Add(@{name=$Name;result='passed'})
    Write-Host "PASS $Name"
}
try {
    foreach ($fixture in @($Proxy,$Original,$Checker)) { Require (Test-Path -LiteralPath $fixture -PathType Leaf) "Missing fixture: $fixture" }
    New-Item -ItemType Directory -Path $cRoot | Out-Null
    $createdRoots.Add($cRoot)
    if (Test-Path -LiteralPath 'D:\' -PathType Container) {
        try {
            New-Item -ItemType Directory -Path $dRoot | Out-Null
            $createdRoots.Add($dRoot)
        } catch [UnauthorizedAccessException] {
            $dRoot = $null
            Write-Host 'SKIP physical D: matrix (drive root is not writable)'
        }
    } else { $dRoot = $null; Write-Host 'SKIP physical D: matrix (drive not present)' }
    $runner = Join-Path $cRoot 'run-production.ps1'
    $configFile = Join-Path $cRoot 'case.json'
    $runnerText = @'
$ErrorActionPreference = 'Stop'
$config = [IO.File]::ReadAllText($env:HLA_AUDIT_CONFIG_FILE) | ConvertFrom-Json
$env:LOCALAPPDATA = $config.Local
if ($config.SteamRoot) {
    $global:HlaAuditSteamRoot = $config.SteamRoot
    function global:Get-ItemProperty {
        [CmdletBinding()]param([string]$LiteralPath)
        if ($LiteralPath -eq 'HKCU:\Software\Valve\Steam') {
            return [pscustomobject]@{SteamPath=$global:HlaAuditSteamRoot}
        }
        return $null
    }
}
$arguments = @{}
if ($config.GamePath) { $arguments.GamePath = $config.GamePath }
if ($config.Operation -eq 'install') { $arguments.PackagePath = $config.Package }
& (Join-Path $config.Scripts ($config.Operation + '.ps1')) @arguments
exit $LASTEXITCODE
'@
    Write-Text $runner $runnerText
    $package = Join-Path $cRoot ('package ' + $special)
    $local = Join-Path $cRoot ('profile ' + $special)
    $runtimeBase = if ($dRoot) { $dRoot } else { $cRoot }
    $runtime = Join-Path $runtimeBase ('runtime ' + $special)
    New-Item -ItemType Directory -Path $package,(Join-Path $local 'openvr'),(Join-Path $runtime 'bin') -Force | Out-Null
    Copy-Item -LiteralPath $Proxy -Destination (Join-Path $package 'openvr_api.dll')
    Copy-Item -LiteralPath $Checker -Destination (Join-Path $package 'ftp_check.exe')
    foreach ($script in @('install.ps1','uninstall.ps1','common.ps1','install.bat','uninstall.bat')) {
        Copy-Item -LiteralPath (Join-Path $Repo ('scripts\' + $script)) -Destination (Join-Path $package $script)
    }
    Copy-Item -LiteralPath $Original -Destination (Join-Path $runtime 'bin\vrclient_x64.dll')
    Write-Text (Join-Path $local 'openvr\openvrpaths.vrpath') (@{runtime=@($runtime)} | ConvertTo-Json -Compress)

    $gameC = Join-Path $cRoot ('manual ' + $special)
    [void](New-Game $gameC)
    Roundtrip 'C: Cyrillic/spaces/brackets/ampersand/percent/parentheses/exclamation' $gameC $gameC
    Roundtrip 'C: explicit bin path with trailing separator' $gameC ((Join-Path $gameC 'game\bin\win64') + '\')
    Roundtrip 'C: explicit game subdirectory' $gameC (Join-Path $gameC 'game')
    Roundtrip 'C: actual batch install/uninstall, arbitrary working directory and trailing separator' $gameC ($gameC + '\') -Batch
    if ($dRoot) {
        $gameD = Join-Path $dRoot ('manual ' + $special)
        [void](New-Game $gameD)
        Roundtrip 'D: Cyrillic/special paths with C: package and profile' $gameD $gameD
        Roundtrip 'D: explicit bin path with trailing separator' $gameD ((Join-Path $gameD 'game\bin\win64') + '\')
        Roundtrip 'D: actual batch install/uninstall with Cyrillic/special package and trailing separator' $gameD ((Join-Path $gameD 'game\bin\win64') + '\') -Batch
    }

    $steam = Join-Path $cRoot ('Steam ' + $special)
    $libraryC = Join-Path $cRoot ('library ' + $special)
    $libraryD = Join-Path $runtimeBase ('second library ' + $special)
    New-Item -ItemType Directory -Path (Join-Path $steam 'steamapps'),(Join-Path $libraryC 'steamapps'),(Join-Path $libraryD 'steamapps') -Force | Out-Null
    $otherGame = Join-Path $libraryC 'steamapps\common\Unrelated VR Game\game\bin\win64'
    New-Item -ItemType Directory -Path $otherGame -Force | Out-Null
    Copy-Item -LiteralPath $Original -Destination (Join-Path $otherGame 'openvr_api.dll')
    Write-Text (Join-Path $otherGame 'vr.dll') 'other game fixture'
    $otherHash = Hash (Join-Path $otherGame 'openvr_api.dll')
    $installDir = $unicode + ' Custom Install [Alyx]'
    $discoveredGame = Join-Path $libraryD ('steamapps\common\' + $installDir)
    [void](New-Game $discoveredGame)
    $manifest = '"AppState" { "appid" "546560" "installdir" "' + $installDir + '" }'
    Write-Text (Join-Path $libraryD 'steamapps\appmanifest_546560.acf') $manifest
    $modern = '"libraryfolders" { "0" { "path" "' + $libraryC.Replace('\','\\') + '" "apps" { "123" "1" } } "1" { "path" "' + $libraryD.Replace('\','\\') + '" "apps" { "546560" "1" } } }'
    Write-Text (Join-Path $steam 'steamapps\libraryfolders.vdf') $modern
    Roundtrip 'Automatic discovery: Steam on C:, modern VDF, custom Cyrillic installdir in second library' $discoveredGame '' $steam
    $legacy = '"LibraryFolders" { "1" "' + $libraryD.Replace('\','\\') + '" }'
    Write-Text (Join-Path $steam 'steamapps\libraryfolders.vdf') $legacy
    Roundtrip 'Automatic discovery: legacy VDF and second-drive library' $discoveredGame '' $steam
    Require ((Hash (Join-Path $otherGame 'openvr_api.dll')) -eq $otherHash) 'Unrelated game was modified'
    [void](Run-Production install $otherGame '' 1)
    Require ((Hash (Join-Path $otherGame 'openvr_api.dll')) -eq $otherHash) 'Rejected wrong game was modified'
    $results.Add(@{name='Unrelated VR game rejected and unchanged';result='passed'})
    Write-Host 'PASS unrelated VR game rejected and unchanged'
    Write-Host "$($results.Count) end-to-end path scenarios passed; no real game installation or registry was changed"
} finally {
    $env:HLA_AUDIT_CONFIG_FILE = $savedConfig
    foreach ($directory in $createdRoots) {
        $resolved = [IO.Path]::GetFullPath($directory)
        if (-not (Split-Path -Leaf $resolved).StartsWith('hla-ftp-path-audit-')) { throw 'Unexpected cleanup target' }
        if ($resolved -ne [IO.Path]::GetFullPath($cRoot) -and ($null -eq $dRoot -or $resolved -ne [IO.Path]::GetFullPath($dRoot))) { throw 'Cleanup target is outside the two created test roots' }
        Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue
    }
}
