cmake_minimum_required(VERSION 3.21)
project(YanOS LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MSVC)
    set(YAN_WARNINGS /W4 /WX)
else()
    set(YAN_WARNINGS -Wall -Wextra -Wpedantic -Werror)
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

add_library(yan_machine STATIC src/ram.c src/bus.c src/machine.c src/cpu.c)
target_include_directories(yan_machine PUBLIC include)
target_compile_options(yan_machine PRIVATE ${YAN_WARNINGS})

include(CTest)
if(BUILD_TESTING)
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    # Unity v2.6.1, pinned to its release commit and archive checksum.
    FetchContent_Declare(unity
        URL https://codeload.github.com/ThrowTheSwitch/Unity/tar.gz/cbcd08fa7de711053a3deec6339ee89cad5d2697
        URL_HASH SHA256=a197ea06556f524e378861e503fcfe2fe040447c7b90540ad86b653b13245ae4)
    FetchContent_MakeAvailable(unity)

    foreach(suite ram bus machine cpu fetch)
        add_executable(test_${suite} tests/test_${suite}.c)
        target_link_libraries(test_${suite} PRIVATE yan_machine unity::framework)
        target_compile_options(test_${suite} PRIVATE ${YAN_WARNINGS})
        add_test(NAME ${suite} COMMAND test_${suite})
        set_tests_properties(${suite} PROPERTIES TIMEOUT 10)
    endforeach()
endif()
