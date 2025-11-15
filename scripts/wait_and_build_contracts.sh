#!/bin/bash
# Wait for vcpkg installation and then build contract tests

set -e

VCPKG_ROOT="${VCPKG_ROOT:-/home/steve/source/vcpkg}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "=============================================================="
echo "Waiting for vcpkg installation and building contract tests"
echo "=============================================================="
echo ""

# Function to check if packages are installed
check_packages() {
    cd "$VCPKG_ROOT"
    ./vcpkg list 2>/dev/null | grep -q "grpc:x64-linux" && \
    ./vcpkg list 2>/dev/null | grep -q "gtest:x64-linux"
}

# Wait for installation if running
if pgrep -f "vcpkg install" > /dev/null || ! check_packages; then
    echo "[INFO] Waiting for vcpkg installation to complete..."
    echo "  This may take 10-20 minutes for gRPC compilation."
    echo ""
    
    MAX_WAIT=3600  # 1 hour max
    ELAPSED=0
    CHECK_INTERVAL=30
    
    while [ $ELAPSED -lt $MAX_WAIT ]; do
        if ! pgrep -f "vcpkg install" > /dev/null; then
            sleep 5  # Give it a moment to finish
            if check_packages; then
                echo "[OK] vcpkg installation complete!"
                break
            fi
        fi
        
        # Show progress every minute
        if [ $((ELAPSED % 60)) -eq 0 ] && [ $ELAPSED -gt 0 ]; then
            echo "  Still waiting... ($(($ELAPSED / 60)) minutes elapsed)"
        fi
        
        sleep $CHECK_INTERVAL
        ELAPSED=$((ELAPSED + CHECK_INTERVAL))
    done
    
    if ! check_packages; then
        echo "[ERROR] Installation timeout or failed. Please check manually:"
        echo "  cd $VCPKG_ROOT && ./vcpkg install grpc gtest --triplet x64-linux"
        exit 1
    fi
else
    echo "[OK] Required packages already installed"
fi

echo ""
echo "[1/3] Configuring CMake..."
cd "$PROJECT_ROOT"
export VCPKG_ROOT="$VCPKG_ROOT"
cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    2>&1 | tee /tmp/cmake_config.log | grep -E "(Found|Error|Configuring|Build files)" || true

if grep -q "Configuring incomplete" /tmp/cmake_config.log; then
    echo "[ERROR] CMake configuration failed. Check /tmp/cmake_config.log for details."
    exit 1
fi

echo "[OK] CMake configured"
echo ""

echo "[2/3] Building contract tests..."
cmake --build build -j$(nproc) --target \
    contracts_masterclock_tests \
    contracts_metricsandtiming_tests \
    contracts_playoutcontrol_tests \
    contracts_orchestrationloop_tests \
    contracts_playoutengine_tests \
    contracts_renderer_tests \
    2>&1 | tee /tmp/cmake_build.log | tail -30

if [ ${PIPESTATUS[0]} -ne 0 ]; then
    echo "[ERROR] Build failed. Check /tmp/cmake_build.log for details."
    exit 1
fi

echo "[OK] Contract tests built"
echo ""

echo "[3/3] Running contract tests..."
cd build
ctest --output-on-failure -R "contracts_" --verbose 2>&1 | tee /tmp/ctest_output.log

echo ""
echo "=============================================================="
echo "Contract tests complete!"
echo "=============================================================="







