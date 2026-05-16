#!/usr/bin/env bash
# Author: Gino Francesco Bogo

clear

path=$(
    cd "$(dirname "$0")"
    pwd -P
)

echo "Path: $path"

# Clean phase (from clean.sh)
echo "Cleaning..."
rm -rf $path/build 2>/dev/null
echo "... all cleaned."

# Build phase (from build.sh)
echo "Building..."
mkdir $path/build 2>/dev/null
cd $path/build

# Set default to release if no arguments, otherwise use provided argument
if [ $# = 0 ]; then
    cmake $path -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=release
else
    cmake $path -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=$1
fi

make -j$(nproc)

# Copy compile_commands.json to the root path
cp $path/build/compile_commands.json $path/

echo "... rebuild complete."
