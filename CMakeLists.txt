cmake_minimum_required(VERSION 3.5)
project(format_conversion CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(extern/pybind11)

find_package(OpenMP REQUIRED)
find_package(Arrow REQUIRED)
find_package(Parquet REQUIRED)

add_compile_definitions(OPENMP)

pybind11_add_module(_core src/convert_wrap.cpp)

target_include_directories(_core PRIVATE
    src                          
    extern/graph-openmp/inc      
)

target_link_libraries(_core PRIVATE
    OpenMP::OpenMP_CXX
    Arrow::arrow_shared
    Parquet::parquet_shared
)

target_compile_options(_core PRIVATE -O3 -march=native -fpermissive)
