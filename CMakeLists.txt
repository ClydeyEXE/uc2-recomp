cmake_minimum_required(VERSION 3.20)
project(ucc2_recomp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Toolchain smoke test and address-space probe; the recompiled game target goes here later.
add_executable(ucc2_smoke src/smoke.cpp)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    # Keep the exe clear of the XBE's load range (0x10000-0x47A000).
    target_link_options(ucc2_smoke PRIVATE /BASE:0x10000000 /FIXED /DYNAMICBASE:NO /LARGEADDRESSAWARE)
endif()
