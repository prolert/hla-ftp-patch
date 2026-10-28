$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$root = Join-Path ([IO.Path]::GetTempPath()) ('hla-ftp-launchers-' + [guid]::NewGuid().ToString('N'))
$utf8 = New-Object Text.UTF8Encoding($false)
$cyrillic = -join ([char[]](0x418,0x433,0x440,0x44b))
$gamePath = 'D:\' + $cyrillic + ' & 50% [A]!bang ($dollar);''quote`tick\Half-Life Alyx'
$packagePath = 'C:\Package & 50% [B]!bang ' + $cyrillic + '\'
$failed = 0
$passed = 0
# Only the child script is a fixture: launch the actual shipped batch wrappers.
$sentinel = @'
param([string]$GamePath,[string]$PackagePath)
$record = @{ ScriptRoot=$PSScriptRoot; GamePath=$GamePath; PackagePath=$PackagePath; ExtraArguments=@($args) } | ConvertTo-Json -Compress
[IO.File]::WriteAllText($env:HLA_LAUNCHER_RECORD,$record,(New-Object Text.UTF8Encoding($false)))
exit [int]$env:HLA_LAUNCHER_EXIT
'@
$cases = @(
    @{Name='no arguments'; Arguments=''; Game=''; Package=''; Valid=$true},
    @{Name='named Unicode path'; Arguments=('-GamePath "'+$gamePath+'"'); Game=$gamePath; Package=''; Valid=$true},
    @{Name='positional Unicode path'; Arguments=('"'+$gamePath+'"'); Game=$gamePath; Package=''; Valid=$true},
    @{Name='trailing game separator'; Arguments=('-GamePath "'+$gamePath+'\"'); Game=($gamePath+'\'); Package=''; Valid=$true},
    @{Name='C drive root'; Arguments='-GamePath "C:\"'; Game='C:\'; Package=''; Valid=$true},
    @{Name='D drive root'; Arguments='-GamePath "D:\"'; Game='D:\'; Package=''; Valid=$true},
    @{Name='unknown option'; Arguments='-Unknown value'; Game=''; Package=''; Valid=$false},
    @{Name='missing game value'; Arguments='-GamePath'; Game=''; Package=''; Valid=$false},
    @{Name='empty game value'; Arguments='-GamePath ""'; Game=''; Package=''; Valid=$false},
    @{Name='empty positional value'; Arguments='""'; Game=''; Package=''; Valid=$false},
    @{Name='missing game before option'; Arguments='-GamePath -GamePath'; Game=''; Package=''; Valid=$false},
    @{Name='duplicate game option'; Arguments='-GamePath C:\first -GamePath D:\second'; Game=''; Package=''; Valid=$false},
    @{Name='extra positional argument'; Arguments='C:\first D:\second'; Game=''; Package=''; Valid=$false}
)
try {
    [void][IO.Directory]::CreateDirectory($root)
    foreach ($launcher in @('install','uninstall')) {
        $launcherCases = @($cases)
        if ($launcher -eq 'install') {
            $launcherCases += @{Name='named package trailing separator'; Arguments=('-PackagePath "'+$packagePath+'" -GamePath "'+$gamePath+'\"'); Game=($gamePath+'\'); Package=$packagePath; Valid=$true}
            $launcherCases += @{Name='missing package value'; Arguments='-PackagePath'; Game=''; Package=''; Valid=$false}
            $launcherCases += @{Name='missing package before game option'; Arguments='-PackagePath -GamePath C:\game'; Game=''; Package=''; Valid=$false}
            $launcherCases += @{Name='duplicate package option'; Arguments='-PackagePath C:\first -PackagePath D:\second'; Game=''; Package=''; Valid=$false}
        } else {
            $launcherCases += @{Name='unsupported package option'; Arguments='-PackagePath C:\package'; Game=''; Package=''; Valid=$false}
        }
        foreach ($delayed in @('ON','OFF')) {
            $package = Join-Path $root ($launcher + '-' + $delayed + ' ! [A] & 50% ' + $cyrillic)
            [void][IO.Directory]::CreateDirectory($package)
            Copy-Item -LiteralPath (Join-Path $repo ('scripts\' + $launcher + '.bat')) -Destination (Join-Path $package ($launcher + '.bat'))
            [IO.File]::WriteAllText((Join-Path $package ($launcher + '.ps1')),$sentinel,$utf8)
            foreach ($case in $launcherCases) {
            $recordPath = Join-Path $root ([guid]::NewGuid().ToString('N') + '.json')
            $start = New-Object Diagnostics.ProcessStartInfo
            $start.FileName = Join-Path $env:SystemRoot 'System32\cmd.exe'
            $start.WorkingDirectory = $package
            # Delayed expansion of this variable preserves literal ! characters in
            # its value before the wrapper starts. A literal ! in a /V:ON command
            # can instead be consumed by the caller, outside the wrapper's control.
            if ($delayed -eq 'ON') { $arguments = '!HLA_LAUNCHER_ARGUMENTS!' }
            else { $arguments = '%HLA_LAUNCHER_ARGUMENTS%' }
            if (-not $case.Arguments) { $arguments = '' }
            $start.Arguments = '/D /V:' + $delayed + ' /C "' + $launcher + '.bat ' + $arguments + ' <nul"'
            $start.UseShellExecute = $false
            $start.CreateNoWindow = $true
            $start.RedirectStandardOutput = $true
            $start.RedirectStandardError = $true
            $start.EnvironmentVariables['HLA_LAUNCHER_ARGUMENTS'] = $case.Arguments
            $start.EnvironmentVariables['HLA_LAUNCHER_RECORD'] = $recordPath
            $start.EnvironmentVariables['HLA_LAUNCHER_EXIT'] = '37'
            $process = [Diagnostics.Process]::Start($start)
            try {
                $output = $process.StandardOutput.ReadToEnd() + $process.StandardError.ReadToEnd()
                $process.WaitForExit()
                if (-not $case.Valid) {
                    if ([IO.File]::Exists($recordPath)) { throw 'Invalid arguments reached the backend script' }
                    if ($process.ExitCode -eq 0) { throw 'Invalid arguments returned success' }
                    $passed++
                    Write-Host "PASS $launcher cmd /V:${delayed}: $($case.Name)"
                    continue
                }
                if (-not [IO.File]::Exists($recordPath)) { throw "Child script was not reached (exit $($process.ExitCode)): $output" }
                $record = [IO.File]::ReadAllText($recordPath) | ConvertFrom-Json
                if ($record.ScriptRoot -cne $package) { throw "Script path changed: $($record.ScriptRoot)" }
                if ($record.GamePath -cne $case.Game -or $record.PackagePath -cne $case.Package -or @($record.ExtraArguments).Count -ne 0) { throw "Arguments changed: GamePath=<$($record.GamePath)> PackagePath=<$($record.PackagePath)>" }
                if ($process.ExitCode -ne 37) { throw "Child exit 37 was changed to $($process.ExitCode)" }
                $passed++
                Write-Host "PASS $launcher cmd /V:${delayed}: $($case.Name)"
            } catch {
                $failed++
                Write-Host "FAIL $launcher cmd /V:${delayed}: $($case.Name): $($_.Exception.Message)"
            } finally { $process.Dispose() }
            }
        }
    }
    if ($failed -gt 0) { throw "$failed launcher tests failed; $passed passed." }
    Write-Host "$passed launcher tests passed."
} finally {
    if ([IO.Directory]::Exists($root)) { [IO.Directory]::Delete($root,$true) }
}
