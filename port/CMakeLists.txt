cmake_minimum_required(VERSION 3.20)
project(hambo_jit CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)

if(NOT Boost_INCLUDE_DIR AND EXISTS "/opt/homebrew/opt/boost/include")
    set(Boost_INCLUDE_DIR "/opt/homebrew/opt/boost/include" CACHE PATH "" FORCE)
endif()

add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)

add_library(hambo SHARED
    src/memory.cpp
    src/cpu.cpp
    src/libc.cpp
    src/gl.cpp
    src/jni.cpp
    third_party/dlmalloc/dlmalloc.c
)

target_compile_options(hambo PRIVATE -O2 -Wall -Wno-unused-parameter)
target_include_directories(hambo PRIVATE
    ${Boost_INCLUDE_DIR}
    third_party/dlmalloc
)
set_source_files_properties(third_party/dlmalloc/dlmalloc.c PROPERTIES COMPILE_OPTIONS "-w")

target_link_libraries(hambo PRIVATE dynarmic merry::mcl log GLESv1_CM dl z)
target_link_options(hambo PRIVATE -Wl,-z,max-page-size=16384 -Wl,--exclude-libs,ALL)
