cmake_minimum_required(VERSION 3.16)
project(parquet_sweep CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Repo root, relative to benchmarks/parquet_sweep/
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

find_package(Arrow REQUIRED)
find_package(Parquet REQUIRED)
find_package(OpenMP REQUIRED)

add_executable(bench_parquet bench_parquet.cpp)

target_compile_options(bench_parquet PRIVATE -O3 -fpermissive -DOPENMP)

target_include_directories(bench_parquet PRIVATE
  ${REPO_ROOT}/format_conversion
  ${REPO_ROOT}/extern/graph-csr-openmp/inc
  ${REPO_ROOT}/extern/robin-hood-hashing/src/include
)

target_link_libraries(bench_parquet PRIVATE
  Arrow::arrow_shared
  Parquet::parquet_shared
  OpenMP::OpenMP_CXX
)

# Notes:
#  * Requires an Arrow/Parquet C++ install discoverable by find_package (set
#    ARROW_HOME / CMAKE_PREFIX_PATH if it lives in a non-standard prefix).
#  * Submodules must be checked out: git submodule update --init --recursive.
