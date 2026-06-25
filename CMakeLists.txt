cmake_minimum_required(VERSION 3.13)
project(format_conversion CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(PYBIND11_FINDPYTHON ON)

find_package(OpenMP REQUIRED)
find_package(pybind11 REQUIRED)

# Arrow/Parquet via pyarrow — must be in [build-system] requires so it is
# present in the build environment when cmake runs.
execute_process(
    COMMAND "${Python_EXECUTABLE}" -c
        "import pyarrow; print(pyarrow.get_library_dirs()[0])"
    OUTPUT_VARIABLE ARROW_LIB_DIR OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _arrow_result)
execute_process(
    COMMAND "${Python_EXECUTABLE}" -c
        "import pyarrow; print(pyarrow.get_include())"
    OUTPUT_VARIABLE ARROW_INCLUDE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _arrow_result EQUAL 0)
    message(FATAL_ERROR
        "pyarrow not found in build environment. "
        "Add pyarrow to [build-system] requires in pyproject.toml.")
endif()

add_compile_definitions(OPENMP)

pybind11_add_module(format format_conversion/format_wrap.cpp)

target_include_directories(format PRIVATE
    format_conversion
    extern/graph-csr-openmp/inc
    extern/robin-hood-hashing/src/include
    ${ARROW_INCLUDE_DIR}
)

target_link_libraries(format PRIVATE
    OpenMP::OpenMP_CXX
    -L${ARROW_LIB_DIR} -larrow -lparquet
)

target_compile_options(format PRIVATE -O3 -fpermissive -fopenmp)

set_target_properties(format PROPERTIES
    BUILD_RPATH "${ARROW_LIB_DIR}"
    INSTALL_RPATH "${ARROW_LIB_DIR}")
