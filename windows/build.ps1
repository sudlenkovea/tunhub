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
$Shared  = "..\avalonia"
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

Write-Host "==> [3/6] Stamping build"
$Stamp = "{0}-{1}" -f (Get-Date -Format "yyyyMMddHHmmss"), (git rev-parse --short HEAD 2>$null)
if (-not $Stamp) { $Stamp = "nogit" }
$engine = "$Shared\src\TunHub.Engine\EngineHost.cs"
(Get-Content $engine -Raw) -replace 'public const string Value = ".*?";', "public const string Value = ""$Stamp"";" |
    Set-Content $engine
Write-Host "    stamp: $Stamp"

Write-Host "==> [4/6] Publishing WinUI app ($Rid)"
if (Test-Path $Dist) { Remove-Item -Recurse -Force $Dist }
dotnet publish src\TunHub.WinUI\TunHub.WinUI.csproj -c $Config -r $Rid --self-contained true -o "$Dist"

Write-Host "==> [5/6] Publishing privileged helper"
dotnet publish "$Shared\src\TunHub.Helper\TunHub.Helper.csproj" -c $Config -r $Rid --self-contained true -o "$Dist\helper"
Copy-Item "$Dist\helper\tunhub-helper.exe" "$Dist\" -Force

Write-Host "==> [6/6] Bundling cores"
Copy-Item "$Cores\amneziawg-go.exe","$Cores\wireguard-go.exe","$Cores\wintun.dll" "$Dist\" -Force

Write-Host ""
Write-Host "Done: $Dist\TunHub.exe"
Write-Host "Register the privileged helper service (run as Administrator):"
Write-Host "  sc.exe create TunHubHelper binPath= `"$((Resolve-Path "$Dist\tunhub-helper.exe").Path)`" start= auto"
Write-Host "  sc.exe start TunHubHelper"
Write-Host "Then run $Dist\TunHub.exe"
Write-Host "(MSI packaging via WiX — TODO.)"
