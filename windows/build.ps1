# TunHub for Windows (native WinUI 3). Produces dist\TunHub with TunHub.exe + helper + cores.
#
#   powershell -ExecutionPolicy Bypass -File .\build.ps1
#
# Requirements: .NET 8 SDK + Windows App SDK workload, Go 1.21+, git.
# Build ONLY on Windows (WinUI 3 needs the Windows App SDK). Fetches wintun.dll.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$Rid     = if ($env:RID) { $env:RID } else { "win-x64" }
$Config  = if ($env:CONFIG) { $env:CONFIG } else { "Release" }
$Dist    = "dist\TunHub"
$Cores   = ".cores"
$AwgRef  = if ($env:AWG_REF) { $env:AWG_REF } else { "v0.2.18" }
$AwgRepo = "https://github.com/amnezia-vpn/amneziawg-go"
$WgRepo  = "https://git.zx2c4.com/wireguard-go"

foreach ($tool in @("dotnet","go","git")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not found in PATH" }
}

Write-Host "==> [1/6] Building tunnel cores (windows/amd64)"
New-Item -ItemType Directory -Force -Path $Cores | Out-Null
if (-not (Test-Path "$Cores\amneziawg-go")) { git clone $AwgRepo "$Cores\amneziawg-go" }
if (-not (Test-Path "$Cores\wireguard-go"))  { git clone --depth 1 $WgRepo "$Cores\wireguard-go" }
Push-Location "$Cores\amneziawg-go"; git fetch --tags origin 2>$null; git checkout -q $AwgRef
$env:GOOS="windows"; $env:GOARCH="amd64"; $env:CGO_ENABLED="0"
go build -trimpath -ldflags "-s -w" -o ..\amneziawg-go.exe .
Pop-Location
Push-Location "$Cores\wireguard-go"
go build -trimpath -ldflags "-s -w" -o ..\wireguard-go.exe .
Pop-Location

Write-Host "==> [2/6] Fetching wintun.dll"
if (-not (Test-Path "$Cores\wintun.dll")) {
    Invoke-WebRequest -Uri "https://www.wintun.net/builds/wintun-0.14.1.zip" -OutFile "$Cores\wintun.zip"
    Expand-Archive -Path "$Cores\wintun.zip" -DestinationPath "$Cores\wintun" -Force
    Copy-Item "$Cores\wintun\wintun\bin\amd64\wintun.dll" "$Cores\wintun.dll" -Force
}

Write-Host "==> [3/7] Fetching OpenVPN core (openvpn.exe + DLLs)"
# The community OpenVPN Windows build (GPLv2) is downloaded as the official MSI and
# administratively extracted (no install), then its bin\ (openvpn.exe + OpenSSL/lzo DLLs)
# and license files are staged into .cores\openvpn\. We run it as a separate process, so
# this is mere aggregation — TunHub stays MIT while shipping the unmodified GPLv2 binary.
# Overrides: OPENVPN_MSI (a different MSI URL) or OPENVPN_ZIP (a portable zip).
if (-not (Test-Path "$Cores\openvpn\openvpn.exe")) {
    New-Item -ItemType Directory -Force -Path "$Cores\openvpn" | Out-Null
    if ($env:OPENVPN_ZIP) {
        Invoke-WebRequest -Uri $env:OPENVPN_ZIP -OutFile "$Cores\openvpn.zip"
        Expand-Archive -Path "$Cores\openvpn.zip" -DestinationPath "$Cores\openvpn-x" -Force
        $exe = Get-ChildItem -Recurse "$Cores\openvpn-x" -Filter openvpn.exe | Select-Object -First 1
        if ($exe) { Copy-Item (Join-Path $exe.DirectoryName "*") "$Cores\openvpn\" -Recurse -Force }
    } else {
        $OvpnMsiUrl = if ($env:OPENVPN_MSI) { $env:OPENVPN_MSI } `
                      else { "https://build.openvpn.net/downloads/releases/latest/openvpn-latest-stable-amd64.msi" }
        $msi = (Join-Path (Resolve-Path $Cores).Path "openvpn.msi")
        Write-Host "    downloading $OvpnMsiUrl"
        Invoke-WebRequest -Uri $OvpnMsiUrl -OutFile $msi
        $extract = (New-Item -ItemType Directory -Force -Path "$Cores\openvpn-msi").FullName
        # /a = administrative install: unpack files only, no system install / elevation.
        $p = Start-Process msiexec.exe -Wait -PassThru -ArgumentList "/a `"$msi`" /qn TARGETDIR=`"$extract`""
        if ($p.ExitCode -ne 0) { Write-Warning "msiexec extract failed ($($p.ExitCode))" }
        $exe = Get-ChildItem -Recurse $extract -Filter openvpn.exe -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($exe) {
            Copy-Item (Join-Path $exe.DirectoryName "*") "$Cores\openvpn\" -Recurse -Force
            # Stage license / copyright files (GPLv2 compliance) into a separate licenses dir.
            New-Item -ItemType Directory -Force -Path "$Cores\_licenses" | Out-Null
            Get-ChildItem -Recurse $extract -File -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match '(?i)(licen|copying|gpl|notice)' } |
                ForEach-Object { Copy-Item $_.FullName (Join-Path "$Cores\_licenses" ("openvpn-" + $_.Name)) -Force -ErrorAction SilentlyContinue }
        } else {
            Write-Warning "openvpn.exe not found in the MSI — OpenVPN tunnels will be unavailable (WireGuard/AmneziaWG still work)."
        }
    }
}

Write-Host "==> [4/7] Stamping build"
$Stamp = "{0}-{1}" -f (Get-Date -Format "yyyyMMddHHmmss"), (git rev-parse --short HEAD 2>$null)
if (-not $Stamp) { $Stamp = "nogit" }
$engine = "src\TunHub.Engine\EngineHost.cs"
(Get-Content $engine -Raw) -replace 'public const string Value = ".*?";', "public const string Value = ""$Stamp"";" |
    Set-Content $engine
Write-Host "    stamp: $Stamp"

Write-Host "==> [5/7] Publishing WinUI app ($Rid)"
if (Test-Path $Dist) { Remove-Item -Recurse -Force $Dist }
dotnet publish src\TunHub.WinUI\TunHub.WinUI.csproj -c $Config -r $Rid --self-contained true -o "$Dist"

Write-Host "==> [6/7] Publishing privileged helper (into the app folder — shared .NET runtime)"
# Publish alongside the app so tunhub-helper.exe has its runtime next to it (the base
# .NET 8 runtime DLLs are identical to the app's and just merge). The service then runs
# from the top-level folder, not a broken exe-only copy.
dotnet publish "src\TunHub.Helper\TunHub.Helper.csproj" -c $Config -r $Rid --self-contained true -o "$Dist"

Write-Host "==> [7/7] Bundling cores"
Copy-Item "$Cores\amneziawg-go.exe","$Cores\wireguard-go.exe","$Cores\wintun.dll" "$Dist\" -Force
if (Test-Path "$Cores\openvpn\openvpn.exe") {
    Copy-Item "$Cores\openvpn\*" "$Dist\" -Force
    Write-Host "    bundled OpenVPN core"
}

Write-Host "==> Bundling third-party license texts"
$Lic = "$Dist\licenses"
New-Item -ItemType Directory -Force -Path $Lic | Out-Null
# Core repos are cloned under .cores\ — copy their LICENSE text verbatim (MIT requires the notice).
if (Test-Path "$Cores\wireguard-go\LICENSE") { Copy-Item "$Cores\wireguard-go\LICENSE" "$Lic\wireguard-go-LICENSE.txt" -Force }
Get-ChildItem "$Cores\amneziawg-go" -Filter "LICENSE*" -File -ErrorAction SilentlyContinue |
    Select-Object -First 1 | ForEach-Object { Copy-Item $_.FullName "$Lic\amneziawg-go-LICENSE.txt" -Force }
Get-ChildItem "$Cores\wintun" -Recurse -Filter "*LICENSE*" -File -ErrorAction SilentlyContinue |
    Select-Object -First 1 | ForEach-Object { Copy-Item $_.FullName "$Lic\wintun-LICENSE.txt" -Force }
if (Test-Path "$Cores\_licenses") { Copy-Item "$Cores\_licenses\*" "$Lic\" -Force }
if (Test-Path "..\LICENSE") { Copy-Item "..\LICENSE" "$Lic\TunHub-LICENSE.txt" -Force }
if (Test-Path "..\THIRD-PARTY-NOTICES.md") { Copy-Item "..\THIRD-PARTY-NOTICES.md" "$Lic\" -Force }
Write-Host "    licenses → $Lic"

if (-not $env:SKIP_MSI) {
    Write-Host "==> Building MSI installer (WiX)"
    if (-not (Get-Command wix -ErrorAction SilentlyContinue)) {
        dotnet tool install --global wix --version 5.0.2 | Out-Null
        $env:Path += ";$env:USERPROFILE\.dotnet\tools"
    }
    # Extensions (pinned to the WiX 5 line): UI = folder-selection dialogs, Util = CloseApplication.
    wix extension add -g WixToolset.UI.wixext/5.0.2   2>$null | Out-Null
    wix extension add -g WixToolset.Util.wixext/5.0.2 2>$null | Out-Null
    $Msi = "dist\TunHub-0.8.1-$Rid.msi"
    # Absolute DistDir: WiX resolves -d paths relative to the .wxs file (installer\), not the cwd.
    $DistAbs = (Resolve-Path $Dist).Path
    wix build installer\TunHub.wxs -ext WixToolset.UI.wixext -ext WixToolset.Util.wixext `
        -d DistDir="$DistAbs" -arch ($Rid -replace 'win-','') -o $Msi
    if (Test-Path $Msi) { Write-Host "    MSI: $Msi" }
}

Write-Host ""
Write-Host "Done."
Write-Host "  * Installer:  dist\TunHub-0.8.1-$Rid.msi  (installs app + registers TunHubHelper service)"
Write-Host "  * Portable:   $Dist\TunHub.exe  (register the service manually if not using the MSI):"
Write-Host "      sc.exe create TunHubHelper binPath= `"$((Resolve-Path "$Dist\tunhub-helper.exe" -ErrorAction SilentlyContinue).Path)`" start= auto"
Write-Host "      sc.exe start TunHubHelper"
