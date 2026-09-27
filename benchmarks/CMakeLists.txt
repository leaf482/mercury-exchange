add_executable(mercury_bench engine_bench.cpp)
target_link_libraries(mercury_bench PRIVATE mercury benchmark::benchmark)
mercury_set_warnings(mercury_bench)

add_executable(mercury_latency latency_sample.cpp)
target_link_libraries(mercury_latency PRIVATE mercury)
mercury_set_warnings(mercury_latency)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(mercury_bench PRIVATE -Wno-c2y-extensions)
  target_compile_options(mercury_latency PRIVATE -Wno-c2y-extensions)
endif()
