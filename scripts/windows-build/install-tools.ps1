. "$PSScriptRoot\common.ps1"

# Mirrors scripts/ubuntu-build/install-tools.sh. Skips packages with no
# Windows/mingw equivalent (valgrind) and apt-bootstrapping packages that
# are unnecessary on Windows or assumed present (git, curl, ca-certificates,
# gnupg, software-properties-common, dialog, apt-utils).
$packages = @(
    'mingw-w64-x86_64-gcc'
    'mingw-w64-x86_64-clang'
    'mingw-w64-x86_64-clang-tools-extra'
    'mingw-w64-x86_64-cmake'
    'mingw-w64-x86_64-ninja'
    'mingw-w64-x86_64-gdb'
    'mingw-w64-x86_64-pkgconf'
    'mingw-w64-ucrt-x86_64-ripgrep'
    'mingw-w64-x86_64-python'
    'mingw-w64-x86_64-python-pytest'
    'mingw-w64-x86_64-make'
)

Invoke-Pacman -Packages $packages
