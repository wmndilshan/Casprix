[CmdletBinding()]
param(
    [ValidateSet('doctor', 'build', 'install', 'launch', 'logs')]
    [string]$Action = 'doctor',
    [string]$Source = 'examples/android/hello_android.cpx',
    [string]$Package = 'com.casprix.hello',
    [string]$Name = 'Casprix Android',
    [string]$Apk = 'build-apk/casprix-hello.apk',
    [string]$Device = '',
    [switch]$Release,
    [switch]$KeepBuild
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir '..\..')).Path

function Resolve-Tool([string]$Name, [string[]]$Candidates) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return (Resolve-Path $candidate).Path
        }
    }
    throw "Required tool '$Name' was not found."
}

$sdkRoot = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) {
    $env:ANDROID_SDK_ROOT
} else {
    Join-Path $env:LOCALAPPDATA 'Android\Sdk'
}
$adb = Resolve-Tool 'adb' @((Join-Path $sdkRoot 'platform-tools\adb.exe'))

function Get-DeviceId {
    if ($Device) { return $Device }
    $connected = @(& $adb devices | Select-Object -Skip 1 | ForEach-Object {
        if ($_ -match '^([^\s]+)\s+device$') { $Matches[1] }
    })
    if ($connected.Count -eq 0) { throw 'No authorized Android device is connected.' }
    if ($connected.Count -gt 1) {
        throw "Multiple devices are connected; pass -Device. Devices: $($connected -join ', ')"
    }
    return $connected[0]
}

function Invoke-Adb([string[]]$Arguments) {
    $deviceId = Get-DeviceId
    & $adb -s $deviceId @Arguments
    if ($LASTEXITCODE -ne 0) { throw "adb failed with exit code $LASTEXITCODE" }
}

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $repoRoot $Path
}

Push-Location $repoRoot
try {
    switch ($Action) {
        'doctor' {
            $deviceId = Get-DeviceId
            $model = (& $adb -s $deviceId shell getprop ro.product.model).Trim()
            $api = (& $adb -s $deviceId shell getprop ro.build.version.sdk).Trim()
            $abi = (& $adb -s $deviceId shell getprop ro.product.cpu.abi).Trim()
            $ndk = Get-ChildItem (Join-Path $sdkRoot 'ndk') -Directory -ErrorAction SilentlyContinue |
                Sort-Object Name -Descending | Select-Object -First 1
            Write-Host "Android SDK : $sdkRoot"
            Write-Host "Android NDK : $(if ($ndk) { $ndk.FullName } else { '(missing)' })"
            Write-Host "Device      : $deviceId ($model, API $api, $abi)"
            Write-Host "Compiler    : $(Resolve-RepoPath 'build-gcc/casprix.exe')"
            Write-Host "APK builder : $(Resolve-RepoPath 'build-gcc/apk-builder.exe')"
            $skiaLib = Resolve-RepoPath 'third_party/skia/lib-android-arm64/libskia.a'
            Write-Host "Android Skia: $(if (Test-Path $skiaLib) { $skiaLib } else { '(not built; build action will bootstrap it)' })"
        }
        'build' {
            $builder = Resolve-RepoPath 'build-gcc/apk-builder.exe'
            if (-not (Test-Path $builder)) {
                throw 'APK builder is missing. Build the apk-builder CMake target first.'
            }
            $arguments = @('--input', (Resolve-RepoPath $Source), '--package', $Package,
                '--name', $Name, '--output', (Resolve-RepoPath $Apk), '--abi', 'arm64-v8a')
            if ($Release) { $arguments += '--release' }
            if ($KeepBuild) { $arguments += '--keep-build' }
            & $builder @arguments
            if ($LASTEXITCODE -ne 0) { throw "APK build failed with exit code $LASTEXITCODE" }
        }
        'install' {
            $apkPath = Resolve-RepoPath $Apk
            if (-not (Test-Path $apkPath)) { throw "APK not found: $apkPath" }
            Invoke-Adb -Arguments @('install', '-r', $apkPath)
        }
        'launch' {
            $component = "$Package/com.casprix.app.CasprixNativeActivity"
            Invoke-Adb -Arguments @('shell', 'am', 'start', '-W', '-n', $component)
        }
        'logs' {
            Invoke-Adb -Arguments @('logcat', '-v', 'threadtime', 'CasprixApp:I', 'CasprixEGL:I',
                'CasprixRenderer:I', 'AndroidRuntime:E', '*:S')
        }
    }
}
finally {
    Pop-Location
}
