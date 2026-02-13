$ErrorActionPreference = "Stop"

param(
  [string]$BuildDir = "",
  [string]$Config = "Release",
  [string]$Generator = "",
  [switch]$SkipTests
)

$Root = (Resolve-Path "$PSScriptRoot/..").Path
if ([string]::IsNullOrWhiteSpace($BuildDir)) {
  $BuildDir = Join-Path $Root "build-win"
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
  Write-Error "cmake not found in PATH"
  exit 2
}

if ([string]::IsNullOrWhiteSpace($Generator)) {
  if (Get-Command ninja -ErrorAction SilentlyContinue) {
    $Generator = "Ninja"
  } else {
    $Generator = "Visual Studio 17 2022"
  }
}

$toolchainArgs = @()
if ($env:VCPKG_ROOT) {
  $toolchain = Join-Path $env:VCPKG_ROOT "scripts/buildsystems/vcpkg.cmake"
  if (Test-Path $toolchain) {
    $toolchainArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
  }
}

Write-Host "Windows build verification"
Write-Host "  root: $Root"
Write-Host "  build: $BuildDir"
Write-Host "  generator: $Generator"
Write-Host "  config: $Config"
if ($toolchainArgs.Count -gt 0) {
  Write-Host "  vcpkg: enabled"
} else {
  Write-Host "  vcpkg: not configured (set VCPKG_ROOT to enable)"
}

$cmakeArgs = @(
  "-S", $Root,
  "-B", $BuildDir,
  "-G", $Generator,
  "-DAGENT_BUILD_HOST=ON",
  "-DAGENT_BUILD_ESP32SIM=OFF"
) + $toolchainArgs

if ($Generator -like "Visual Studio*") {
  $cmakeArgs += "-A"
  $cmakeArgs += "x64"
} elseif ($Generator -eq "Ninja") {
  $cmakeArgs += "-DCMAKE_BUILD_TYPE=$Config"
}

cmake @cmakeArgs
cmake --build $BuildDir --config $Config

if (-not $SkipTests) {
  ctest --test-dir $BuildDir -C $Config -R agent_core_tests --output-on-failure
}
