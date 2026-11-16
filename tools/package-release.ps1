param(
    [string]$Version = "v3.5.1-EX.1"
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Get-Item $PSScriptRoot).Parent.FullName
$distDir = Join-Path $repoRoot "dist"
$pkgDirName = "AegisubEX-$Version-win64-portable"
$pkgDir = Join-Path $distDir $pkgDirName
$zipFile = Join-Path $distDir "$pkgDirName.zip"

Write-Host "=== Packaging AegisubEX Release: $Version ===" -ForegroundColor Cyan
Write-Host "Source Root: $repoRoot"
Write-Host "Target Directory: $pkgDir"

if (Test-Path $pkgDir) {
    Remove-Item -Recurse -Force $pkgDir
}
if (Test-Path $zipFile) {
    Remove-Item -Force $zipFile
}

New-Item -ItemType Directory -Force -Path $pkgDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $pkgDir "models") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $pkgDir "locale\zh_CN\LC_MESSAGES") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $pkgDir "automation\autoload") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $pkgDir "automation\include") | Out-Null

# 1. Main binary and runtimes
Write-Host "Copying core executables and runtimes..."
Copy-Item (Join-Path $repoRoot "build-local\aegisub.exe") -Destination $pkgDir -Force
Copy-Item (Join-Path $repoRoot "build-local\DirectML.dll") -Destination $pkgDir -Force
Copy-Item (Join-Path $repoRoot "build-local\onnxruntime.dll") -Destination $pkgDir -Force

if (Test-Path (Join-Path $repoRoot "build-local\ffmpeg.exe")) {
    Write-Host "Copying FFmpeg engine..."
    Copy-Item (Join-Path $repoRoot "build-local\ffmpeg.exe") -Destination $pkgDir -Force
}

# 2. Models
Write-Host "Copying AI models..."
Copy-Item (Join-Path $repoRoot "build-local\models\ch_PP-OCRv4_det.onnx") -Destination (Join-Path $pkgDir "models") -Force

# 3. Locales
Write-Host "Copying translations..."
$moPath = Join-Path $repoRoot "build-local\zh_CN\LC_MESSAGES\aegisub.mo"
if (-not (Test-Path $moPath)) {
    $moPath = Join-Path $repoRoot "build-local\aegisub-zh_CN.mo"
}
Copy-Item $moPath -Destination (Join-Path $pkgDir "locale\zh_CN\aegisub.mo") -Force
Copy-Item $moPath -Destination (Join-Path $pkgDir "locale\zh_CN\LC_MESSAGES\aegisub.mo") -Force

# 4. Automation
Write-Host "Copying automation scripts..."
Copy-Item (Join-Path $repoRoot "automation\autoload\*") -Destination (Join-Path $pkgDir "automation\autoload") -Recurse -Force
Copy-Item (Join-Path $repoRoot "automation\include\*") -Destination (Join-Path $pkgDir "automation\include") -Recurse -Force

# 5. Portable Config
Write-Host "Configuring portable mode..."
$configJson = @"
{
	"App" : {
		"Local Config" : true
	}
}
"@
Set-Content -Path (Join-Path $pkgDir "config.json") -Value $configJson -Encoding utf8

# 6. Launcher batch script
Write-Host "Creating portable launcher..."
$launcherCmd = @"
@echo off
setlocal
cd /d "%~dp0"
start "" "%~dp0aegisub.exe" %*
endlocal
exit /b 0
"@
Set-Content -Path (Join-Path $pkgDir "打开AegisubEX.cmd") -Value $launcherCmd -Encoding ascii

# 7. Documentation
Write-Host "Copying docs..."
Copy-Item (Join-Path $repoRoot "README.md") -Destination $pkgDir -Force

# 8. Compress ZIP
Write-Host "Compressing $zipFile..." -ForegroundColor Green
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($pkgDir, $zipFile, [System.IO.Compression.CompressionLevel]::Optimal, $true)

$hash = (Get-FileHash -Path $zipFile -Algorithm SHA256).Hash
$fileSizeMB = [math]::Round(((Get-Item $zipFile).Length / 1MB), 2)

Write-Host "=== Packaging Complete ===" -ForegroundColor Cyan
Write-Host "Zip Archive: $zipFile"
Write-Host "Archive Size: $fileSizeMB MB"
Write-Host "SHA256: $hash"
