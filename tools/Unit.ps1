param(
    [string]$Filter = '',
    [string]$BuildDir = ''
)

# Builds the unit tests with this machine's own compiler and runs them: everything that does
# not need Android. An argument runs only the tests whose names contain it.
# -BuildDir puts the intermediate files somewhere else than build\host, for runs side by side.

. "$PSScriptRoot\Env.ps1"
$cmake = Find-CMake
Enter-VisualStudio
$folder = if ($BuildDir) { $BuildDir } else { Join-Path $Root 'build\host' }

$said = Invoke-Tool "$cmake\cmake.exe" @('-S', $Root, '-B', $folder, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$cmake\ninja.exe", '-DCMAKE_BUILD_TYPE=RelWithDebInfo', '-DCMAKE_CXX_COMPILER=cl')
$said | Where-Object { $_ -match 'CMake (Warning|Error)' }
$previous = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& "$cmake\cmake.exe" --build $folder --target unit-tests 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -match 'warning|error|FAILED' -or $_ -match '^\S+\(\d+\)' }
$built = $LASTEXITCODE
$ErrorActionPreference = $previous
if ($built -ne 0) { throw 'the unit tests did not build' }
& (Join-Path $folder 'unit-tests.exe') $Filter
exit $LASTEXITCODE
