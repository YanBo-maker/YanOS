cmake_minimum_required(VERSION 3.21)
project(YanOS LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

option(YAN_ENABLE_SANITIZERS "Check address safety and undefined behavior" OFF)
if(YAN_ENABLE_SANITIZERS)
    if(UNIX AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                            -fno-sanitize-recover=all)
        add_link_options(-fsanitize=address,undefined -fno-sanitize-recover=all)
    else()
        message(FATAL_ERROR "Sanitizer builds require GCC or Clang on Unix")
    endif()
endif()

add_library(yan_machine STATIC src/ram.c)
target_include_directories(yan_machine PUBLIC include)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_ram tests/test_ram.c)
    target_link_libraries(test_ram PRIVATE yan_machine)
    add_test(NAME ram COMMAND test_ram)
    set_tests_properties(ram PROPERTIES TIMEOUT 10)
endif()
