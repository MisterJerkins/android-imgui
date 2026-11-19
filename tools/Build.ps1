param(
    [string[]]$Abi = @('all'),
    [switch]$Debug,
    [switch]$Quiet,
    [string]$BuildDir = ''
)

# Builds the binaries for Android: out\<abi>\android-imgui (the demo), with the tests next to it.
#   tools\Build.ps1                    every ABI
#   tools\Build.ps1 -Abi arm64-v8a     one of them
# -BuildDir puts the intermediate files somewhere else than build\, for builds that run side by side.

. "$PSScriptRoot\Env.ps1"
$cmake = Find-CMake
$ndk = (Find-Ndk) -replace '\\', '/'
$kind = if ($Debug) { 'Debug' } else { 'Release' }
$base = if ($BuildDir) { $BuildDir } else { Join-Path $Root 'build' }

foreach ($name in Resolve-Abi $Abi) {
    $folder = Join-Path $base "$name-$($kind.ToLower())"
    $configure = @('-S', $Root, '-B', $folder, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$cmake\ninja.exe", "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake",
        "-DANDROID_ABI=$name", '-DANDROID_PLATFORM=android-21', '-DANDROID_STL=c++_static', "-DCMAKE_BUILD_TYPE=$kind")
    $said = Invoke-Tool "$cmake\cmake.exe" $configure
    if (-not $Quiet) { $said | Where-Object { $_ -match 'warning|error' } }
    $said = Invoke-Tool "$cmake\cmake.exe" @('--build', $folder)
    if (-not $Quiet) { $said | Where-Object { $_ -match 'warning|error|FAILED' } }

    $out = Join-Path $Root "out\$name"
    New-Item -ItemType Directory -Force $out | Out-Null
    # Every binary the project builds goes to out.
    Get-ChildItem $folder -File | Where-Object { -not $_.Extension -and $_.Name -notmatch '^(Makefile|build)$' } | Copy-Item -Destination $out -Force
    $size = (Get-Item (Join-Path $out 'android-imgui')).Length
    Write-Host ("{0,-12} out\{0}\android-imgui  {1:N0} bytes" -f $name, $size)
}
