$here = $PSScriptRoot

& "$here\install-msys2.ps1"
& "$here\install-tools.ps1"
& "$here\install-deps.ps1"
& "$here\install-boost.ps1"

# Some submodule fixture paths (e.g. third_party/ethereum-tests' nested
# LegacyTests) exceed Windows' 260-character MAX_PATH. core.longpaths makes
# git itself bypass that limit (via the \\?\ path prefix internally) for
# clone/checkout, independent of any OS policy. Set --global so it covers
# the main repo and every submodule (including nested ones, and any added
# later) without needing to repeat it per-repo -- git config doesn't cascade
# from a parent repo into its submodules' own configs. This does not require
# administrator privileges, unlike the OS-level policy below.
git config --global core.longpaths true

# core.longpaths only affects git's own file operations. Other tools (e.g.
# Explorer, editors, non-git build steps) that touch the same long-path
# files still rely on Windows' own long-path support, which is a
# machine-wide registry policy (HKLM, not HKCU) and therefore requires
# administrator privileges to enable -- it can't be set from this script
# without elevation. If you hit "Filename too long" errors outside of git,
# enable it manually (one-time, persists across reboots):
#   Computer Configuration > Administrative Templates > System > Filesystem
#   > "Enable Win32 long paths", or run as Administrator:
#     Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" `
#       -Name "LongPathsEnabled" -Value 1
Write-Host "Note: if you hit 'Filename too long' errors outside of git (e.g. in Explorer or an editor), enable Windows' long path support manually -- see comments in this script for the one-time, admin-required steps." -ForegroundColor Yellow

$repoRoot = Resolve-Path "$here\..\.."
Push-Location $repoRoot
try {
    git submodule update --init --recursive
}
finally {
    Pop-Location
}
