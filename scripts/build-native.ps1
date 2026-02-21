# ──────────────────────────────────────────────────────────────────────────────
# Ainoiceguard - Native Build Script (Windows / PowerShell)
#
# Prerequisites:
#   - Visual Studio 2022 Build Tools (or full VS) with "Desktop C++" workload
#   - CMake 3.20+ (included with VS or install separately)
#   - Node.js 20+ with npm
#   - Python 3.x (for node-gyp)
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File ./scripts/build-native.ps1
#
# What it does:
#   1. Runs CMake to fetch & build PortAudio and RNNoise as static libs
#   2. Installs headers and libs to deps/install/
#   3. Runs node-gyp rebuild to compile the .node addon
# ──────────────────────────────────────────────────────────────────────────────

$ErrorActionPreference = "Stop"

$ROOT = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$DEPS_BUILD = Join-Path (Join-Path $ROOT "deps") "build"
$DEPS_INSTALL = Join-Path (Join-Path $ROOT "deps") "install"

# Find CMake (PATH, then Visual Studio, then standalone install)
$CMAKE_CMD = $null
if (Get-Command cmake -ErrorAction SilentlyContinue) {
    $CMAKE_CMD = "cmake"
} else {
    $cmakeCandidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "${env:ProgramFiles}\CMake\bin\cmake.exe"
    )
    foreach ($c in $cmakeCandidates) {
        if (Test-Path $c) {
            $CMAKE_CMD = $c
            Write-Host "Using CMake: $CMAKE_CMD" -ForegroundColor Gray
            break
        }
    }
}
if (-not $CMAKE_CMD) {
    Write-Host "ERROR: CMake not found." -ForegroundColor Red
    Write-Host "  - Add CMake to PATH, or" -ForegroundColor Yellow
    Write-Host "  - Install Visual Studio 2022 with 'Desktop development with C++' (includes CMake), or" -ForegroundColor Yellow
    Write-Host "  - Install CMake from https://cmake.org/download/" -ForegroundColor Yellow
    exit 1
}

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Ainoiceguard Native Build" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""

# ── Step 1: Build C dependencies with CMake ──────────────────────────────────
Write-Host "[1/3] Building PortAudio + RNNoise via CMake..." -ForegroundColor Yellow

# Create build and install directories
New-Item -ItemType Directory -Path $DEPS_BUILD -Force | Out-Null
New-Item -ItemType Directory -Path $DEPS_INSTALL -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DEPS_INSTALL "lib") -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $DEPS_INSTALL "include") -Force | Out-Null

$cmakeSource = Join-Path $ROOT "native"

# Configure: try VS 2026 (18), then 2022, then 2019 (required for node-gyp on Windows)
# InstancePath is used when VS is in a non-standard folder (e.g. "18" instead of "2026")
# When the instance is not in the VS Installer registry, add ",version=..." so CMake accepts it (portable instance)
# VS 18 may be under Program Files (x86) or Program Files
$vs18Path = $null
foreach ($base in @("${env:ProgramFiles(x86)}", "${env:ProgramFiles}")) {
    $p = Join-Path $base "Microsoft Visual Studio\18\BuildTools"
    if (Test-Path $p) { $vs18Path = $p; break }
}
if (-not $vs18Path) { $vs18Path = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\18\BuildTools" }
$vs18Version = "18.0.0.0"
if (Test-Path $vs18Path) {
    # CMake needs version as four integers "18.x.y.z". vswhere can return error text (e.g. "Error 0x7f..."), so validate with regex only.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        try {
            $raw = & $vswhere -path $vs18Path -property installationVersion 2>$null | Out-String
            $raw = ($raw -split "`n")[0].Trim()
            if ($raw -match '^(18\.\d+\.\d+\.\d+)$') { $vs18Version = $Matches[1] }
        } catch { }
    }
    if ($vs18Version -eq "18.0.0.0") {
        $versionFile = Join-Path $vs18Path "Common7\IDE\devenv.isolation.ini"
        if (Test-Path $versionFile) {
            $content = Get-Content $versionFile -Raw -ErrorAction SilentlyContinue
            if ($content -match 'version\s*=\s*(18\.\d+\.\d+\.\d+)') { $vs18Version = $Matches[1] }
        }
    }
}

$generators = @(
    @{ Name = "Visual Studio 18 2026"; Arch = "x64"; InstancePath = $vs18Path; InstanceVersion = $vs18Version },
    @{ Name = "Visual Studio 17 2022"; Arch = "x64"; InstancePath = $null; InstanceVersion = $null },
    @{ Name = "Visual Studio 16 2019"; Arch = "x64"; InstancePath = $null; InstanceVersion = $null }
)
$configured = $false
foreach ($gen in $generators) {
    Write-Host "Trying generator: $($gen.Name) -A $($gen.Arch)..." -ForegroundColor Gray
    if (Test-Path $DEPS_BUILD) {
        Remove-Item -Recurse -Force $DEPS_BUILD
    }
    New-Item -ItemType Directory -Path $DEPS_BUILD -Force | Out-Null
    $cmakeArgs = @(
        "-S", $cmakeSource, "-B", $DEPS_BUILD,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_INSTALL_PREFIX=$DEPS_INSTALL",
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
        "-G", $gen.Name, "-A", $gen.Arch
    )
    if ($gen.InstancePath -and (Test-Path $gen.InstancePath)) {
        $instanceValue = $gen.InstancePath
        if ($gen.InstanceVersion) {
            $instanceValue = "$($gen.InstancePath),version=$($gen.InstanceVersion)"
            Write-Host "  Using VS instance: $($gen.InstancePath) (version=$($gen.InstanceVersion))" -ForegroundColor Gray
        } else {
            Write-Host "  Using VS instance: $($gen.InstancePath)" -ForegroundColor Gray
        }
        $cmakeArgs += "-DCMAKE_GENERATOR_INSTANCE=`"$instanceValue`""
    }
    & $CMAKE_CMD @cmakeArgs
    if ($LASTEXITCODE -eq 0) {
        $configured = $true
        Write-Host "Configured with $($gen.Name)." -ForegroundColor Green
        break
    }
}
$usedNinjaFallback = $false
if (-not $configured) {
    # Fallback: use Ninja + vcvars so the compiler is found (VS 18 in non-standard path).
    $vcvars64 = Join-Path $vs18Path "VC\Auxiliary\Build\vcvars64.bat"
    $vcvarsAll = Join-Path $vs18Path "VC\Auxiliary\Build\vcvarsall.bat"
    $vcvarsToUse = $null
    if (Test-Path $vcvars64) { $vcvarsToUse = $vcvars64 }
    elseif (Test-Path $vcvarsAll) { $vcvarsToUse = "`"$vcvarsAll`" x64" }
    if ((Test-Path $vs18Path) -and $vcvarsToUse) {
        Write-Host "Trying Ninja with VS 18 environment (vcvars)..." -ForegroundColor Gray
        if (Test-Path $DEPS_BUILD) { Remove-Item -Recurse -Force $DEPS_BUILD }
        New-Item -ItemType Directory -Path $DEPS_BUILD -Force | Out-Null
        if ($vcvarsToUse -like "*vcvarsall*") {
            $ninjaCmd = "call $vcvarsToUse && `"$CMAKE_CMD`" -S `"$cmakeSource`" -B `"$DEPS_BUILD`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=`"$DEPS_INSTALL`" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 && `"$CMAKE_CMD`" --build `"$DEPS_BUILD`" && `"$CMAKE_CMD`" --install `"$DEPS_BUILD`""
        } else {
            $ninjaCmd = "call `"$vcvarsToUse`" && `"$CMAKE_CMD`" -S `"$cmakeSource`" -B `"$DEPS_BUILD`" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=`"$DEPS_INSTALL`" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 && `"$CMAKE_CMD`" --build `"$DEPS_BUILD`" && `"$CMAKE_CMD`" --install `"$DEPS_BUILD`""
        }
        cmd /c $ninjaCmd
        if ($LASTEXITCODE -eq 0) {
            $configured = $true
            $usedNinjaFallback = $true
            Write-Host "Configured and built with Ninja (VS 18)." -ForegroundColor Green
        }
    }
    elseif (Test-Path $vs18Path) {
        Write-Host "  VS 18 path found but VC\Auxiliary\Build\vcvars64.bat (or vcvarsall.bat) is missing." -ForegroundColor Yellow
        Write-Host "  Install the 'Desktop development with C++' workload in Visual Studio Installer." -ForegroundColor Yellow
    }
}
if (-not $configured) {
    Write-Host "CMake configure failed: no Visual Studio found." -ForegroundColor Red
    Write-Host "  Install Visual Studio 2022 Build Tools with 'Desktop development with C++':" -ForegroundColor Yellow
    Write-Host "  https://visualstudio.microsoft.com/visual-cpp-build-tools/" -ForegroundColor Yellow
    Write-Host "  (If you have VS 18 under folder '18', add the C++ workload in the Installer.)" -ForegroundColor Yellow
    exit 1
}

# Build (skip if we already built via Ninja fallback)
if (-not $usedNinjaFallback) {
    & $CMAKE_CMD --build $DEPS_BUILD --config Release

    if ($LASTEXITCODE -ne 0) {
        Write-Host "CMake build failed!" -ForegroundColor Red
        exit 1
    }

    # Install (copies libs and headers to deps/install)
    & $CMAKE_CMD --install $DEPS_BUILD --config Release

    if ($LASTEXITCODE -ne 0) {
        Write-Host "CMake install failed!" -ForegroundColor Red
        exit 1
    }
}

Write-Host "[1/3] Done!" -ForegroundColor Green

# ── Step 2: Verify dependencies ─────────────────────────────────────────────
Write-Host ""
Write-Host "[2/3] Verifying built dependencies..." -ForegroundColor Yellow

# PortAudio installs portaudio.h to include/; RNNoise we install to include/rnnoise/
$requiredFiles = @(
    (Join-Path (Join-Path $DEPS_INSTALL "include") "portaudio.h"),
    (Join-Path (Join-Path (Join-Path $DEPS_INSTALL "include") "rnnoise") "rnnoise.h")
)

foreach ($f in $requiredFiles) {
    if (Test-Path $f) {
        Write-Host "  OK: $f" -ForegroundColor Green
    } else {
        Write-Host "  MISSING: $f" -ForegroundColor Red
        Write-Host "Build may have failed. Check CMake output above." -ForegroundColor Red
    }
}

# Check for library files (name may vary)
$libDir = Join-Path $DEPS_INSTALL "lib"
$libs = Get-ChildItem -Path $libDir -Filter "*.lib" -ErrorAction SilentlyContinue
if ($libs) {
    foreach ($lib in $libs) {
        Write-Host "  OK: $($lib.FullName)" -ForegroundColor Green
    }
} else {
    Write-Host "  WARNING: No .lib files found in $libDir" -ForegroundColor Yellow
    Write-Host "  Checking for .a files (MinGW)..." -ForegroundColor Yellow
    $aLibs = Get-ChildItem -Path $libDir -Filter "*.a" -ErrorAction SilentlyContinue
    if ($aLibs) {
        foreach ($lib in $aLibs) {
            Write-Host "  OK: $($lib.FullName)" -ForegroundColor Green
        }
    }
}

Write-Host "[2/3] Done!" -ForegroundColor Green

# ── Step 3: Build Node native addon ─────────────────────────────────────────
Write-Host ""
Write-Host "[3/3] Building native addon with node-gyp..." -ForegroundColor Yellow

# node-gyp needs a valid Visual Studio environment. When VS is in a non-standard path
# (e.g. VS 18 under "18\BuildTools"), run node-gyp from a shell that has vcvars64.bat
# applied so VCINSTALLDIR is set and node-gyp uses that (node-gyp 12.1+ supports VS 2026).
$vcvars64 = $null
if ($vs18Path -and (Test-Path $vs18Path)) {
    $vcvars64 = Join-Path $vs18Path "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars64)) {
        $vcvars64 = Join-Path $vs18Path "VC\Auxiliary\Build\vcvarsall.bat"
        if (Test-Path $vcvars64) { $vcvars64 = "`"$vcvars64`" x64" }
        else { $vcvars64 = $null }
    }
}

$nativeDir = Join-Path $ROOT "native"
$nodeGypOk = $false

if ($vcvars64) {
    Write-Host "  Using VS environment (vcvars) for node-gyp..." -ForegroundColor Gray
    $nodeGypCmd = "call `"$vcvars64`" && cd /d `"$nativeDir`" && npx node-gyp rebuild --release --msvs_version=2026"
    cmd /c $nodeGypCmd
    $nodeGypOk = ($LASTEXITCODE -eq 0)
} else {
    Push-Location $nativeDir
    try {
        npx node-gyp rebuild --release --msvs_version=2026
        $nodeGypOk = ($LASTEXITCODE -eq 0)
    } finally {
        Pop-Location
    }
}

if (-not $nodeGypOk) {
    Write-Host "node-gyp build failed!" -ForegroundColor Red
    Write-Host "  Ensure node-gyp is 12.1+ (npm install node-gyp@^12.1.0) for VS 2026 support." -ForegroundColor Yellow
    exit 1
}

# Copy the built .node file to project root build/ for easy access
$buildDir = Join-Path (Join-Path $ROOT "build") "Release"
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null

$nodeFile = Join-Path (Join-Path (Join-Path (Join-Path $ROOT "native") "build") "Release") "ainoiceguard.node"
if (Test-Path $nodeFile) {
    Copy-Item $nodeFile -Destination $buildDir -Force
    Write-Host "  Copied ainoiceguard.node to build/Release/" -ForegroundColor Green
}

Write-Host "[3/3] Done!" -ForegroundColor Green
Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  Build complete!" -ForegroundColor Cyan
Write-Host "  Run 'npm start' to launch Ainoiceguard." -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
