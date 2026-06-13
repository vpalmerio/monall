. "$PSScriptRoot\common.ps1"

# Mirrors scripts/ubuntu-build/install-deps.sh. Skips liburing-dev and
# libcap-dev (Linux-only, already gated behind CMAKE_SYSTEM_NAME == Linux in
# category/core/CMakeLists.txt and category/async/CMakeLists.txt) and
# libcgroup-dev (unused anywhere in the CMake build). libgtest-dev and
# libgmock-dev are provided together by mingw-w64-x86_64-gtest.
$packages = @(
    'mingw-w64-x86_64-libarchive'
    'mingw-w64-x86_64-benchmark'
    'mingw-w64-x86_64-brotli'
    'mingw-w64-x86_64-cli11'
    'mingw-w64-x86_64-crypto++'
    'mingw-w64-x86_64-gtest'
    'mingw-w64-x86_64-gmp'
    'mingw-w64-x86_64-tbb'
    'mingw-w64-x86_64-zstd'
)

Invoke-Pacman -Packages $packages
