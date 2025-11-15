#!/bin/bash
# Install required vcpkg packages for retrovue_air

set -e

VCPKG_ROOT="${VCPKG_ROOT:-/home/steve/source/vcpkg}"

echo "=============================================================="
echo "Installing required vcpkg packages"
echo "=============================================================="
echo ""
echo "Required packages:"
echo "  - grpc (for gRPC support)"
echo "  - gtest (for contract tests)"
echo ""
echo "This may take 10-20 minutes as gRPC needs to be compiled..."
echo ""

cd "$VCPKG_ROOT"

# Install grpc and gtest
./vcpkg install grpc gtest --triplet x64-linux

echo ""
echo "=============================================================="
echo "Installation complete!"
echo "=============================================================="
echo ""
echo "Verify installation:"
echo "  cd $VCPKG_ROOT && ./vcpkg list | grep -E '(grpc|gtest)'"
echo ""
echo "Then build contract tests:"
echo "  cd /home/steve/source/retrovue/retrovue_air"
echo "  ./BUILD_CONTRACTS.sh"







