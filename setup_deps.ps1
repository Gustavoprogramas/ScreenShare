# setup_deps.ps1
# Download FFmpeg and SDL2 prebuilt binaries for Windows x64
# Run: powershell -ExecutionPolicy Bypass -File setup_deps.ps1

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$DepsDir   = Join-Path $ScriptDir 'deps'

Write-Host ''
Write-Host '=== Remote Desktop -- Dependency Setup ===' -ForegroundColor Cyan
Write-Host "Dependencies dir: $DepsDir" -ForegroundColor Gray
Write-Host ''

function Download-And-Extract {
    param(
        [string]$Url,
        [string]$ZipPath,
        [string]$ExtractTo
    )

    if (-not (Test-Path $ZipPath)) {
        Write-Host "  Downloading: $Url" -ForegroundColor Yellow
        Invoke-WebRequest -Uri $Url -OutFile $ZipPath -UseBasicParsing
        Write-Host "  Downloaded: $(Split-Path -Leaf $ZipPath)" -ForegroundColor Green
    } else {
        Write-Host "  Already downloaded: $(Split-Path -Leaf $ZipPath)" -ForegroundColor Gray
    }

    Write-Host '  Extracting...' -ForegroundColor Yellow
    Expand-Archive -Path $ZipPath -DestinationPath $ExtractTo -Force
    Write-Host "  Extracted to: $ExtractTo" -ForegroundColor Green
}

New-Item -ItemType Directory -Path $DepsDir -Force | Out-Null

$TempDir = Join-Path $DepsDir '_temp'
New-Item -ItemType Directory -Path $TempDir -Force | Out-Null

# FFmpeg -- BtbN GPL shared build (includes NVENC + CUVID + libx264)
Write-Host '[1/2] Setting up FFmpeg...' -ForegroundColor Cyan

$FFmpegDir = Join-Path $DepsDir 'ffmpeg'
$FFmpegZip = Join-Path $TempDir 'ffmpeg-release-full-shared.zip'
$FFmpegUrl = 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl-shared.zip'

$FFmpegHeaderCheck = Join-Path $FFmpegDir 'include\libavcodec\avcodec.h'

if (-not (Test-Path $FFmpegHeaderCheck)) {
    Download-And-Extract -Url $FFmpegUrl -ZipPath $FFmpegZip -ExtractTo $TempDir

    $FFmpegExtracted = Get-ChildItem $TempDir -Directory |
                       Where-Object { $_.Name -like 'ffmpeg-*' } |
                       Select-Object -First 1

    if (-not $FFmpegExtracted) {
        Write-Error 'FFmpeg extraction failed -- no directory found'
        exit 1
    }

    if (Test-Path $FFmpegDir) { Remove-Item $FFmpegDir -Recurse -Force }
    Move-Item $FFmpegExtracted.FullName $FFmpegDir

    Write-Host "  FFmpeg ready: $FFmpegDir" -ForegroundColor Green
} else {
    Write-Host '  FFmpeg already present -- skipping' -ForegroundColor Gray
}

# SDL2 -- Official SDL2 VC development libraries
Write-Host ''
Write-Host '[2/2] Setting up SDL2...' -ForegroundColor Cyan

$SDL2Dir     = Join-Path $DepsDir 'sdl2'
$SDL2Zip     = Join-Path $TempDir 'SDL2-devel-VC.zip'
$SDL2Version = '2.30.9'
$SDL2Url     = "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2Version/SDL2-devel-$SDL2Version-VC.zip"

$SDL2HeaderCheck = Join-Path $SDL2Dir 'include\SDL2\SDL.h'

if (-not (Test-Path $SDL2HeaderCheck)) {
    Download-And-Extract -Url $SDL2Url -ZipPath $SDL2Zip -ExtractTo $TempDir

    $SDL2Extracted = Get-ChildItem $TempDir -Directory |
                     Where-Object { $_.Name -like 'SDL2-*' } |
                     Select-Object -First 1

    if (-not $SDL2Extracted) {
        Write-Error 'SDL2 extraction failed'
        exit 1
    }

    if (Test-Path $SDL2Dir) { Remove-Item $SDL2Dir -Recurse -Force }
    Move-Item $SDL2Extracted.FullName $SDL2Dir

    Write-Host "  SDL2 ready: $SDL2Dir" -ForegroundColor Green
} else {
    Write-Host '  SDL2 already present -- skipping' -ForegroundColor Gray
}

# Cleanup temp
Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ''
Write-Host '=== Dependencies ready! ===' -ForegroundColor Green
Write-Host ''
Write-Host "FFmpeg : $FFmpegDir"
Write-Host "SDL2   : $SDL2Dir"
Write-Host ''
Write-Host 'Next steps:' -ForegroundColor Cyan
Write-Host '  1. cmake -B build -S . -DCMAKE_BUILD_TYPE=Release'
Write-Host '  2. cmake --build build --config Release'
Write-Host '  3. HOST  : build\Release\RemoteHost.exe   [client_ip]'
Write-Host '  4. CLIENT: build\Release\RemoteClient.exe [host_ip]'
Write-Host ''
