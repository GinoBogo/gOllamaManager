#!/usr/bin/env bash
# Author: Gino Francesco Bogo

clear

path=$(
    cd "$(dirname "$0")"
    pwd -P
)

mkdir $path/build 2>/dev/null
cd $path/build

if [ $# = 0 ]; then
    cmake $path -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=debug
else
    cmake $path -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=$1
fi

make -j$(nproc)

# Copy compile_commands.json to the root path
cp $path/build/compile_commands.json $path/
