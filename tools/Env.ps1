$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent

$Abis = 'arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86'

function Get-Setting([string]$Name) {
    $value = [Environment]::GetEnvironmentVariable($Name, 'Process')
    if (-not $value) { $value = [Environment]::GetEnvironmentVariable($Name, 'User') }
    if (-not $value) { $value = [Environment]::GetEnvironmentVariable($Name, 'Machine') }
    $value
}

# Paths that are this machine's own live in local.properties at the project root, which git
# leaves alone: sdk.dir, ndk.dir, cmake.dir, java.home, vs.dir.
function Get-Local([string]$Name) {
    $file = Join-Path $Root 'local.properties'
    if (-not (Test-Path $file)) { return }
    foreach ($line in Get-Content $file) {
        if ($line -match "^\s*$([regex]::Escape($Name))\s*=\s*(.+?)\s*$") { return $Matches[1] }
    }
}

function Find-Sdk {
    @((Get-Setting 'ANDROID_HOME'), (Get-Setting 'ANDROID_SDK_ROOT'), (Get-Local 'sdk.dir'), "$env:LOCALAPPDATA\Android\Sdk") | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}

function Find-Ndk {
    foreach ($name in 'ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT', 'NDK_HOME') {
        $value = Get-Setting $name
        if ($value -and (Test-Path $value)) { return $value }
    }
    $named = Get-Local 'ndk.dir'
    if ($named -and (Test-Path $named)) { return $named }
    $found = Get-ChildItem (Join-Path (Find-Sdk) 'ndk') -Directory -ErrorAction SilentlyContinue | Where-Object {
        $properties = Join-Path $_.FullName 'source.properties'
        (Test-Path $properties) -and -not (Select-String -Path $properties -Pattern 'Pkg\.Revision.*-(beta|rc)' -Quiet)
    }
    $best = $found | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if (-not $best) { throw 'Android NDK was not found; set ANDROID_NDK_HOME, or sdk.dir or ndk.dir in local.properties' }
    $best.FullName
}

# CMake and Ninja: the ones on PATH, or the pair the Android SDK ships.
function Find-CMake {
    $named = Get-Local 'cmake.dir'
    $folders = @($named) + @(Get-ChildItem (Join-Path (Find-Sdk) 'cmake') -Directory -ErrorAction SilentlyContinue | Sort-Object { [version]($_.Name -replace '[^\d.].*$') } -Descending | ForEach-Object { $_.FullName })
    foreach ($folder in $folders | Where-Object { $_ }) {
        $bin = Join-Path $folder 'bin'
        if ((Test-Path (Join-Path $bin 'cmake.exe')) -and (Test-Path (Join-Path $bin 'ninja.exe'))) { return $bin }
    }
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    $ninja = Get-Command ninja -ErrorAction SilentlyContinue
    if ($cmake -and $ninja) { return Split-Path $cmake.Source }
    throw 'CMake with Ninja was not found; install the CMake package of the Android SDK or set cmake.dir in local.properties'
}

# Brings the Visual Studio compiler into this session, for the unit tests that run on this machine.
function Enter-VisualStudio {
    if (Get-Command cl -ErrorAction SilentlyContinue) { return }
    $folder = Get-Local 'vs.dir'
    if (-not $folder) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (Test-Path $vswhere) { $folder = & $vswhere -latest -products * -property installationPath | Select-Object -First 1 }
    }
    $script = if ($folder) { Join-Path $folder 'VC\Auxiliary\Build\vcvars64.bat' }
    if (-not $script -or -not (Test-Path $script)) { throw 'Visual Studio with the C++ tools was not found; set vs.dir in local.properties' }
    foreach ($line in cmd /c "`"$script`" >nul 2>&1 && set") {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
    }
    if (-not (Get-Command cl -ErrorAction SilentlyContinue)) { throw 'the Visual Studio environment did not bring a compiler' }
}

function Resolve-Abi([string[]]$Abi) {
    $names = $Abi | ForEach-Object { $_ -split ',' } | Where-Object { $_ }
    if (-not $names -or $names -contains 'all') { $names = $Abis }
    foreach ($name in $names) {
        if ($Abis -notcontains $name) { throw "unknown ABI '$name' (expected: $($Abis -join ', '))" }
    }
    $names
}

# Runs a program and returns what it printed. If it fails, the lines that say why are shown and
# the script stops. What a program prints on its error stream is not a failure by itself.
function Invoke-Tool([string]$Program, [string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $said = @(& $Program @Arguments 2>&1 | ForEach-Object { "$_" }) } finally { $ErrorActionPreference = $previous }
    if ($LASTEXITCODE -ne 0) {
        $said | Where-Object { $_ -match 'error|FAILED|undefined|cannot' } | Select-Object -First 40 | Write-Host
        throw "$(Split-Path $Program -Leaf) failed (exit code $LASTEXITCODE)"
    }
    $said
}

# Writes a file's bytes as the numbers of a C++ array, next to it under the name "<file>.inc":
# how the binary carries the files it needs inside itself.
function Export-Bytes([string]$File) {
    $bytes = [IO.File]::ReadAllBytes($File)
    $lines = for ($start = 0; $start -lt $bytes.Length; $start += 24) {
        '    ' + ((($bytes[$start..([math]::Min($start + 23, $bytes.Length - 1))]) | ForEach-Object { '0x{0:x2}' -f $_ }) -join ', ') + ','
    }
    [IO.File]::WriteAllText("$File.inc", (($lines -join "`n") + "`n"), (New-Object Text.UTF8Encoding $false))
}
