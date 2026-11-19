. "$PSScriptRoot\Env.ps1"

# Compiles the Java helper into the dex file the binary carries inside it.
$sdk = Find-Sdk
$platform = Get-ChildItem (Join-Path $sdk 'platforms') -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'android.jar') } | Sort-Object { [double]($_.Name -replace '^android-') } | Select-Object -Last 1
$d8 = Get-ChildItem (Join-Path $sdk 'build-tools') -Directory | Sort-Object { [version]$_.Name } | Select-Object -Last 1 | ForEach-Object { Join-Path $_.FullName 'd8.bat' }
$javac = @((Get-Setting 'JAVA_HOME'), (Get-Local 'java.home')) | Where-Object { $_ -and (Test-Path (Join-Path $_ 'bin\javac.exe')) } | Select-Object -First 1 | ForEach-Object { Join-Path $_ 'bin\javac.exe' }
if (-not ($platform -and $d8 -and $javac)) { throw 'the helper needs a JDK (JAVA_HOME, or java.home in local.properties) and an Android SDK with a platform and build-tools' }

$helper = Join-Path $Root 'src\Helper'
$classes = Join-Path $Root 'build\helper'
Remove-Item $classes -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $classes | Out-Null
$android = Join-Path $platform.FullName 'android.jar'
$env:JAVA_HOME = Split-Path (Split-Path $javac)

# Hidden framework classes are compiled against stand-ins, which stay out of the dex.
$stubs = Join-Path $classes 'stubs'
New-Item -ItemType Directory -Force $stubs | Out-Null
Invoke-Tool $javac (@('-source', '8', '-target', '8', '-Xlint:-options', '-cp', $android, '-d', $stubs) + (Get-ChildItem (Join-Path $helper 'Stubs') -Recurse -Filter *.java).FullName) | Out-Null
Invoke-Tool $javac (@('-source', '8', '-target', '8', '-Xlint:-options', '-cp', "$stubs;$android", '-d', $classes) + (Get-ChildItem (Join-Path $helper 'Java') -Recurse -Filter *.java).FullName) | Out-Null
Invoke-Tool $d8 (@('--release', '--min-api', '21', '--lib', $android, '--classpath', $stubs, '--output', $classes) + (Get-ChildItem (Join-Path $classes 'imgui') -Recurse -Filter *.class).FullName) | Out-Null
Copy-Item (Join-Path $classes 'classes.dex') (Join-Path $helper 'Helper.dex') -Force
Export-Bytes (Join-Path $helper 'Helper.dex')
Write-Host ("Helper.dex  {0:N0} bytes" -f (Get-Item (Join-Path $helper 'Helper.dex')).Length)
