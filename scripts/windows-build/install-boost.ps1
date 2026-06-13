. "$PSScriptRoot\common.ps1"

# Mirrors scripts/ubuntu-build/install-boost.sh. The single mingw-w64-x86_64-boost
# package provides the context, filesystem, json and stacktrace components.
Invoke-Pacman -Packages @('mingw-w64-x86_64-boost')
