# Compiles the Vulkan shaders. Each one gets two files beside its source: <name>.spv, and the
# same words written as numbers in <name>.spv.inc. The second is what the C++ code includes:
# the clang of NDK 28 has no #embed yet.

. "$PSScriptRoot\Env.ps1"

$glslc = Join-Path (Find-Ndk) 'shader-tools\windows-x86_64\glslc.exe'
$shaders = Join-Path $Root 'src\Graphics\Vulkan\Shaders'
foreach ($source in Get-ChildItem $shaders -Include *.vert, *.frag -Recurse) {
    $binary = "$($source.FullName).spv"
    & $glslc -O $source.FullName -o $binary
    if ($LASTEXITCODE -ne 0) { throw "glslc failed for $($source.Name)" }
    $bytes = [IO.File]::ReadAllBytes($binary)
    $words = @(for ($at = 0; $at -lt $bytes.Length; $at += 4) { '0x{0:x8}' -f [BitConverter]::ToUInt32($bytes, $at) })
    $lines = for ($at = 0; $at -lt $words.Count; $at += 8) { ($words[$at..([Math]::Min($at + 7, $words.Count - 1))] -join ', ') + ',' }
    [IO.File]::WriteAllText("$binary.inc", ($lines -join "`n") + "`n")
    Write-Host ("{0,-10} {1:N0} bytes" -f $source.Name, $bytes.Length)
}
