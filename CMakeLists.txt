cmake_minimum_required(VERSION 3.20)
project(tron_vanity_generator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- libsecp256k1 (third_party/secp256k1, git submodule/clone) ----
set(SECP256K1_INSTALL OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_CTIME_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ECDH OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_EXTRAKEYS OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_SCHNORRSIG OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_MUSIG OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_ELLSWIFT OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_SILENTPAYMENTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_RECOVERY OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/secp256k1 EXCLUDE_FROM_ALL)

# ---- 内核嵌入 ----
set(KERNEL_CL  ${CMAKE_CURRENT_SOURCE_DIR}/kernels/secp256k1_tron.cl)
set(KERNEL_GEN ${CMAKE_CURRENT_BINARY_DIR}/gen/kernel_src.h)
add_custom_command(
    OUTPUT  ${KERNEL_GEN}
    COMMAND ${CMAKE_COMMAND} -DIN=${KERNEL_CL} -DOUT=${KERNEL_GEN}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_kernel.cmake
    DEPENDS ${KERNEL_CL} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_kernel.cmake
    VERBATIM)

# ---- 主程序 ----
file(GLOB SRC CONFIGURE_DEPENDS src/*.cpp)
add_executable(tron_vanity_generator ${SRC} ${KERNEL_GEN})
target_include_directories(tron_vanity_generator PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_link_libraries(tron_vanity_generator PRIVATE secp256k1)

find_package(Threads REQUIRED)
target_link_libraries(tron_vanity_generator PRIVATE Threads::Threads)

if(MSVC)
  target_compile_options(tron_vanity_generator PRIVATE /O2 /EHsc /utf-8)
else()
  target_compile_options(tron_vanity_generator PRIVATE -O3 -march=native)
endif()
