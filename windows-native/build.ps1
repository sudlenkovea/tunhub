<#
    Builds TunHub for Windows: native binaries, bundled tunnel cores, and an MSI.

    Requires Visual Studio 2022 build tools (MSVC + CMake), Go (to build the tunnel cores)
    and the WiX v5 CLI (`dotnet tool install --global wix`).

        .\build.ps1                # x64 release + MSI
        .\build.ps1 -SkipInstaller # binaries only
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'arm64')] [string]$Arch = 'x64',
    [string]$Config = 'Release',
    [switch]$SkipCores,
    [switch]$SkipInstaller
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root "build\$Arch"
$dist = Join-Path $root "dist\TunHub"

$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
Write-Host "==> TunHub $version ($Arch, $Config)" -ForegroundColor Cyan

# ── 1. Compile ───────────────────────────────────────────────────────────────
Write-Host '==> [1/4] Compiling' -ForegroundColor Cyan
$cmakeArch = if ($Arch -eq 'arm64') { 'ARM64' } else { 'x64' }
cmake -S $root -B $build -G 'Visual Studio 17 2022' -A $cmakeArch | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
cmake --build $build --config $Config --parallel | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

# ── 2. Stage ─────────────────────────────────────────────────────────────────
Write-Host '==> [2/4] Staging' -ForegroundColor Cyan
if (Test-Path $dist) { Remove-Item $dist -Recurse -Force }
New-Item -ItemType Directory -Path $dist -Force | Out-Null

Copy-Item (Join-Path $build "src\app\$Config\TunHub.exe") $dist
Copy-Item (Join-Path $build "src\helper\$Config\tunhub-helper.exe") $dist
Copy-Item (Join-Path $root 'src\app\TunHub.ico') $dist

# ── 3. Tunnel cores ──────────────────────────────────────────────────────────
# These are the only large artefacts in the installer, and they are a fixed cost: the tunnel
# protocols are implemented in Go / OpenVPN upstream.
if (-not $SkipCores) {
    Write-Host '==> [3/4] Building tunnel cores' -ForegroundColor Cyan
    $work = Join-Path $root 'build\cores'
    New-Item -ItemType Directory -Path $work -Force | Out-Null
    $env:GOOS = 'windows'
    $env:GOARCH = if ($Arch -eq 'arm64') { 'arm64' } else { 'amd64' }

    function Build-Core([string]$repo, [string]$tag, [string]$output) {
        $src = Join-Path $work ([IO.Path]::GetFileNameWithoutExtension($output))
        if (-not (Test-Path $src)) {
            git clone --depth 1 --branch $tag $repo $src | Out-Host
        }
        Push-Location $src
        try {
            # -s -w strips symbols and DWARF: roughly a third off each binary.
            go build -trimpath -ldflags '-s -w' -o (Join-Path $dist $output) . | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "go build failed for $output" }
        } finally { Pop-Location }
    }

    Build-Core 'https://github.com/amnezia-vpn/amneziawg-go' 'master' 'amneziawg-go.exe'
    Build-Core 'https://github.com/WireGuard/wireguard-go'   'master' 'wireguard-go.exe'

    # Wintun is required by both cores.
    $wintunZip = Join-Path $work 'wintun.zip'
    if (-not (Test-Path $wintunZip)) {
        Invoke-WebRequest 'https://www.wintun.net/builds/wintun-0.14.1.zip' -OutFile $wintunZip
    }
    Expand-Archive $wintunZip -DestinationPath (Join-Path $work 'wintun') -Force
    $wintunArch = if ($Arch -eq 'arm64') { 'arm64' } else { 'amd64' }
    Copy-Item (Join-Path $work "wintun\wintun\bin\$wintunArch\wintun.dll") $dist

    # OpenVPN: take the executable and its own DLLs from the official MSI, nothing else.
    # A published URL going stale should degrade to "no OpenVPN support" rather than fail the
    # whole build — WireGuard and AmneziaWG are unaffected by it.
    try {
        $ovpnMsi = Join-Path $work 'openvpn.msi'
        if (-not (Test-Path $ovpnMsi)) {
            Invoke-WebRequest 'https://swupdate.openvpn.org/community/releases/OpenVPN-2.6.11-I001-amd64.msi' `
                -OutFile $ovpnMsi -UseBasicParsing
        }
        $extract = Join-Path $work 'openvpn-extract'
        if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
        Start-Process msiexec.exe -ArgumentList "/a `"$ovpnMsi`" /qn TARGETDIR=`"$extract`"" -Wait
        $ovpnBin = Get-ChildItem $extract -Recurse -Filter 'openvpn.exe' -ErrorAction SilentlyContinue |
                   Select-Object -First 1
        if ($ovpnBin) {
            Copy-Item $ovpnBin.FullName $dist
            Get-ChildItem $ovpnBin.Directory -Filter '*.dll' | ForEach-Object { Copy-Item $_.FullName $dist }
        } else {
            Write-Warning 'openvpn.exe not found in the MSI — OpenVPN tunnels will not work'
        }
    } catch {
        Write-Warning "could not stage OpenVPN ($($_.Exception.Message)) — OpenVPN tunnels will not work"
    }
}

# Third-party licence texts must ship next to the binaries they cover.
$licenses = Join-Path $dist 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item (Join-Path $root '..\LICENSE') (Join-Path $licenses 'TunHub-LICENSE.txt') -ErrorAction SilentlyContinue

Write-Host ("    staged size: {0:N1} MB" -f ((Get-ChildItem $dist -Recurse | Measure-Object Length -Sum).Sum / 1MB))

# ── 4. Installer ─────────────────────────────────────────────────────────────
if (-not $SkipInstaller) {
    Write-Host '==> [4/4] Building MSI' -ForegroundColor Cyan
    $msi = Join-Path $root "dist\TunHub-$version-$Arch.msi"
    wix build (Join-Path $root 'installer\TunHub.wxs') `
        -arch $Arch `
        -d "DistDir=$dist" -d "Version=$version" `
        -ext WixToolset.UI.wixext -ext WixToolset.Util.wixext `
        -o $msi | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'wix build failed' }
    Write-Host ("==> {0} ({1:N1} MB)" -f $msi, ((Get-Item $msi).Length / 1MB)) -ForegroundColor Green
}
