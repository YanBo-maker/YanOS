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

option(YAN_BUILD_TOOLS "Build Guest validation tools" OFF)

# External validation layers. They stay optional: a missing dependency is
# reported as a skipped test, never as a pass.
set(YAN_NEMU_REF_SO "" CACHE FILEPATH
    "NEMU riscv32 reference shared object used for differential testing")
set(YAN_NEMU_REF_DIR "" CACHE PATH
    "NEMU source tree used to rebuild a deliberately broken reference model")
set(YAN_RISCV_PREFIX "" CACHE STRING
    "RISC-V bare-metal toolchain prefix, e.g. riscv64-unknown-elf-")
set(YAN_RISCV_TESTS_DIR "" CACHE PATH
    "riscv-tests checkout used for the official ISA suite")
set(YAN_RISCV_ARCH_TEST_DIR "" CACHE PATH
    "riscv-arch-test (ACT4) checkout used for the official test corpus")
set(YAN_SAIL_BIN "" CACHE FILEPATH
    "sail_riscv_sim used as the reference model for signature comparison")

add_library(yan_machine STATIC src/ram.c src/bus.c src/machine.c src/cpu.c src/csr.c
                              src/interrupt.c src/image.c src/difftest.c
                              src/uart.c src/transport.c)
target_include_directories(yan_machine PUBLIC include)
target_compile_options(yan_machine PRIVATE ${YAN_WARNINGS})

if(YAN_BUILD_TOOLS)
    add_executable(yan_run tools/yan_run.c tools/host_file.c)
    target_link_libraries(yan_run PRIVATE yan_machine)
    target_compile_options(yan_run PRIVATE ${YAN_WARNINGS})

    add_executable(yan_gen tools/yan_gen.c)
    target_compile_options(yan_gen PRIVATE ${YAN_WARNINGS})

    if(UNIX)
        # The differential tester loads the reference model at run time, which
        # is a POSIX facility, so it is built on Unix only.
        add_executable(yan_difftest tools/yan_difftest.c tools/host_file.c)
        target_link_libraries(yan_difftest PRIVATE yan_machine ${CMAKE_DL_LIBS})
        target_compile_options(yan_difftest PRIVATE ${YAN_WARNINGS})
    endif()
endif()

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

    foreach(suite ram bus machine cpu fetch step alu control memory csr trap m snapshot
                  image difftest interrupt clint plic boot uart transport)
        add_executable(test_${suite} tests/test_${suite}.c)
        target_link_libraries(test_${suite} PRIVATE yan_machine unity::framework)
        target_compile_options(test_${suite} PRIVATE ${YAN_WARNINGS})
        add_test(NAME ${suite} COMMAND test_${suite})
        set_tests_properties(${suite} PROPERTIES TIMEOUT 10)
    endforeach()

    # The boot suite compares the Guest-visible platform description with the
    # Host one, so it needs the Guest headers on its include path.
    target_include_directories(test_boot PRIVATE
                               "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest")

    # ---------------------------------------------------------------- external
    set(_yan_gcc_names riscv64-unknown-elf-gcc riscv32-unknown-elf-gcc
                       riscv64-linux-gnu-gcc)
    if(YAN_RISCV_PREFIX)
        list(INSERT _yan_gcc_names 0 "${YAN_RISCV_PREFIX}gcc")
    endif()
    unset(YAN_RISCV_GCC CACHE)
    find_program(YAN_RISCV_GCC NAMES ${_yan_gcc_names})

    # The Guest trap environment needs a RISC-V toolchain and the DUT executor,
    # but no reference model: it exercises CSR and interrupt semantics that
    # NEMU cannot mirror.
    if(YAN_BUILD_TOOLS AND UNIX AND YAN_RISCV_GCC)
        add_test(NAME guest_trap_env
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_trap_env.sh"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --work "${CMAKE_BINARY_DIR}/trap-env")
        set_tests_properties(guest_trap_env PROPERTIES SKIP_RETURN_CODE 77
                                                     TIMEOUT 300)
    endif()

    if(YAN_BUILD_TOOLS AND UNIX AND EXISTS "${YAN_NEMU_REF_SO}" AND YAN_RISCV_GCC)
        add_test(NAME difftest_corpus
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_corpus.sh"
                    --gcc "${YAN_RISCV_GCC}"
                    --gen "$<TARGET_FILE:yan_gen>"
                    --dut "$<TARGET_FILE:yan_difftest>"
                    --ref "${YAN_NEMU_REF_SO}"
                    --work "${CMAKE_BINARY_DIR}/guest-corpus"
                    --seeds 8)
        set_tests_properties(difftest_corpus PROPERTIES SKIP_RETURN_CODE 77
                                                     TIMEOUT 900)

        # The differential tester must fail on a planted defect, otherwise it
        # proves nothing. This test plants four known YanCPU bugs in a private
        # copy of the tree and requires each one to be reported.
        add_test(NAME difftest_mutation
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_mutation.sh"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --cc "${CMAKE_C_COMPILER}"
                    --gcc "${YAN_RISCV_GCC}"
                    --ref "${YAN_NEMU_REF_SO}"
                    --work "${CMAKE_BINARY_DIR}/mutation")
        set_tests_properties(difftest_mutation PROPERTIES SKIP_RETURN_CODE 77
                                                         TIMEOUT 900)

        # A trapped run is a category the reference cannot mirror; it must be
        # classified as such instead of being reported as a mismatch.
        add_test(NAME difftest_trap_scope
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_scope.sh"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --dut "$<TARGET_FILE:yan_difftest>"
                    --ref "${YAN_NEMU_REF_SO}"
                    --work "${CMAKE_BINARY_DIR}/trap-scope")
        set_tests_properties(difftest_trap_scope PROPERTIES SKIP_RETURN_CODE 77
                                                         TIMEOUT 300)

        # Breaking the DUT proves the tester can report a difference; breaking
        # the reference proves it actually consumes the reference state.
        if(EXISTS "${YAN_NEMU_REF_DIR}/src/isa/riscv32/inst.c")
            add_test(NAME difftest_ref_mutation
                COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_ref_mutation.sh"
                        --source "${CMAKE_CURRENT_SOURCE_DIR}"
                        --ref-dir "${YAN_NEMU_REF_DIR}"
                        --baseline-ref "${YAN_NEMU_REF_SO}"
                        --cc "${CMAKE_C_COMPILER}"
                        --gcc "${YAN_RISCV_GCC}"
                        --work "${CMAKE_BINARY_DIR}/ref-mutation")
            set_tests_properties(difftest_ref_mutation PROPERTIES
                                 SKIP_RETURN_CODE 77 TIMEOUT 1200)
        endif()
        message(STATUS "Differential testing enabled with ${YAN_NEMU_REF_SO}")
    else()
        message(STATUS "Differential testing not registered: "
                       "YAN_BUILD_TOOLS=${YAN_BUILD_TOOLS}, ref='${YAN_NEMU_REF_SO}', "
                       "riscv gcc='${YAN_RISCV_GCC}'")
    endif()

    if(YAN_BUILD_TOOLS AND UNIX AND YAN_RISCV_GCC AND
       EXISTS "${YAN_RISCV_TESTS_DIR}/isa/rv32ui")
        add_test(NAME official_riscv_tests
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/official/run_riscv_tests.sh"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --suite "${YAN_RISCV_TESTS_DIR}"
                    --work "${CMAKE_BINARY_DIR}/riscv-tests")
        set_tests_properties(official_riscv_tests PROPERTIES SKIP_RETURN_CODE 77
                                                           TIMEOUT 1800)
        message(STATUS "Official riscv-tests suite enabled from ${YAN_RISCV_TESTS_DIR}")
    endif()

    # Signature comparison: an independent reference model (Sail) computes the
    # expected signature of each image and the DUT must reproduce it byte for
    # byte. This is the flow the RISC-V architectural tests use.
    if(YAN_BUILD_TOOLS AND UNIX AND YAN_RISCV_GCC AND EXISTS "${YAN_SAIL_BIN}")
        add_test(NAME signature_corpus
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/official/run_signature.sh"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --gen "$<TARGET_FILE:yan_gen>"
                    --sail "${YAN_SAIL_BIN}"
                    --work "${CMAKE_BINARY_DIR}/signature"
                    --seeds 3)
        set_tests_properties(signature_corpus PROPERTIES SKIP_RETURN_CODE 77
                                                        TIMEOUT 1800)
        message(STATUS "Signature comparison enabled with ${YAN_SAIL_BIN}")
    endif()

    # The official ACT4 test corpus: an official test source is assembled here,
    # Sail computes the expected signature and the DUT must reproduce it.
    if(YAN_BUILD_TOOLS AND UNIX AND YAN_RISCV_GCC AND EXISTS "${YAN_SAIL_BIN}" AND
       EXISTS "${YAN_RISCV_ARCH_TEST_DIR}/tests/rv32i/I")
        add_test(NAME official_act4
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/official/run_act4.sh"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --sail "${YAN_SAIL_BIN}"
                    --suite "${YAN_RISCV_ARCH_TEST_DIR}"
                    --work "${CMAKE_BINARY_DIR}/act4")
        set_tests_properties(official_act4 PROPERTIES SKIP_RETURN_CODE 77
                                                     TIMEOUT 2400)
        message(STATUS "Official ACT4 corpus enabled from ${YAN_RISCV_ARCH_TEST_DIR}")
    endif()
endif()
