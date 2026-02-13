$ErrorActionPreference = "Stop"

param(
  [string]$BuildDir = "",
  [string]$Config = "Release",
  [string]$Generator = "",
  [string]$VcpkgRoot = "",
  [switch]$InstallDeps,
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

if ([string]::IsNullOrWhiteSpace($VcpkgRoot)) {
  $VcpkgRoot = $env:VCPKG_ROOT
}
if (-not [string]::IsNullOrWhiteSpace($VcpkgRoot)) {
  $env:VCPKG_ROOT = $VcpkgRoot
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
  if (-not (Test-Path $toolchain)) {
    if ($InstallDeps) {
      if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        Write-Error "git not found in PATH (required to install vcpkg)"
        exit 3
      }
      Write-Host "Cloning vcpkg to $env:VCPKG_ROOT"
      git clone https://github.com/microsoft/vcpkg $env:VCPKG_ROOT
    } else {
      Write-Error "vcpkg toolchain not found; set VCPKG_ROOT or use -InstallDeps"
      exit 3
    }
  }
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

if ($InstallDeps) {
  if (-not $env:VCPKG_ROOT) {
    Write-Error "InstallDeps requires VCPKG_ROOT or -VcpkgRoot"
    exit 3
  }
  $vcpkgExe = Join-Path $env:VCPKG_ROOT "vcpkg.exe"
  if (-not (Test-Path $vcpkgExe)) {
    $bootstrap = Join-Path $env:VCPKG_ROOT "bootstrap-vcpkg.bat"
    if (-not (Test-Path $bootstrap)) {
      Write-Error "missing vcpkg bootstrap script at $bootstrap"
      exit 3
    }
    & $bootstrap
  }
  & $vcpkgExe install curl jsoncpp sqlite3
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
