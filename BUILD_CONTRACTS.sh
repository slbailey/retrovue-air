#!/bin/bash
# Build and test contract tests (run this once vcpkg installation completes)

set -e

VCPKG_ROOT="${VCPKG_ROOT:-/home/steve/source/vcpkg}"
cd "$(dirname "$0")"

echo "=============================================================="
echo "Building Contract Tests"
echo "=============================================================="
echo ""

# Check if packages are installed
cd "$VCPKG_ROOT"
if ! ./vcpkg list | grep -q "grpc:x64-linux"; then
    echo "ERROR: gRPC not installed yet. Please wait for vcpkg installation to complete."
    echo "  Check status: cd $VCPKG_ROOT && ./vcpkg list | grep grpc"
    exit 1
fi

if ! ./vcpkg list | grep -q "gtest:x64-linux"; then
    echo "ERROR: gtest not installed yet. Installing now..."
    ./vcpkg install gtest --triplet x64-linux
fi

# Configure CMake
echo "[1/3] Configuring CMake..."
cd "$(dirname "$0")"
export VCPKG_ROOT="$VCPKG_ROOT"
cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo

# Build contract tests
echo ""
echo "[2/3] Building contract tests..."
cmake --build build -j$(nproc) --target \
    contracts_masterclock_tests \
    contracts_metricsandtiming_tests \
    contracts_playoutcontrol_tests \
    contracts_orchestrationloop_tests \
    contracts_playoutengine_tests \
    contracts_renderer_tests \
    contracts_videofileproducer_tests

# Run tests
echo ""
echo "[3/3] Running contract tests..."
cd build
ctest --output-on-failure -R "contracts_" --verbose

echo ""
echo "=============================================================="
echo "Contract tests complete!"
echo "=============================================================="
