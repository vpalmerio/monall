$here = $PSScriptRoot

& "$here\install-msys2.ps1"
& "$here\install-tools.ps1"
& "$here\install-deps.ps1"
& "$here\install-boost.ps1"

$repoRoot = Resolve-Path "$here\..\.."
Push-Location $repoRoot
try {
    git submodule update --init --recursive
}
finally {
    Pop-Location
}
