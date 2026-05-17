cmake_minimum_required(VERSION 3.20)
project(microlisp_consumer LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(microlisp 0.1 REQUIRED)

add_executable(consumer consumer.c)
target_link_libraries(consumer PRIVATE microlisp::microlisp)

enable_testing()
add_test(NAME consumer_smoke COMMAND consumer)
set_tests_properties(consumer_smoke PROPERTIES
    PASS_REGULAR_EXPRESSION "result=6"
)
