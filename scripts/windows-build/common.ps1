$Msys2Root = "C:\msys64"

function Find-Msys2Root {
    $bash = Join-Path $Msys2Root "usr\bin\bash.exe"
    if (-not (Test-Path $bash)) {
        throw "MSYS2 not found at $Msys2Root. Run install-msys2.ps1 first."
    }
    return $Msys2Root
}

function Invoke-Pacman {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Packages
    )

    $root = Find-Msys2Root
    $bash = Join-Path $root "usr\bin\bash.exe"
    $packageList = $Packages -join ' '

    & $bash -lc "pacman -S --needed --noconfirm $packageList"
    if ($LASTEXITCODE -ne 0) {
        throw "pacman failed installing: $packageList"
    }
}
