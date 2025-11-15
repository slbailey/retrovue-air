#!/bin/bash
# Quick check if packages are installed and build if ready

VCPKG_ROOT="${VCPKG_ROOT:-/home/steve/source/vcpkg}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$VCPKG_ROOT"
if ./vcpkg list 2>/dev/null | grep -q "grpc:x64-linux" && \
   ./vcpkg list 2>/dev/null | grep -q "gtest:x64-linux"; then
    echo "✓ Packages installed! Building contract tests..."
    cd "$PROJECT_ROOT"
    export VCPKG_ROOT="$VCPKG_ROOT"
    
    cmake -S . -B build \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
    
    cmake --build build -j$(nproc) --target \
        contracts_masterclock_tests \
        contracts_metricsandtiming_tests \
        contracts_playoutcontrol_tests \
        contracts_orchestrationloop_tests \
        contracts_playoutengine_tests \
        contracts_renderer_tests
    
    cd build && ctest --output-on-failure -R "contracts_"
else
    echo "⚠ Packages not ready yet. Installation still in progress."
    echo "  Run this script again once installation completes."
    exit 1
fi







