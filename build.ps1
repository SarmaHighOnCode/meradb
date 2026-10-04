<#
.SYNOPSIS
  Build the C++ MeraDB (cpp/) in Release mode on Windows.

.DESCRIPTION
  Finds CMake, configures a Release build, builds with all cores and (optionally)
  runs the tests. Uses MinGW Makefiles when g++ is on the PATH, otherwise the
  newest Visual Studio that CMake knows about.

.EXAMPLE
  .\build.ps1                    # configure + build into cpp\build
  .\build.ps1 -Test              # ... then run ctest
  .\build.ps1 -NoWorkbench       # skip FTXUI (nothing is downloaded for it)
  .\build.ps1 -DryRun            # only print what would be run
#>
param(
    [switch]$Test,
    [switch]$NoWorkbench,
    [switch]$DryRun,
    [string]$BuildDir = "cpp\build",
    [string]$Generator = ""
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# 1. Find cmake: PATH first, then the default installer location.
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $guess = Join-Path $env:ProgramFiles "CMake\bin\cmake.exe"
    if (Test-Path $guess) { $cmake = $guess }
}
if (-not $cmake) { throw "cmake not found. Install CMake 3.20 or newer and add it to PATH." }

# 2. Pick a generator: MinGW if g++ is available, else CMake's default (Visual Studio).
$multi = $false
if (-not $Generator -and (Get-Command g++ -ErrorAction SilentlyContinue)) {
    $Generator = "MinGW Makefiles"
}
$configure = @("-S", "cpp", "-B", $BuildDir, "-DCMAKE_BUILD_TYPE=Release")
if ($Generator) { $configure += @("-G", $Generator) }
if ($Generator -like "Visual Studio*") { $multi = $true }
if (-not $Generator -and -not (Get-Command g++ -ErrorAction SilentlyContinue)) { $multi = $true }
if ($NoWorkbench) { $configure += "-DMERADB_WORKBENCH=OFF" }

$cores = [Environment]::ProcessorCount
$build = @("--build", $BuildDir, "--parallel", $cores)
if ($multi) { $build += @("--config", "Release") }
$ctest = @("--test-dir", $BuildDir, "--output-on-failure", "--parallel", $cores)
if ($multi) { $ctest += @("-C", "Release") }

function Run($exe, $argList) {
    Write-Host "> $exe $($argList -join ' ')"
    if ($DryRun) { return }
    # CMake prints warnings on stderr; do not let PowerShell treat that as a failure.
    $saved = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $exe @argList
    $code = $LASTEXITCODE
    $ErrorActionPreference = $saved
    if ($code -ne 0) { throw "failed (exit code $code)" }
}

Run $cmake $configure
Run $cmake $build
if ($Test) { Run (Join-Path (Split-Path $cmake) "ctest.exe") $ctest }

$exeDir = if ($multi) { Join-Path $BuildDir "Release" } else { $BuildDir }
Write-Host "Done. Program: $(Join-Path $exeDir 'meradb_cli.exe')"
