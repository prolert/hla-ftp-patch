param(
    [string]$GamePath,
    [string]$PackagePath=$PSScriptRoot
)
$ErrorActionPreference='Stop'
try {
    . (Join-Path $PSScriptRoot 'common.ps1')
    Assert-HlaGameClosed
    $bin=Resolve-HlaGameBin $GamePath
    $package=[IO.Path]::GetFullPath($PackagePath)
    $source=Join-Path $package 'openvr_api.dll'
    $sourceInfo=Get-HlaPeInfo $source
    if ($sourceInfo.ProductName -cne $script:HlaProductName) { throw "Package openvr_api.dll ProductName must be exactly '$script:HlaProductName'. Extract the complete official release ZIP." }
    $current=Join-Path $bin 'openvr_api.dll'
    $backup=Join-Path $bin 'openvr_api_ftp_original.dll'
    $currentInfo=Get-HlaPeInfo $current
    $updating=$currentInfo.ProductName -ceq $script:HlaProductName
    $backupInfo=$null
    if ($updating -or (Test-Path -LiteralPath $backup)) { $backupInfo=Assert-HlaOriginalBackup $backup }
    Invoke-HlaCompatibilityCheck $package
    Assert-HlaGameClosed
    if (-not $updating) { Write-Warning 'The existing openvr_api.dll will be preserved as the original. Any other proxy remains in the forwarding chain; compatibility with other proxies must be checked in game.' }
    Invoke-HlaInstallTransaction $source $current $backup $currentInfo $backupInfo $updating $sourceInfo
    Write-Host "Installed HLA FTP Patch: $current"
    Write-Host "Original preserved: $backup"
    Write-Host 'Jump: /actions/move/in/ShiftDashJump; Grip as Button; activation 50, deactivation 100.'
    exit 0
} catch {
    [Console]::Error.WriteLine('Installation failed: '+$_.Exception.Message)
    exit 1
}
