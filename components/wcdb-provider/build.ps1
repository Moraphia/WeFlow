$ErrorActionPreference = 'Stop'
$providerRoot = $PSScriptRoot
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $providerRoot '..\..')).Path
$vendorRoot = Join-Path $providerRoot 'vendor\wcdb-2.1.16'
$downloadDir = Join-Path $projectRoot '.deployment\downloads'
$zipPath = Join-Path $downloadDir 'tencent-wcdb-2.1.16.zip'
$expectedDigest = '260845053c5dedc4578570203a5ba235c3be1e74a671c2fc1aeb12c988dd5346'
if (-not (Test-Path -LiteralPath (Join-Path $vendorRoot 'src\CMakeLists.txt'))) {
    New-Item -ItemType Directory -Path $downloadDir -Force | Out-Null
    if (-not (Test-Path -LiteralPath $zipPath)) {
        Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/Tencent/wcdb/releases/download/v2.1.16/wcdb-2.1.16.zip' -OutFile $zipPath
    }
    if ((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLower() -ne $expectedDigest) { throw 'Tencent WCDB source checksum mismatch' }
    Expand-Archive -LiteralPath $zipPath -DestinationPath (Join-Path $providerRoot 'vendor') -Force
}
$jsonHeader = Join-Path $providerRoot 'include\json.hpp'
if (-not (Test-Path -LiteralPath $jsonHeader)) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $jsonHeader) -Force | Out-Null
    Invoke-WebRequest -UseBasicParsing -Uri 'https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp' -OutFile $jsonHeader
}
if ((Get-FileHash -LiteralPath $jsonHeader -Algorithm SHA256).Hash.ToLower() -ne 'aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63') { throw 'JSON header checksum mismatch' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudioRoot = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
if (-not $visualStudioRoot) { throw 'Install Visual Studio C++ Build Tools before rebuilding' }
$cmakePath = Join-Path $visualStudioRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $cmakePath)) { $cmakePath = (Get-Command cmake -ErrorAction Stop).Source }
$buildPath = Join-Path $providerRoot 'build'
& $cmakePath -S $providerRoot -B $buildPath -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_GENERATOR_INSTANCE=$visualStudioRoot"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
& $cmakePath --build $buildPath --config Release --target weflow_wcdb_provider create_fixture weflow_local_export --parallel 8
if ($LASTEXITCODE -ne 0) { throw 'Native provider build failed' }
Push-Location $projectRoot
try {
    & node components/wcdb-provider/test.cjs
    if ($LASTEXITCODE -ne 0) { throw 'Provider tests failed' }
    & node components/wcdb-provider/full-test.cjs
    if ($LASTEXITCODE -ne 0) { throw 'Feature tests failed' }
} finally { Pop-Location }
