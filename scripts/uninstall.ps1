param([string]$GamePath)
$ErrorActionPreference='Stop'
try {
    . (Join-Path $PSScriptRoot 'common.ps1')
    Assert-HlaGameClosed
    $bin=Resolve-HlaGameBin $GamePath
    $current=Join-Path $bin 'openvr_api.dll'
    $backup=Join-Path $bin 'openvr_api_ftp_original.dll'
    $currentInfo=Get-HlaPeInfo $current
    if ($currentInfo.ProductName -cne $script:HlaProductName) { Write-Host 'HLA FTP Patch is not installed. Existing DLL and backup files were preserved.'; exit 0 }
    $backupInfo=Assert-HlaOriginalBackup $backup
    Assert-HlaGameClosed
    Invoke-HlaUninstallTransaction $current $backup $currentInfo $backupInfo
    Write-Host "Original openvr_api.dll restored: $current"
    Write-Host 'Archived backups and hla_ftp.log, if present, were preserved.'
    exit 0
} catch {
    [Console]::Error.WriteLine('Uninstall failed: '+$_.Exception.Message)
    exit 1
}
