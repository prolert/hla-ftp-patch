param(
    [Parameter(Mandatory=$true)][string]$Proxy,
    [Parameter(Mandatory=$true)][string]$Original,
    [Parameter(Mandatory=$true)][string]$Checker
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$scripts = Join-Path $repo 'scripts'
if (-not (Test-Path -LiteralPath (Join-Path $scripts 'common.ps1') -PathType Leaf)) { throw 'Missing installer implementation: scripts/common.ps1' }
foreach ($fixture in @($Proxy,$Original,$Checker)) { if (-not (Test-Path -LiteralPath $fixture -PathType Leaf)) { throw "Missing test fixture: $fixture" } }
$root = Join-Path ([IO.Path]::GetTempPath()) ('hla-ftp-installer-' + [guid]::NewGuid().ToString('N'))
$savedLocal = $env:LOCALAPPDATA
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$utf8 = New-Object Text.UTF8Encoding($false)
$script:passed = 0
function Require($Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Add-Marker([string]$Path,[string]$Text) {
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Append,[IO.FileAccess]::Write)
    try { $bytes=[Text.Encoding]::ASCII.GetBytes($Text); $stream.Write($bytes,0,$bytes.Length) } finally { $stream.Dispose() }
}
function New-Case([string]$Name) {
    $case = Join-Path $root $Name
    $bin = Join-Path $case 'game\bin\win64'
    $package = Join-Path $case 'package'
    New-Item -ItemType Directory -Path $bin,$package -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $bin 'hlvr.exe'),'sandbox placeholder',$utf8)
    [IO.File]::WriteAllText((Join-Path $bin 'vr.dll'),'sandbox placeholder',$utf8)
    Copy-Item -LiteralPath $Original -Destination (Join-Path $bin 'openvr_api.dll')
    Copy-Item -LiteralPath $Proxy -Destination (Join-Path $package 'openvr_api.dll')
    Copy-Item -LiteralPath $Checker -Destination (Join-Path $package 'ftp_check.exe')
    return @{ Root=$case; Bin=$bin; Package=$package; Current=(Join-Path $bin 'openvr_api.dll'); Backup=(Join-Path $bin 'openvr_api_ftp_original.dll') }
}
function Run-Script([string]$Name,$Case,[int]$Expected,[string]$ExpectedText='',[string]$GamePath='') {
    if (-not $GamePath) { $GamePath=$Case.Root }
    $previous=$ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        if ($Name -eq 'install') { $output=(& $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $scripts 'install.ps1') -GamePath $GamePath -PackagePath $Case.Package 2>&1 | Out-String) }
        else { $output=(& $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $scripts 'uninstall.ps1') -GamePath $GamePath 2>&1 | Out-String) }
        $code=$LASTEXITCODE
    } finally { $ErrorActionPreference=$previous }
    Require ($code -eq $Expected) "$Name exit $code expected $Expected. $output"
    if ($ExpectedText) { Require ($output -match $ExpectedText) "Expected '$ExpectedText': $output" }
    return $output
}
function Test([string]$Name,[scriptblock]$Body) { & $Body; $script:passed++; Write-Host "PASS $Name" }
function Unchanged($Case,[string]$Before) { Require ((Hash $Case.Current) -eq $Before) 'Current DLL changed after refused operation' }
try {
    New-Item -ItemType Directory -Path $root | Out-Null
    $env:LOCALAPPDATA=Join-Path $root 'local'
    $configDir=Join-Path $env:LOCALAPPDATA 'openvr'
    New-Item -ItemType Directory -Path $configDir -Force | Out-Null
    $config=Join-Path $configDir 'openvrpaths.vrpath'
    $runtime=Join-Path $root 'runtime'
    New-Item -ItemType Directory -Path (Join-Path $runtime 'bin') -Force | Out-Null
    Copy-Item -LiteralPath $Original -Destination (Join-Path $runtime 'bin\vrclient_x64.dll')
    $validConfig=@{runtime=@($runtime)} | ConvertTo-Json -Compress
    [IO.File]::WriteAllText($config,$validConfig,$utf8)

    Test 'first install preserves original and accepts game root' {
        $c=New-Case 'first'; $before=Hash $c.Current
        [void](Run-Script install $c 0 'Installed')
        Require ((Hash $c.Current) -eq (Hash $Proxy)) 'Proxy not installed'
        Require ((Hash $c.Backup) -eq $before) 'Original not backed up'
    }
    Test 'repeat install preserves original backup and accepts bin path' {
        $c=New-Case 'repeat'; [void](Run-Script install $c 0); $backup=Hash $c.Backup
        [void](Run-Script install $c 0 'Installed' $c.Bin)
        Require ((Hash $c.Backup) -eq $backup) 'Repeat install replaced original backup'
        Require (@(Get-ChildItem -LiteralPath $c.Bin -Filter '*.archived-*').Count -eq 0) 'Repeat archived backup'
    }
    Test 'uninstall restores original and repeated uninstall is harmless' {
        $c=New-Case 'uninstall'; $before=Hash $c.Current; [void](Run-Script install $c 0)
        [void](Run-Script uninstall $c 0 'restored')
        Unchanged $c $before; Require (-not (Test-Path -LiteralPath $c.Backup)) 'Backup remains after successful restore'
        [void](Run-Script uninstall $c 0 'not installed'); Unchanged $c $before
    }
    Test 'game update archives stale backup and preserves new original' {
        $c=New-Case 'update'; $old=Hash $c.Current; [void](Run-Script install $c 0)
        Copy-Item -LiteralPath $Original -Destination $c.Current -Force; Add-Marker $c.Current 'new game update'; $new=Hash $c.Current
        [void](Run-Script install $c 0 'Archived')
        Require ((Hash $c.Backup) -eq $new) 'New game original lost'
        $archives=@(Get-ChildItem -LiteralPath $c.Bin -Filter '*.archived-*')
        Require ($archives.Count -eq 1 -and (Hash $archives[0].FullName) -eq $old) 'Stale backup lost'
    }
    Test 'foreign proxy is preserved with warning' {
        $c=New-Case 'foreign'; Add-Marker $c.Current 'foreign proxy'; $before=Hash $c.Current
        [void](Run-Script install $c 0 'proxy|forwarding chain'); Require ((Hash $c.Backup) -eq $before) 'Foreign proxy lost'
    }
    Test 'missing backup refuses reinstall and uninstall' {
        $c=New-Case 'missing-backup'; Copy-Item -LiteralPath $Proxy -Destination $c.Current -Force; $before=Hash $c.Current
        [void](Run-Script install $c 1 'backup|integrity'); Unchanged $c $before
        [void](Run-Script uninstall $c 1 'backup|integrity'); Unchanged $c $before
    }
    Test 'corrupt backup refuses reinstall and uninstall' {
        $c=New-Case 'corrupt-backup'; [void](Run-Script install $c 0); [IO.File]::WriteAllText($c.Backup,'corrupt',$utf8); $before=Hash $c.Current
        [void](Run-Script install $c 1 'backup|PE'); Unchanged $c $before
        [void](Run-Script uninstall $c 1 'backup|PE'); Unchanged $c $before
        Require ([IO.File]::ReadAllText($c.Backup) -eq 'corrupt') 'Corrupt user backup deleted'
    }
    Test 'backup pointing to our proxy is refused' {
        $c=New-Case 'ours-backup'; Copy-Item -LiteralPath $Proxy -Destination $c.Backup; $before=Hash $c.Current
        [void](Run-Script install $c 1 'backup'); Unchanged $c $before
    }
    Test 'checker failure aborts before mutation' {
        $c=New-Case 'checker-fail'; $before=Hash $c.Current; [IO.File]::WriteAllText($config,'{}',$utf8)
        try { [void](Run-Script install $c 1 'compatibility|checker|runtime') } finally { [IO.File]::WriteAllText($config,$validConfig,$utf8) }
        Unchanged $c $before; Require (-not (Test-Path -LiteralPath $c.Backup)) 'Checker failure created backup'
    }
    Test 'missing package proxy and checker abort before mutation' {
        foreach ($name in @('openvr_api.dll','ftp_check.exe')) {
            $c=New-Case ('missing-'+$name); $before=Hash $c.Current; Remove-Item -LiteralPath (Join-Path $c.Package $name)
            [void](Run-Script install $c 1 'package|missing|exist'); Unchanged $c $before
        }
    }
    Test 'unmarked package and malformed current are refused' {
        $c=New-Case 'bad-package'; Copy-Item -LiteralPath $Original -Destination (Join-Path $c.Package 'openvr_api.dll') -Force; $before=Hash $c.Current
        [void](Run-Script install $c 1 'ProductName|package'); Unchanged $c $before
        $c=New-Case 'bad-current'; [IO.File]::WriteAllText($c.Current,'broken',$utf8); $before=Hash $c.Current
        [void](Run-Script install $c 1 'PE|DLL'); Unchanged $c $before
    }
    Test 'invalid game path is refused' {
        $c=New-Case 'wrong-game'; $before=Hash $c.Current; Remove-Item -LiteralPath (Join-Path $c.Bin 'vr.dll')
        [void](Run-Script install $c 1 'vr.dll|Half-Life|game path'); Unchanged $c $before
    }
    Test 'failed atomic replacement rolls back new and archived backups' {
        $c=New-Case 'locked'; Copy-Item -LiteralPath $Original -Destination $c.Backup; Add-Marker $c.Backup 'prior backup'; $before=Hash $c.Current; $backup=Hash $c.Backup
        $lock=[IO.File]::Open($c.Current,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
        try { [void](Run-Script install $c 1 'failed|transaction|process|access') } finally { $lock.Dispose() }
        Unchanged $c $before; Require ((Hash $c.Backup) -eq $backup) 'Failed transaction changed prior backup'
        Require (@(Get-ChildItem -LiteralPath $c.Bin -Filter '*.hlaftp-*').Count -eq 0) 'Failed transaction left stages'
    }
    Test 'uninstall failure preserves installed proxy and backup' {
        $c=New-Case 'uninstall-locked'; [void](Run-Script install $c 0); $before=Hash $c.Current; $backup=Hash $c.Backup
        $lock=[IO.File]::Open($c.Current,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
        try { [void](Run-Script uninstall $c 1 'failed|transaction|process|access') } finally { $lock.Dispose() }
        Unchanged $c $before; Require ((Hash $c.Backup) -eq $backup) 'Failed uninstall changed backup'
    }
    Test 'Steam library and installdir discovery supports modern and legacy VDF' {
        . (Join-Path $scripts 'common.ps1')
        $steam=Join-Path $root 'steam'; $library=Join-Path $root 'second library'; $legacy=Join-Path $root 'legacy library'
        New-Item -ItemType Directory -Path (Join-Path $steam 'steamapps'),(Join-Path $library 'steamapps\common\Custom Alyx\game\bin\win64'),(Join-Path $legacy 'steamapps') -Force | Out-Null
        $target=Join-Path $library 'steamapps\common\Custom Alyx\game\bin\win64'
        [IO.File]::WriteAllText((Join-Path $target 'hlvr.exe'),'placeholder',$utf8); [IO.File]::WriteAllText((Join-Path $target 'vr.dll'),'placeholder',$utf8)
        $vdf='"libraryfolders" { "0" { "path" "'+$steam.Replace('\','\\')+'" } "1" { "path" "'+$library.Replace('\','\\')+'" } "2" "'+$legacy.Replace('\','\\')+'" }'
        [IO.File]::WriteAllText((Join-Path $steam 'steamapps\libraryfolders.vdf'),$vdf,$utf8)
        [IO.File]::WriteAllText((Join-Path $library 'steamapps\appmanifest_546560.acf'),'"AppState" { "appid" "546560" "installdir" "Custom Alyx" }',$utf8)
        $libraries=@(Get-HlaSteamLibraries -SteamRoot $steam)
        Require ($libraries.Count -eq 3) 'Library discovery lost paths'
        $bins=@(Get-HlaGameBinsFromLibraries -Libraries $libraries)
        Require ($bins.Count -eq 1 -and $bins[0] -eq $target) 'Manifest installdir discovery failed'
        [IO.File]::WriteAllText((Join-Path $library 'steamapps\appmanifest_546560.acf'),'"AppState" { "installdir" "..\\outside" }',$utf8)
        Require (@(Get-HlaGameBinsFromLibraries -Libraries $libraries).Count -eq 0) 'Traversal manifest accepted'
    }
    Test 'package changed after validation is refused before mutation' {
        . (Join-Path $scripts 'common.ps1')
        $c=New-Case 'package-race'; $source=Join-Path $c.Package 'openvr_api.dll'; $sourceInfo=Get-HlaPeInfo $source; $currentInfo=Get-HlaPeInfo $c.Current; $before=Hash $c.Current
        Add-Marker $source 'changed after validation'; $caught=$false
        try { Invoke-HlaInstallTransaction $source $c.Current $c.Backup $currentInfo $null $false $sourceInfo } catch { $caught=$true }
        Require $caught 'Changed package was accepted'; Unchanged $c $before
        Require (-not (Test-Path -LiteralPath $c.Backup)) 'Changed package created backup'
    }
    Test 'running hlvr process blocks install and uninstall' {
        $c=New-Case 'running'; $before=Hash $c.Current; $runningExe=Join-Path $root 'hlvr.exe'
        Add-Type -TypeDefinition 'public static class HlaInstallerRunningFixture { public static void Main() { System.Threading.Thread.Sleep(60000); } }' -OutputAssembly $runningExe -OutputType ConsoleApplication
        $process=Start-Process -FilePath $runningExe -WindowStyle Hidden -PassThru
        try {
            [void](Run-Script install $c 1 'running|close'); [void](Run-Script uninstall $c 1 'running|close'); Unchanged $c $before
        } finally { if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force } }
    }
    Write-Host "$script:passed installer sandbox tests passed."
} finally {
    $env:LOCALAPPDATA=$savedLocal
    $resolved=[IO.Path]::GetFullPath($root); $temp=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ($resolved.StartsWith($temp,[StringComparison]::OrdinalIgnoreCase) -and (Split-Path -Leaf $resolved).StartsWith('hla-ftp-installer-')) { Remove-Item -LiteralPath $resolved -Recurse -Force -ErrorAction SilentlyContinue }
}
