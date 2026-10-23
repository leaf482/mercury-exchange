add_executable(demo demo.cpp)
target_link_libraries(demo PRIVATE mercury)
mercury_set_warnings(demo)
