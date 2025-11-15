#!/bin/bash
# Build and test contract tests
# Waits for vcpkg installation if needed, then builds and runs contract tests

set -e

VCPKG_ROOT="${VCPKG_ROOT:-/home/steve/source/vcpkg}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "=============================================================="
echo "Building Contract Tests"
echo "=============================================================="
echo ""

# Wait for vcpkg installation to complete if in progress
if pgrep -f "vcpkg install" > /dev/null; then
    echo "[INFO] vcpkg installation in progress, waiting for completion..."
    while pgrep -f "vcpkg install" > /dev/null; do
        echo "  Still installing... (checking every 10 seconds)"
        sleep 10
    done
    echo "[INFO] vcpkg installation complete!"
    echo ""
fi

# Verify required packages
echo "[1/4] Checking vcpkg packages..."
cd "$VCPKG_ROOT"
if ! ./vcpkg list | grep -q "grpc:x64-linux"; then
    echo "[ERROR] grpc not installed. Installing now..."
    ./vcpkg install grpc --triplet x64-linux
fi
if ! ./vcpkg list | grep -q "gtest:x64-linux"; then
    echo "[ERROR] gtest not installed. Installing now..."
    ./vcpkg install gtest --triplet x64-linux
fi
echo "[OK] Required packages found"
echo ""

# Configure CMake
echo "[2/4] Configuring CMake..."
cd "$PROJECT_ROOT"
export VCPKG_ROOT="$VCPKG_ROOT"
cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    2>&1 | grep -E "(Found|Error|Warning|Configuring|Build files)" || true
echo "[OK] CMake configured"
echo ""

# Build contract tests
echo "[3/4] Building contract tests..."
cmake --build build -j$(nproc) --target \
    contracts_masterclock_tests \
    contracts_metricsandtiming_tests \
    contracts_playoutcontrol_tests \
    contracts_orchestrationloop_tests \
    contracts_playoutengine_tests \
    contracts_renderer_tests \
    contracts_videofileproducer_tests \
    2>&1 | tail -20
echo "[OK] Contract tests built"
echo ""

# Run contract tests
echo "[4/4] Running contract tests..."
cd build
ctest --output-on-failure -R "contracts_" --verbose
echo ""
echo "=============================================================="
echo "Contract tests complete!"
echo "=============================================================="




