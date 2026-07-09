#!/bin/bash
set -e

# Core count detection
NCPU=$(sysctl -n hw.ncpu 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

# Build settings (default is fast build: Debug type, LTO disabled, Unity Build disabled)
BUILD_TYPE="Debug"
LTO_OPTION="-DENABLE_LTO=OFF"
UNITY_OPTION="-DENABLE_UNITY_BUILD=OFF"

# Allow forcing rebuild of chassis
BUILD_CHASSIS=1
ARG="$1"

if [ "$ARG" = "release" ] || [ "$RELEASE" = "1" ] || [ "$OPT" = "1" ]; then
  echo "=== Release build requested (Release build type, LTO enabled, Unity Build enabled) ==="
  BUILD_TYPE="Release"
  LTO_OPTION="-DENABLE_LTO=ON"
  UNITY_OPTION="-DENABLE_UNITY_BUILD=ON"
  # Clean up ARG so it doesn't trigger "chassis" check
  ARG=""
else
  echo "=== Fast build (default: Debug build type, LTO disabled, Unity Build disabled) ==="
fi

# Detect Ninja generator for faster builds
GENERATOR=""
if command -v ninja >/dev/null 2>&1; then
  echo "=== Ninja detected: using Ninja generator ==="
  GENERATOR="-G Ninja"
fi

# Skip chassis build if it has already been compiled once, unless requested
if [ -f "build/Binaries/dolphin-tool" ] && [ "$ARG" != "chassis" ] && [ "$ARG" != "all" ]; then
  echo "=== Dolphin chassis already built. Skipping chassis build (run './build.sh chassis' to rebuild) ==="
  BUILD_CHASSIS=0
fi

if [ "$BUILD_CHASSIS" = "1" ]; then
  echo "=== Building RecompCore (Dolphin Emulator chassis) ==="
  cmake $GENERATOR -S . -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
  cmake --build build -j"$NCPU"
fi

echo "=== Building Static Recomp Module ==="
cd module-template
cmake $GENERATOR -S . -B build -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DGAME_ID=000002 $LTO_OPTION $UNITY_OPTION
cmake --build build -j"$NCPU"
cd ..

# Copy compiled module to default Dolphin User path search directory
DEST_DIR="$HOME/Library/Application Support/Dolphin/StaticRecompModules"
mkdir -p "$DEST_DIR"
cp module-template/build/g000002_recomp.dylib "$DEST_DIR/g000002_recomp.dylib" 2>/dev/null || cp module-template/build/Debug/g000002_recomp.dylib "$DEST_DIR/g000002_recomp.dylib" 2>/dev/null || cp module-template/build/Release/g000002_recomp.dylib "$DEST_DIR/g000002_recomp.dylib"
echo "=== Copied module to default user directory: $DEST_DIR ==="

echo "=== Build Complete! ==="
