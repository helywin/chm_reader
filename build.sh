#!/bin/bash

# CHM Reader Build Script

echo "Building CHM Reader..."

# Create build directory if not exists
mkdir -p build
cd build

# Run cmake
echo "Running cmake..."
cmake ..

# Compile
echo "Compiling..."
make -j$(nproc)

if [ $? -eq 0 ]; then
    echo "✓ Build successful!"
    echo "Run with: ./build/chmreader"
else
    echo "✗ Build failed!"
    exit 1
fi
