#!/bin/bash
# Test script for complete decode->render->metrics pipeline
# Copyright (c) 2025 RetroVue

set -e

echo ""
echo "=============================================================="
echo "Phase 3 Pipeline Test"
echo "=============================================================="
echo ""

# Determine build directory based on build type
BUILD_DIR="build"
if [ -d "build/RelWithDebInfo" ]; then
    BUILD_DIR="build/RelWithDebInfo"
elif [ -d "build/Debug" ]; then
    BUILD_DIR="build/Debug"
elif [ -d "build/Release" ]; then
    BUILD_DIR="build/Release"
fi

BINARY="$BUILD_DIR/retrovue_air"

if [ ! -f "$BINARY" ]; then
    echo "[ERROR] Binary not found at $BINARY"
    echo "Please build the project first:"
    echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo"
    echo "  cmake --build build"
    exit 1
fi

echo "[1/4] Starting playout engine..."
"$BINARY" --port 50051 > /dev/null 2>&1 &
PROCESS_PID=$!

sleep 3

echo "[2/4] Running gRPC tests..."
echo "" | python3 scripts/test_server.py

echo ""
echo "[3/4] Testing HTTP metrics endpoint..."
if curl -s -f --max-time 5 http://localhost:9308/metrics > /tmp/metrics_output.txt 2>&1; then
    echo "[SUCCESS] Metrics endpoint responding"
    echo ""
    echo "Sample metrics:"
    head -n 15 /tmp/metrics_output.txt | sed 's/^/  /'
    rm -f /tmp/metrics_output.txt
else
    echo "[FAIL] Metrics endpoint not responding"
    rm -f /tmp/metrics_output.txt
fi

echo ""
echo "[4/4] Cleaning up..."
kill $PROCESS_PID 2>/dev/null || true
wait $PROCESS_PID 2>/dev/null || true

echo ""
echo "=============================================================="
echo "Test complete!"
echo "=============================================================="
echo ""








