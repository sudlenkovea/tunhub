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
# Drop a community OpenVPN build into .cores\openvpn\ (openvpn.exe plus its OpenSSL/lzo DLLs).
# Set OPENVPN_ZIP to a URL of a portable zip, or pre-populate .cores\openvpn yourself. If absent,
# OpenVPN tunnels are skipped (WireGuard/AmneziaWG still work).
if (-not (Test-Path "$Cores\openvpn\openvpn.exe")) {
    if ($env:OPENVPN_ZIP) {
        Invoke-WebRequest -Uri $env:OPENVPN_ZIP -OutFile "$Cores\openvpn.zip"
        Expand-Archive -Path "$Cores\openvpn.zip" -DestinationPath "$Cores\openvpn-x" -Force
        $exe = Get-ChildItem -Recurse "$Cores\openvpn-x" -Filter openvpn.exe | Select-Object -First 1
        if ($exe) {
            New-Item -ItemType Directory -Force -Path "$Cores\openvpn" | Out-Null
            Copy-Item (Join-Path $exe.DirectoryName "*") "$Cores\openvpn\" -Recurse -Force
        }
    } else {
        Write-Warning "OpenVPN core not found (.cores\openvpn\openvpn.exe). Set OPENVPN_ZIP or add it manually; skipping."
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

Write-Host "==> [6/7] Publishing privileged helper"
dotnet publish "src\TunHub.Helper\TunHub.Helper.csproj" -c $Config -r $Rid --self-contained true -o "$Dist\helper"
Copy-Item "$Dist\helper\tunhub-helper.exe" "$Dist\" -Force

Write-Host "==> [7/7] Bundling cores"
Copy-Item "$Cores\amneziawg-go.exe","$Cores\wireguard-go.exe","$Cores\wintun.dll" "$Dist\" -Force
if (Test-Path "$Cores\openvpn\openvpn.exe") {
    Copy-Item "$Cores\openvpn\*" "$Dist\" -Force
    Write-Host "    bundled OpenVPN core"
}

if (-not $env:SKIP_MSI) {
    Write-Host "==> Building MSI installer (WiX)"
    if (-not (Get-Command wix -ErrorAction SilentlyContinue)) {
        dotnet tool install --global wix --version 5.0.2 | Out-Null
        $env:Path += ";$env:USERPROFILE\.dotnet\tools"
    }
    $Msi = "dist\TunHub-0.8.1-$Rid.msi"
    # Absolute DistDir: WiX resolves -d paths relative to the .wxs file (installer\), not the cwd.
    $DistAbs = (Resolve-Path $Dist).Path
    wix build installer\TunHub.wxs -d DistDir="$DistAbs" -arch ($Rid -replace 'win-','') -o $Msi
    if (Test-Path $Msi) { Write-Host "    MSI: $Msi" }
}

Write-Host ""
Write-Host "Done."
Write-Host "  * Installer:  dist\TunHub-0.8.1-$Rid.msi  (installs app + registers TunHubHelper service)"
Write-Host "  * Portable:   $Dist\TunHub.exe  (register the service manually if not using the MSI):"
Write-Host "      sc.exe create TunHubHelper binPath= `"$((Resolve-Path "$Dist\tunhub-helper.exe" -ErrorAction SilentlyContinue).Path)`" start= auto"
Write-Host "      sc.exe start TunHubHelper"
