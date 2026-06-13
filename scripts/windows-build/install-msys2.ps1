$msys2Root = "C:\msys64"
$bash = Join-Path $msys2Root "usr\bin\bash.exe"

if (Test-Path $bash) {
    Write-Host "MSYS2 already installed at $msys2Root"
}
else {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        throw "winget not found. Install MSYS2 manually from https://www.msys2.org and re-run this script."
    }

    Write-Host "Installing MSYS2 via winget..."
    winget install --id MSYS2.MSYS2 -e --accept-source-agreements --accept-package-agreements
    if ($LASTEXITCODE -ne 0) {
        throw "winget failed to install MSYS2"
    }

    if (-not (Test-Path $bash)) {
        throw "MSYS2 still not found at $msys2Root after install"
    }
}

Write-Host "Updating MSYS2 base packages..."
& $bash -lc "pacman -Syu --noconfirm"
& $bash -lc "pacman -Syu --noconfirm"
if ($LASTEXITCODE -ne 0) {
    throw "pacman -Syu failed"
}
