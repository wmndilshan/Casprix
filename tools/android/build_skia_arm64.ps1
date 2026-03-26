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
        throw 'Could not find a usable Python installation. Install Python 3.x or pre-populate build-tools\\python311.'
    }

    $sourcePython = $candidates[0]
    Write-Info "Preparing repo-local Python runtime from $sourcePython"
    New-Item -ItemType Directory -Force -Path $repoPython | Out-Null
    robocopy $sourcePython $repoPython /E /NFL /NDL /NJH /NJS /NC /NS /NP /XD __pycache__ | Out-Null
    if ($LASTEXITCODE -gt 7) {
        throw "robocopy failed while preparing repo-local Python runtime (exit $LASTEXITCODE)"
    }

    Copy-Item (Join-Path $repoPython 'python.exe') (Join-Path $repoPython 'python3.exe') -Force
    return $repoPython
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
$skiaSrc = Join-Path $repoRoot 'third_party\skia-src'
$skiaOut = Join-Path $skiaSrc (Join-Path 'out' $BuildName)
$skiaLibDir = Join-Path $repoRoot 'third_party\skia\lib-android-arm64'
$finalLib = Join-Path $skiaLibDir 'libskia.a'
$gnExe = Join-Path $scriptDir 'gn.exe'

if (-not (Test-Path $skiaSrc)) {
    throw "Skia source tree not found: $skiaSrc"
}
if (-not (Test-Path $gnExe)) {
    throw "GN executable not found: $gnExe"
}

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
$env:PATH = "$pythonRoot;$pythonRoot\Scripts;$env:PATH"
$env:PYTHONHOME = $pythonRoot

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
skia_enable_sksl = true
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

    $commands = & $ninjaExe -C (Join-Path 'out' $BuildName) -t commands skia
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to enumerate Skia build commands (exit $LASTEXITCODE)"
    }

    $commands = @($commands | Where-Object { $_ -and $_.Trim() })
    if ($commands.Count -eq 0) {
        throw 'Skia build command list is empty.'
    }

    $commandsFile = Join-Path $skiaOut 'skia-commands.txt'
    Set-Content -Path $commandsFile -Value $commands -Encoding ASCII
    Write-Info "Prepared $($commands.Count) commands"

    Push-Location $skiaOut
    try {
        for ($i = 0; $i -lt $commands.Count; $i++) {
            if ($StopAfter -gt 0 -and $i -ge $StopAfter) {
                Write-Info "StopAfter hit at command $StopAfter"
                break
            }

            $command = $commands[$i]
            $primaryOutput = Get-CommandPrimaryOutput $command
            if ($primaryOutput) {
                $outputPath = if ([System.IO.Path]::IsPathRooted($primaryOutput)) {
                    $primaryOutput
                } else {
                    Join-Path $skiaOut $primaryOutput
                }

                $outputDir = Split-Path -Parent $outputPath
                if ($outputDir) {
                    New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
                }

                if ($primaryOutput -like '*.a') {
                    Ensure-ArchiveRspFile $skiaOut $primaryOutput
                }

                if (Test-Path $outputPath) {
                    Write-Host ("[build-skia] [{0}/{1}] Skipping existing {2}" -f ($i + 1), $commands.Count, $primaryOutput)
                    continue
                }
            }

            Write-Host ("[build-skia] [{0}/{1}] {2}" -f ($i + 1), $commands.Count, $command)
            $exitCode = Invoke-BuildCommand $command
            if ($exitCode -ne 0) {
                throw "Command $($i + 1) failed with exit code $exitCode"
            }
        }
    }
    finally {
        Pop-Location
    }

    if ($StopAfter -gt 0 -and $StopAfter -lt $commands.Count) {
        Write-Info 'Partial build requested; stopping before final archive validation.'
        exit 0
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

