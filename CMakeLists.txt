cmake_minimum_required(VERSION 3.13)
project(format_conversion CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(extern/pybind11)

find_package(OpenMP REQUIRED)
find_package(Arrow REQUIRED)
find_package(Parquet REQUIRED)
find_package(pybind11 REQUIRED)

add_compile_definitions(OPENMP)

pybind11_add_module(convert format_conversion/convert_wrap.cpp)
pybind11_add_module(partition format_conversion/partition_wrap.cpp)

target_include_directories(convert PRIVATE
    format_conversion
    extern/graph-openmp/inc      
)

target_include_directories(partition PRIVATE
    format_conversion
    extern/graph-openmp/inc      
)

# Use TLX as a CMake submodule
if(EXISTS "${PROJECT_SOURCE_DIR}/extern/tlx/CMakeLists.txt")
		add_subdirectory(extern/tlx)
else()
		message(FATAL_ERROR
				"Missing TLX library in extern/tlx "
				"Please run `git submodule update --init` to fetch the submodule.")
endif()

target_link_libraries(convert PRIVATE
    OpenMP::OpenMP_CXX
    Arrow::arrow_shared
    Parquet::parquet_shared
)

target_link_libraries(partition PRIVATE
    OpenMP::OpenMP_CXX
    tlx
)

target_compile_options(convert PRIVATE -O3 -march=native -fpermissive)
target_compile_options(partition PRIVATE -O3 -march=native -fpermissive)
