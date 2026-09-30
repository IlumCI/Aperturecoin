# Copyright (c) 2026 The ApertureCoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# ApertureMatMul v2 GPU kernels and self-test.
#   CUDA: cmake -S contrib/gpu-miner -B build-gpu && cmake --build build-gpu
#   HIP:  cmake -S contrib/gpu-miner -B build-gpu -DAPERTURE_GPU_HIP=ON
cmake_minimum_required(VERSION 3.22)
project(aperture_gpu LANGUAGES C CXX)

option(APERTURE_GPU_HIP "Build for AMD GPUs with HIP instead of CUDA" OFF)
get_filename_component(APERTURE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

if(APERTURE_GPU_HIP)
  enable_language(HIP)
  set_source_files_properties(src/aperture_gpu.cu PROPERTIES LANGUAGE HIP)
else()
  enable_language(CUDA)
  if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    # dp4a needs sm_61+, int8 WMMA needs sm_72+.
    set(CMAKE_CUDA_ARCHITECTURES 75 80 86 89 90)
  endif()
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_HIP_STANDARD 17)

add_library(aperture_blake3 STATIC src/blake3_unit.c)
target_include_directories(aperture_blake3 PUBLIC ${APERTURE_SRC}/src)

# The CPU reference and kernels come from the node sources, so the GPU path is
# always checked against exactly what consensus runs.
add_library(aperture_gpu STATIC
  src/aperture_gpu.cu
  ${APERTURE_SRC}/src/crypto/matmulpow_v2.cpp
  ${APERTURE_SRC}/src/crypto/matmulpow_v2_kernel.cpp
)
target_include_directories(aperture_gpu PUBLIC src ${APERTURE_SRC}/src)
target_link_libraries(aperture_gpu PUBLIC aperture_blake3)

add_executable(gpu_selftest src/gpu_selftest.cpp)
target_link_libraries(gpu_selftest aperture_gpu)
if(NOT APERTURE_GPU_HIP)
  find_package(CUDAToolkit REQUIRED)
  target_link_libraries(aperture_gpu PUBLIC CUDA::cudart)
endif()
