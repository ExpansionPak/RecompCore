#!/bin/bash
set -e

# Core count detection
NCPU=$(sysctl -n hw.ncpu 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

echo "=== Building RecompCore (Dolphin Emulator chassis) ==="
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$NCPU"

echo "=== Building Static Recomp Module ==="
cd module-template
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGAME_ID=000002
cmake --build build -j"$NCPU"
cd ..

# Copy compiled module to default Dolphin User path search directory
DEST_DIR="$HOME/Library/Application Support/Dolphin/StaticRecompModules"
mkdir -p "$DEST_DIR"
cp module-template/build/g000002_recomp.dylib "$DEST_DIR/g000002_recomp.dylib"
echo "=== Copied module to default user directory: $DEST_DIR ==="

echo "=== Build Complete! ==="
