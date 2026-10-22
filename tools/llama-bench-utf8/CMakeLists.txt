cmake_minimum_required(VERSION 3.24)
project(localcode_benchmark_launcher LANGUAGES CXX)

# Use the import library from the exact same llama.cpp build as the runtime.
# This target replaces only its tiny launcher, never an inference library.
set(LOCALCODE_BENCH_IMPL_LIBRARY "" CACHE FILEPATH "Matching llama-bench-impl import library")
if(NOT EXISTS "${LOCALCODE_BENCH_IMPL_LIBRARY}")
    message(FATAL_ERROR "Select the matching llama-bench-impl import library")
endif()
add_executable(localcode_bench_utf8 main.cpp)
set_target_properties(localcode_bench_utf8 PROPERTIES OUTPUT_NAME llama-bench)
target_compile_features(localcode_bench_utf8 PRIVATE cxx_std_17)
target_link_libraries(localcode_bench_utf8 PRIVATE "${LOCALCODE_BENCH_IMPL_LIBRARY}")
if(MSVC)
    target_compile_options(localcode_bench_utf8 PRIVATE /W4 /utf-8)
endif()
