param(
    [Parameter(Mandatory = $true)][string]$Checker,
    [string]$RealDll
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Checker -PathType Leaf)) { throw "Checker executable does not exist: $Checker" }
$Checker = (Resolve-Path -LiteralPath $Checker).Path
$root = Join-Path ([IO.Path]::GetTempPath()) ('hla-ftp-checker-' + [guid]::NewGuid().ToString('N'))
$savedLocal = $env:LOCALAPPDATA
$utf8 = New-Object Text.UTF8Encoding($false)
function Require($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Run-Checker([string[]]$Arguments, [int]$ExpectedCode, [string]$ExpectedText) {
    $output = (& $Checker @Arguments 2>&1 | Out-String)
    Require ($LASTEXITCODE -eq $ExpectedCode) "Exit $LASTEXITCODE, expected $ExpectedCode. Output: $output"
    Require ($output -match $ExpectedText) "Expected '$ExpectedText'. Output: $output"
    return $output
}
function U16([byte[]]$Bytes, [int]$Offset, [uint16]$Value) { [BitConverter]::GetBytes($Value).CopyTo($Bytes, $Offset) }
function U32([byte[]]$Bytes, [int]$Offset, [uint32]$Value) { [BitConverter]::GetBytes($Value).CopyTo($Bytes, $Offset) }
function New-Fixture([string]$Path, [string]$Kind) {
    $b = New-Object byte[] 0xc00
    U16 $b 0 0x5a4d; U32 $b 0x3c 0x80; U32 $b 0x80 0x4550
    U16 $b 0x84 0x8664; U16 $b 0x86 2; U16 $b 0x94 0xf0; U16 $b 0x98 0x20b
    U32 $b 0xb8 0x1000; U32 $b 0xbc 0x200; U32 $b 0xd0 0x3000; U32 $b 0xd4 0x400
    U32 $b 0x190 0x400; U32 $b 0x194 0x1000; U32 $b 0x198 0x400; U32 $b 0x19c 0x400; U32 $b 0x1ac 0x60000020
    U32 $b 0x1b8 0x400; U32 $b 0x1bc 0x2000; U32 $b 0x1c0 0x400; U32 $b 0x1c4 0x800; U32 $b 0x1d4 0x40000040
    if ($Kind -ne 'NotNeeded') {
        [Text.Encoding]::ASCII.GetBytes('Activation threshold of %f cannot be less than the deactivation threshold of %f in filter %s.').CopyTo($b, 0x801)
        ([byte[]](0x0f,0x2f,0xc8,0x0f,0x86,0xaa,0,0,0)).CopyTo($b,0x44d)
        ([byte[]](0x48,0x8d,0x0d,0x7a,0x0f,0,0)).CopyTo($b,0x480)
        if ($Kind -eq 'AlreadyPatched') { ([byte[]](0xe9,0xab,0,0,0,0x90)).CopyTo($b,0x450) }
        if ($Kind -eq 'PatternChanged') { $b[0x481] = 0x89 }
    }
    [IO.File]::WriteAllBytes($Path,$b)
}
try {
    New-Item -ItemType Directory -Path $root | Out-Null
    $env:LOCALAPPDATA = Join-Path $root 'local'
    $configDir = Join-Path $env:LOCALAPPDATA 'openvr'
    New-Item -ItemType Directory -Path $configDir -Force | Out-Null
    $config = Join-Path $configDir 'openvrpaths.vrpath'
    $runtime = Join-Path $root ('runtime space ' + [char]0x00e4 + [char]0x4e2d + [char]0xd83d + [char]0xde80)
    $bin = Join-Path $runtime 'bin'
    New-Item -ItemType Directory -Path $bin -Force | Out-Null
    $dll = Join-Path $bin 'vrclient_x64.dll'
    [void](Run-Checker @('--help') 0 'Usage:')
    [void](Run-Checker @('a','b') 1 'Usage:')
    [void](Run-Checker @((Join-Path $root 'missing.dll')) 1 'Error')
    [void](Run-Checker @() 1 'openvrpaths.vrpath')
    foreach ($kind in @('Found','AlreadyPatched','NotNeeded','PatternChanged')) {
        New-Fixture $dll $kind
        $hash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
        $code = if ($kind -eq 'PatternChanged') { 1 } else { 0 }
        $output = Run-Checker @($dll) $code $kind
        if ($kind -eq 'Found') { Require ($output -match 'RVA.*1050') 'Missing patch RVA'; Require ($output -match '0F 86 AA 00 00 00') 'Missing original bytes' }
        Require ((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash -eq $hash) 'Checker changed the DLL'
    }
    New-Fixture $dll 'Found'
    # ConvertTo-Json provides escaped backslashes; explicitly escape Unicode as well.
    $json = @{ runtime = @($runtime); ignored = @{ values = @($true, $null, 1.25e2) } } | ConvertTo-Json -Depth 4 -Compress
    $json = $json.Replace([string][char]0x00e4,'\u00e4').Replace([string][char]0x4e2d,'\u4e2d').Replace(([string][char]0xd83d + [char]0xde80),'\ud83d\ude80')
    [IO.File]::WriteAllText($config,$json,$utf8)
    $hash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
    $output = Run-Checker @() 0 'Found'
    Require ($output -match 'vrclient_x64.dll') 'Missing resolved runtime diagnostic'
    Require ((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash -eq $hash) 'Default check changed the DLL'
    $rawJson = @{ runtime = @($runtime) } | ConvertTo-Json -Compress
    [IO.File]::WriteAllText($config,$rawJson,$utf8)
    [void](Run-Checker @() 0 'Found')
    foreach ($bad in @('{','{}','{"runtime":[]}','{"runtime":[42]}','{"runtime":[""]}','{"runtime":"x"}','{"runtime":["x"],"x":tru}','{"runtime":["x"]} trailing','{"runtime":["\ud800"]}','{"runtime":["x"],"x":01}','{"runtime":["x",]}')) {
        [IO.File]::WriteAllText($config,$bad,$utf8)
        [void](Run-Checker @() 1 'Error')
    }
    [IO.File]::WriteAllBytes($config,[byte[]](0x7b,0x22,0xff,0x22,0x3a,0x31,0x7d))
    [void](Run-Checker @() 1 'Error')
    [IO.File]::WriteAllText($config,(' ' * (1024 * 1024 + 1)),$utf8)
    [void](Run-Checker @() 1 'exceeds permitted size')
    $large = Join-Path $root 'oversized.dll'
    $stream = [IO.File]::Create($large)
    try { $stream.SetLength(256 * 1024 * 1024 + 1) } finally { $stream.Dispose() }
    [void](Run-Checker @($large) 1 'exceeds permitted size')
    $invalid = Join-Path $root 'invalid.dll'
    [IO.File]::WriteAllText($invalid,'not a PE',$utf8)
    [void](Run-Checker @($invalid) 1 'Error')
    if ($RealDll) {
        $hash = (Get-FileHash -LiteralPath $RealDll -Algorithm SHA256).Hash
        [void](Run-Checker @($RealDll) 0 'Found|AlreadyPatched|NotNeeded')
        Require ((Get-FileHash -LiteralPath $RealDll -Algorithm SHA256).Hash -eq $hash) 'Real DLL changed'
    }
    Write-Host 'Checker tests passed.'
} finally {
    $env:LOCALAPPDATA = $savedLocal
    $resolvedRoot = [IO.Path]::GetFullPath($root)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ($resolvedRoot.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase) -and (Split-Path -Leaf $resolvedRoot).StartsWith('hla-ftp-checker-')) {
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
