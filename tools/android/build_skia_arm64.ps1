[CmdletBinding()]
param(
    [string]$NdkRoot = '',
    [string]$SdkRoot = '',
    [string]$BuildName = 'casprix-android-arm64',
    [int]$StopAfter = 0
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Write-Info([string]$Message) {
    Write-Host "[build-skia] $Message"
}

function Test-ZipArchive([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $zip = $null
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [System.IO.Compression.ZipFile]::OpenRead($Path)
        return $zip.Entries.Count -gt 0
    }
    catch {
        return $false
    }
    finally {
        if ($zip) { $zip.Dispose() }
    }
}

function Resolve-LatestChildDir([string]$BasePath) {
    if (-not (Test-Path $BasePath)) {
        return $null
    }

    $dirs = Get-ChildItem $BasePath -Directory | Sort-Object Name -Descending
    if ($dirs.Count -eq 0) {
        return $null
    }

    return $dirs[0].FullName
}

function Ensure-RepoPython([string]$RepoRoot) {
    $repoPython = Join-Path $RepoRoot 'build-tools\python311'
    if (Test-Path (Join-Path $repoPython 'python3.exe')) {
        return $repoPython
    }

    $candidates = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python311'),
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312')
    ) | Where-Object { $_ -and (Test-Path (Join-Path $_ 'python.exe')) }

    if ($candidates.Count -eq 0) {
        $pythonCommand = Get-Command python.exe -ErrorAction SilentlyContinue
        if ($pythonCommand) { return (Split-Path -Parent $pythonCommand.Source) }
        throw 'Could not find a usable Python installation. Install Python 3.x.'
    }

    $sourcePython = $candidates[0]
    Write-Info "Using installed Python runtime: $sourcePython"
    return $sourcePython
}

function Ensure-Python3Shim([string]$PythonRoot, [string]$RepoRoot) {
    $shim = Join-Path $RepoRoot 'build-tools\python3-shim'
    $python3 = Join-Path $shim 'python3.exe'
    if (Test-Path $python3) { return $shim }

    New-Item -ItemType Directory -Force -Path $shim | Out-Null
    Copy-Item (Join-Path $PythonRoot 'python.exe') $python3 -Force
    foreach ($runtime in @('python3.dll', 'python311.dll', 'vcruntime140.dll',
                            'vcruntime140_1.dll')) {
        $source = Join-Path $PythonRoot $runtime
        if (Test-Path $source) { Copy-Item $source $shim -Force }
    }
    return $shim
}

function Ensure-SkiaSource([string]$SourceDir) {
    if (Test-Path (Join-Path $SourceDir 'BUILD.gn')) {
        return
    }

    $parent = Split-Path -Parent $SourceDir
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    if ((Test-Path $SourceDir) -and
        (Get-ChildItem $SourceDir -Force -ErrorAction SilentlyContinue | Select-Object -First 1)) {
        throw "Skia source directory exists but is incomplete: $SourceDir"
    }

    $downloadDir = Join-Path $RepoRoot 'build-tools\downloads'
    $archive = Join-Path $downloadDir 'skia-chrome-m124.zip'
    New-Item -ItemType Directory -Force -Path $downloadDir | Out-Null
    if (-not (Test-ZipArchive $archive)) {
        if (Test-Path -LiteralPath $archive) {
            $invalidArchive = "$archive.invalid-$(Get-Date -Format yyyyMMddHHmmss)"
            Move-Item -LiteralPath $archive -Destination $invalidArchive
            Write-Info "Preserved invalid archive as $invalidArchive"
        }
        Write-Info 'Fetching the Skia milestone-124 source archive matching the vendored headers'
        & curl.exe -L --fail --show-error --connect-timeout 30 `
            --speed-limit 1024 --speed-time 120 --retry 20 --retry-delay 3 `
            --retry-all-errors --output $archive `
            'https://codeload.github.com/google/skia/zip/refs/heads/chrome/m124'
        if ($LASTEXITCODE -ne 0) {
            throw 'Could not fetch the Skia chrome/m124 source archive.'
        }
    } else {
        Write-Info "Reusing verified source archive: $archive"
    }

    if (-not (Test-ZipArchive $archive)) {
        throw "Downloaded Skia archive is invalid: $archive"
    }

    $extractRoot = Join-Path $RepoRoot `
        ("build-tools\skia-extract-{0}" -f [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $extractRoot | Out-Null
    $systemTar = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (-not (Test-Path -LiteralPath $systemTar)) {
        throw "Windows bsdtar was not found: $systemTar"
    }
    & $systemTar -xf $archive -C $extractRoot
    if ($LASTEXITCODE -ne 0) { throw 'Skia source extraction failed.' }
    $extracted = Get-ChildItem $extractRoot -Directory | Select-Object -First 1
    if (-not $extracted -or -not (Test-Path (Join-Path $extracted.FullName 'BUILD.gn'))) {
        throw "Extracted Skia source is incomplete: $extractRoot"
    }
    if (Test-Path $SourceDir) {
        Remove-Item -LiteralPath $SourceDir
    }
    Move-Item -LiteralPath $extracted.FullName -Destination $SourceDir
    Remove-Item -LiteralPath $extractRoot

    if (-not (Test-Path (Join-Path $SourceDir 'BUILD.gn'))) {
        throw 'Could not prepare the Skia chrome/m124 source tree.'
    }
}

function Ensure-Gn([string]$AndroidToolsDir, [string]$RepoRoot) {
    $gn = Join-Path $AndroidToolsDir 'gn.exe'
    if (Test-Path $gn) { return $gn }

    $javaOut = Join-Path $RepoRoot 'build-tools\android-bootstrap'
    New-Item -ItemType Directory -Force -Path $javaOut | Out-Null
    Write-Info 'Downloading the GN build-system binary'
    & javac -d $javaOut (Join-Path $AndroidToolsDir 'DownloadGN.java') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Could not compile the GN downloader.' }
    & java -cp $javaOut DownloadGN $AndroidToolsDir | Out-Host
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $gn)) {
        throw 'Could not download GN.'
    }
    return $gn
}

function Get-CommandPrimaryOutput([string]$Command) {
    if ($Command -match '(?:^|\s)-o\s+([^\s]+)') {
        return $Matches[1]
    }

    if ($Command -match 'llvm-ar\.exe\s+rcs\s+([^\s]+)') {
        return $Matches[1]
    }

    if ($Command -match 'rm\.py"\s+"([^"]+)"') {
        return $Matches[1]
    }

    return $null
}


function Invoke-BuildCommand([string]$Command) {
    $trimmed = $Command.Trim()

    if ($trimmed.StartsWith('cmd.exe /c')) {
        $payload = $trimmed.Substring(10).Trim()
        & cmd.exe /d /c $payload
        return $LASTEXITCODE
    }

    $match = [regex]::Match($trimmed, '^(?:"([^"]+)"|(\S+))(?:\s+(.*))?$')
    if (-not $match.Success) {
        throw "Could not parse build command: $Command"
    }

    $exe = if ($match.Groups[1].Success) { $match.Groups[1].Value } else { $match.Groups[2].Value }
    $args = $match.Groups[3].Value

    $process = Start-Process -FilePath $exe -ArgumentList $args -NoNewWindow -Wait -PassThru
    return $process.ExitCode
}

function Ensure-ArchiveRspFile([string]$SkiaOutDir, [string]$ArchiveName) {
    $rspPath = Join-Path $SkiaOutDir ("{0}.rsp" -f $ArchiveName)
    if (Test-Path $rspPath) {
        return
    }

    $pattern = "^build (?:\.\/)?$([regex]::Escape($ArchiveName)): alink (.*)$"
    $ninjaFiles = Get-ChildItem $SkiaOutDir -Recurse -Filter *.ninja | Sort-Object FullName
    foreach ($ninjaFile in $ninjaFiles) {
        $fileLines = Get-Content $ninjaFile.FullName
        for ($i = 0; $i -lt $fileLines.Count; $i++) {
            $line = $fileLines[$i]
            $match = [regex]::Match($line, $pattern)
            if (-not $match.Success) {
                continue
            }

            $inputs = [System.Collections.Generic.List[string]]::new()
            $fragment = $match.Groups[1].Value
            while ($true) {
                $trimmed = $fragment.Trim()
                $continued = $trimmed.EndsWith('$')
                if ($continued) {
                    $trimmed = $trimmed.Substring(0, $trimmed.Length - 1).TrimEnd()
                }
                if ($trimmed) {
                    [void]$inputs.Add($trimmed)
                }
                if (-not $continued) {
                    break
                }
                $i++
                if ($i -ge $fileLines.Count) {
                    break
                }
                $fragment = $fileLines[$i]
            }

            $content = ($inputs -join ' ') -replace '\s+\|\|.*$', ''
            if (-not $content) {
                throw "Resolved empty response file content for $ArchiveName from $($ninjaFile.FullName)"
            }
            [System.IO.File]::WriteAllText($rspPath, $content, [System.Text.Encoding]::ASCII)
            return
        }
    }

    throw "Could not locate alink inputs for $ArchiveName under $SkiaOutDir"
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir '..\..')).Path
$skiaSrc = Join-Path $repoRoot 'build-tools\sources\skia'
$skiaOut = Join-Path $skiaSrc (Join-Path 'out' $BuildName)
$skiaLibDir = Join-Path $repoRoot 'third_party\skia\lib-android-arm64'
$finalLib = Join-Path $skiaLibDir 'libskia.a'

Ensure-SkiaSource $skiaSrc
$gnExe = Ensure-Gn $scriptDir $repoRoot

if (-not $SdkRoot) {
    $SdkRoot = $env:ANDROID_HOME
}
if (-not $SdkRoot) {
    $SdkRoot = $env:ANDROID_SDK_ROOT
}
if (-not $SdkRoot) {
    $SdkRoot = Join-Path $env:LOCALAPPDATA 'Android\Sdk'
}
if (-not (Test-Path $SdkRoot)) {
    throw 'Android SDK not found. Set ANDROID_HOME or ANDROID_SDK_ROOT.'
}
$SdkRoot = (Resolve-Path $SdkRoot).Path

if (-not $NdkRoot) {
    $NdkRoot = $env:ANDROID_NDK_HOME
}
if (-not $NdkRoot) {
    $NdkRoot = $env:ANDROID_NDK
}
if (-not $NdkRoot) {
    $NdkRoot = $env:NDK_HOME
}
if (-not $NdkRoot) {
    $NdkRoot = Resolve-LatestChildDir (Join-Path $SdkRoot 'ndk')
}
if (-not $NdkRoot -or -not (Test-Path $NdkRoot)) {
    throw 'Android NDK not found. Set ANDROID_NDK_HOME or pass -NdkRoot.'
}
$NdkRoot = (Resolve-Path $NdkRoot).Path

$cmakeDir = Resolve-LatestChildDir (Join-Path $SdkRoot 'cmake')
if (-not $cmakeDir) {
    throw 'Android SDK CMake package not found. Install the SDK CMake component.'
}
$ninjaExe = Join-Path $cmakeDir 'bin\ninja.exe'
if (-not (Test-Path $ninjaExe)) {
    throw "Ninja executable not found: $ninjaExe"
}

$pythonRoot = Ensure-RepoPython $repoRoot
$pythonShim = Ensure-Python3Shim $pythonRoot $repoRoot
$env:PATH = "$pythonShim;$pythonRoot;$pythonRoot\Scripts;$env:PATH"
$env:PYTHONHOME = $pythonRoot
$pythonExe = Join-Path $pythonRoot 'python3.exe'
if (-not (Test-Path $pythonExe)) { $pythonExe = Join-Path $pythonRoot 'python.exe' }

$depsMarker = Join-Path $skiaSrc '.casprix-deps-ready'
if (-not (Test-Path $depsMarker)) {
    Write-Info 'Synchronizing Skia third-party dependencies sequentially'
    Push-Location $skiaSrc
    try {
        # A user-level 500 MB http.postBuffer setting makes each concurrent
        # Git process reserve excessive memory. Override it only for this
        # process tree and synchronize serially.
        $env:GIT_CONFIG_COUNT = '1'
        $env:GIT_CONFIG_KEY_0 = 'http.postBuffer'
        $env:GIT_CONFIG_VALUE_0 = '16777216'
        & $pythonExe (Join-Path $scriptDir 'sync_skia_deps_serial.py') $skiaSrc
        if ($LASTEXITCODE -ne 0) { throw 'Skia dependency synchronization failed.' }
        Set-Content -Path $depsMarker -Value 'chrome/m124' -Encoding ASCII
    }
    finally {
        Pop-Location
    }
}

if (Test-Path $finalLib) {
    Write-Info "Skia ARM64 already exists: $finalLib"
    exit 0
}

New-Item -ItemType Directory -Force -Path $skiaOut | Out-Null
New-Item -ItemType Directory -Force -Path $skiaLibDir | Out-Null

$ndkForGn = $NdkRoot -replace '\\', '/'
$argsGn = @"
target_os = "android"
target_cpu = "arm64"
ndk = "$ndkForGn"
ndk_api = 24
is_official_build = true
is_debug = false
skia_use_gl = true
skia_use_expat = true
skia_use_system_expat = false
skia_use_system_libjpeg_turbo = false
skia_use_system_libpng = false
skia_use_system_libwebp = false
skia_use_system_harfbuzz = false
skia_use_system_icu = false
skia_use_system_zlib = false
skia_use_system_freetype2 = false
skia_use_zlib = true
skia_use_freetype = true
skia_use_harfbuzz = true
skia_enable_skottie = false
skia_enable_pdf = false
"@
Set-Content -Path (Join-Path $skiaOut 'args.gn') -Value $argsGn -Encoding ASCII

Write-Info "SDK: $SdkRoot"
Write-Info "NDK: $NdkRoot"
Write-Info "Python: $pythonRoot"
Write-Info "Ninja: $ninjaExe"
Write-Info "Generating GN files for out/$BuildName"
Push-Location $skiaSrc
try {
    & $gnExe gen (Join-Path 'out' $BuildName)
    if ($LASTEXITCODE -ne 0) {
        throw "gn gen failed with exit code $LASTEXITCODE"
    }

    if ($StopAfter -gt 0) {
        Write-Info 'StopAfter is ignored by the parallel Ninja build.'
    }
    Write-Info 'Building Skia with Ninja (-j 6)'
    & $ninjaExe -C (Join-Path 'out' $BuildName) -j 6 skia
    if ($LASTEXITCODE -ne 0) {
        throw "Skia Ninja build failed with exit code $LASTEXITCODE"
    }

    $builtLib = Join-Path $skiaOut 'libskia.a'
    if (-not (Test-Path $builtLib)) {
        throw "Skia output missing: $builtLib"
    }

    Copy-Item $builtLib $finalLib -Force
    Write-Info "Skia ARM64 ready: $finalLib"
}
finally {
    Pop-Location
}

