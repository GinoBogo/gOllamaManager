# Title: gOllamaManager Project
# Author: Gino Francesco Bogo

cmake_minimum_required(VERSION 3.10)

set(CMAKE_SYSTEM_NAME Linux)
#set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER gcc)
#set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER g++)
#set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

#set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)

project(gOllamaManager)

message("=======================================================================")
message("   OS: " ${CMAKE_SYSTEM_NAME})
message("  DIR: " ${CMAKE_SOURCE_DIR})
message(" TYPE: " ${CMAKE_BUILD_TYPE})
message("=======================================================================")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#set(CMAKE_CXX_STANDARD 17)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_compile_options(
    -fno-omit-frame-pointer
    -pedantic
    -Wall
    -Wcast-align
    -Wextra
    #-Wfloat-equal
    #-Wno-cast-align
    #-Wno-deprecated-non-prototype
    #-Wno-implicit-fallthrough
    #-Wno-missing-braces
    #-Wno-missing-field-initializers
    #-Wno-parentheses
    #-Wno-return-type
    #-Wno-self-assign
    #-Wno-sign-compare
    #-Wno-sign-conversion
    #-Wno-sometimes-uninitialized
    #-Wno-strict-prototypes
    #-Wno-unused-but-set-parameter
    #-Wno-unused-but-set-variable
    -Wno-unused-parameter
    #-Wno-unused-variable
    #-Wshadow
    #-Wswitch-default
)

if(COMPILE_LANGUAGE:CXX)
    add_compile_options(-Wold-style-cast)
endif()

find_library(LIB_PTHREAD pthread)
find_library(LIB_NCURSES ncurses)

add_executable(gollama_manager "gollama_manager.c")

target_link_libraries(gollama_manager ${LIB_PTHREAD} ${LIB_NCURSES})
