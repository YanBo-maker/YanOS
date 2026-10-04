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

# Guest applications have a production build even when tests are disabled.
set(_yan_gcc_names riscv64-unknown-elf-gcc riscv32-unknown-elf-gcc
                   riscv64-linux-gnu-gcc)
if(YAN_RISCV_PREFIX)
    list(INSERT _yan_gcc_names 0 "${YAN_RISCV_PREFIX}gcc")
endif()
unset(YAN_RISCV_GCC CACHE)
find_program(YAN_RISCV_GCC NAMES ${_yan_gcc_names})

add_library(yan_machine STATIC src/ram.c src/bus.c src/machine.c src/cpu.c src/csr.c
                              src/interrupt.c src/image.c src/difftest.c
                              src/uart.c src/transport.c)
target_include_directories(yan_machine PUBLIC include)
target_compile_options(yan_machine PRIVATE ${YAN_WARNINGS})

# The same freestanding filesystem core is used by Guest builds and native
# tests/tools; Host files and the transport adapter stay outside this library.
add_library(yan_fs STATIC os/yanfs.c)
target_include_directories(yan_fs PUBLIC os)
target_compile_options(yan_fs PRIVATE ${YAN_WARNINGS})

add_library(yan_shell STATIC os/shell.c)
target_link_libraries(yan_shell PUBLIC yan_fs)
target_compile_options(yan_shell PRIVATE ${YAN_WARNINGS})

add_library(yan_line STATIC os/line.c)
target_include_directories(yan_line PUBLIC os)
target_compile_options(yan_line PRIVATE ${YAN_WARNINGS})

if(YAN_BUILD_TOOLS)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(yan_mkfs tools/yan_mkfs.c)
        target_link_libraries(yan_mkfs PRIVATE yan_fs)
        target_compile_options(yan_mkfs PRIVATE ${YAN_WARNINGS})
    endif()
    add_executable(yan_run tools/yan_run.c tools/host_file.c tools/host_terminal.c
                            tools/host_block.c tools/host_disk.c)
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

if(YAN_BUILD_TOOLS AND UNIX AND YAN_RISCV_GCC)
    set(YAN_TERMINAL_ELF "${CMAKE_BINARY_DIR}/guest-apps/yanfs_terminal.elf")
    set(_yan_terminal_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/yanfs_terminal/main.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/trap_entry.S"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/task_switch.S"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/task.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/console.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/line.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/terminal.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/shell.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/block.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/yanfs.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/yanfs_block.c")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/os/memory.c")
        list(APPEND _yan_terminal_sources "${CMAKE_CURRENT_SOURCE_DIR}/os/memory.c")
    endif()
    set(_yan_terminal_headers)
    foreach(_header block console line platform shell task terminal yanfs yanfs_block)
        list(APPEND _yan_terminal_headers "${CMAKE_CURRENT_SOURCE_DIR}/os/${_header}.h")
    endforeach()
    add_custom_command(OUTPUT "${YAN_TERMINAL_ELF}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/guest-apps"
        COMMAND "${YAN_RISCV_GCC}" -std=c17 -march=rv32im -mabi=ilp32
                -mcmodel=medany -static -nostdlib -nostartfiles -ffreestanding
                -fno-builtin -fno-stack-protector -O2
                -Wall -Wextra -Wpedantic -Werror
                -I "${CMAKE_CURRENT_SOURCE_DIR}/os"
                -T "${CMAKE_CURRENT_SOURCE_DIR}/os/guest.ld"
                ${_yan_terminal_sources} -Wl,--build-id=none -o "${YAN_TERMINAL_ELF}"
        DEPENDS ${_yan_terminal_sources} ${_yan_terminal_headers}
                "${CMAKE_CURRENT_SOURCE_DIR}/os/guest.ld"
        VERBATIM)
    add_custom_target(yanfs_terminal ALL DEPENDS "${YAN_TERMINAL_ELF}")
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

    add_executable(test_yanfs tests/test_yanfs.c)
    target_link_libraries(test_yanfs PRIVATE yan_fs unity::framework)
    target_compile_options(test_yanfs PRIVATE ${YAN_WARNINGS})
    add_test(NAME yanfs COMMAND test_yanfs)
    set_tests_properties(yanfs PROPERTIES TIMEOUT 30)

    add_executable(test_shell tests/test_shell.c)
    target_link_libraries(test_shell PRIVATE yan_shell unity::framework)
    target_compile_options(test_shell PRIVATE ${YAN_WARNINGS})
    add_test(NAME shell COMMAND test_shell)
    set_tests_properties(shell PROPERTIES TIMEOUT 30)

    add_executable(test_line tests/test_line.c)
    target_link_libraries(test_line PRIVATE yan_line unity::framework)
    target_compile_options(test_line PRIVATE ${YAN_WARNINGS})
    add_test(NAME line COMMAND test_line)
    set_tests_properties(line PROPERTIES TIMEOUT 30)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Native platform/framing doubles exercise the actual adapter source;
        # real cooperative runtime integration is covered by guest_yanfs.
        add_executable(test_yanfs_adapter tests/test_yanfs_adapter.c)
        target_link_libraries(test_yanfs_adapter PRIVATE yan_fs unity::framework)
        target_compile_options(test_yanfs_adapter PRIVATE ${YAN_WARNINGS})
        add_test(NAME yanfs_adapter COMMAND test_yanfs_adapter)
        set_tests_properties(yanfs_adapter PROPERTIES TIMEOUT 30)
    endif()

    if(YAN_BUILD_TOOLS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(yan_mkfs_fault tools/yan_mkfs.c tests/yanfs_mkfs_fault.c)
        target_link_libraries(yan_mkfs_fault PRIVATE yan_fs)
        target_compile_options(yan_mkfs_fault PRIVATE ${YAN_WARNINGS})
        target_link_options(yan_mkfs_fault PRIVATE
                            -Wl,--wrap=fwrite -Wl,--wrap=fflush -Wl,--wrap=fclose
                            -Wl,--wrap=fseeko)
        # Test-only executor: inject a close error for one fixture image after
        # the real fclose, to exercise the final Host exit status after PASS.
        add_executable(yan_run_disk_close_fault tools/yan_run.c tools/host_file.c
                       tools/host_terminal.c tools/host_block.c tools/host_disk.c
                       tests/host_disk_close_fault.c)
        target_link_libraries(yan_run_disk_close_fault PRIVATE yan_machine)
        target_compile_options(yan_run_disk_close_fault PRIVATE ${YAN_WARNINGS})
        target_link_options(yan_run_disk_close_fault PRIVATE -Wl,--wrap=fclose)

        add_executable(test_host_disk tests/test_host_disk.c
                       tools/host_disk.c tools/host_block.c)
        target_include_directories(test_host_disk PRIVATE tools)
        target_link_libraries(test_host_disk PRIVATE yan_machine unity::framework)
        target_compile_options(test_host_disk PRIVATE ${YAN_WARNINGS})
        target_link_options(test_host_disk PRIVATE
                            -Wl,--wrap=fread -Wl,--wrap=fwrite
                            -Wl,--wrap=fflush -Wl,--wrap=fclose)
        add_test(NAME host_disk COMMAND test_host_disk)
        set_tests_properties(host_disk PROPERTIES TIMEOUT 30)
    endif()

    foreach(suite ram bus machine cpu fetch step alu control memory csr trap m snapshot
                  image difftest interrupt clint plic boot uart transport)
        add_executable(test_${suite} tests/test_${suite}.c)
        target_link_libraries(test_${suite} PRIVATE yan_machine unity::framework)
        target_compile_options(test_${suite} PRIVATE ${YAN_WARNINGS})
        add_test(NAME ${suite} COMMAND test_${suite})
        set_tests_properties(${suite} PROPERTIES TIMEOUT 10)
    endforeach()

    # The boot suite compares the Guest-visible platform description with the
    # Host one, so it needs the Guest headers on its include path. It also
    # compares os/platform.h against include/yan/uart.h, which is why the YanOS
    # layer is on the path too.
    target_include_directories(test_boot PRIVATE
                               "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest"
                               "${CMAKE_CURRENT_SOURCE_DIR}/os")

    # The persistent-combined gate's own negative controls need only Python:
    # both Guest processes are mocked, so they run on every machine and are not
    # tied to the cross toolchain. The case gate runs the same file before it
    # builds anything; registering it here puts it in the default suite as well.
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME shell_launcher
                COMMAND "${Python3_EXECUTABLE}" -B
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_shell_launcher.py"
                        --launcher "${CMAKE_CURRENT_SOURCE_DIR}/tools/yan_shell.py"
                        --work "${CMAKE_BINARY_DIR}/shell-launcher")
            set_tests_properties(shell_launcher PROPERTIES TIMEOUT 180)
            if(TARGET yanfs_terminal AND TARGET yan_mkfs AND TARGET yan_run_disk_close_fault)
                add_test(NAME guest_terminal_files
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_terminal_files.py"
                            --source "${CMAKE_CURRENT_SOURCE_DIR}"
                            --work "${CMAKE_BINARY_DIR}/guest-terminal-files"
                            --run "$<TARGET_FILE:yan_run>"
                            --mkfs "$<TARGET_FILE:yan_mkfs>"
                            --guest "${YAN_TERMINAL_ELF}"
                            --close-fault-run "$<TARGET_FILE:yan_run_disk_close_fault>")
                set_tests_properties(guest_terminal_files PROPERTIES TIMEOUT 600)
                set(_yan_terminal_runtime_args)
                if(YAN_ENABLE_SANITIZERS)
                    list(APPEND _yan_terminal_runtime_args --sanitizers)
                endif()
                add_test(NAME guest_terminal_runtime
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_terminal_runtime.py"
                            --source "${CMAKE_CURRENT_SOURCE_DIR}"
                            --work "${CMAKE_BINARY_DIR}/guest-terminal-runtime"
                            --cc "${CMAKE_C_COMPILER}"
                            --gcc "${YAN_RISCV_GCC}"
                            --guest "${YAN_TERMINAL_ELF}"
                            ${_yan_terminal_runtime_args})
                set_tests_properties(guest_terminal_runtime PROPERTIES
                                     SKIP_RETURN_CODE 77 TIMEOUT 600)
            endif()
        endif()
        add_test(NAME yanfs_mutation_gate
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_yanfs_mutation_gate.py")
        set_tests_properties(yanfs_mutation_gate PROPERTIES TIMEOUT 120)
        add_test(NAME terminal_mutation_gate
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_terminal_mutation_gate.py")
        set_tests_properties(terminal_mutation_gate PROPERTIES TIMEOUT 120)
        if(TARGET yan_mkfs_fault)
            add_test(NAME yanfs_mkfs
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_yanfs_mkfs.py"
                        --mkfs "$<TARGET_FILE:yan_mkfs>"
                        --fault-mkfs "$<TARGET_FILE:yan_mkfs_fault>"
                        --work "${CMAKE_BINARY_DIR}/yanfs-mkfs")
            set_tests_properties(yanfs_mkfs PROPERTIES TIMEOUT 120)
        endif()
        add_test(NAME persistent_combined_gate
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/test_persistent_combined_gate.py")
        set_tests_properties(persistent_combined_gate PROPERTIES TIMEOUT 120)
    endif()

    # ---------------------------------------------------------------- external
    # The Guest trap environment needs a RISC-V toolchain and the DUT executor,
    # but no reference model: it exercises CSR and interrupt semantics that
    # NEMU cannot mirror.
    if(YAN_BUILD_TOOLS AND UNIX AND YAN_RISCV_GCC)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        if(TARGET yan_mkfs)
            add_test(NAME guest_yanfs
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_yanfs.py"
                        --source "${CMAKE_CURRENT_SOURCE_DIR}"
                        --gcc "${YAN_RISCV_GCC}"
                        --run "$<TARGET_FILE:yan_run>"
                        --mkfs "$<TARGET_FILE:yan_mkfs>"
                        --work "${CMAKE_BINARY_DIR}/guest-yanfs")
            set_tests_properties(guest_yanfs PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
        endif()
        set(_yan_close_fault_args)
        if(TARGET yan_run_disk_close_fault)
            list(APPEND _yan_close_fault_args --run-close-fault
                 "$<TARGET_FILE:yan_run_disk_close_fault>")
        endif()
        add_test(NAME guest_persistent_block
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_persistent_block.py"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    ${_yan_close_fault_args}
                    --work "${CMAKE_BINARY_DIR}/persistent-block")
        set_tests_properties(guest_persistent_block PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

        # The persistent file backend and the cooperative runtime together: a
        # task issues a block write through the protocol, waits for it with
        # yan_os_task_wait() - once on the already-happened path and once inside
        # a real BLOCKED window built from the response ring's flow control - is
        # woken by the device interrupt, and a *second* simulator process reads
        # the same image back. The host compares the target block, every
        # neighbouring block and the length on its own, without either Guest.
        add_test(NAME guest_persistent_combined
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_persistent_combined.py"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --work "${CMAKE_BINARY_DIR}/persistent-combined")
        set_tests_properties(guest_persistent_combined PROPERTIES
                             SKIP_RETURN_CODE 77 TIMEOUT 300)

        add_test(NAME guest_trap_env
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_trap_env.sh"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --work "${CMAKE_BINARY_DIR}/trap-env")
        set_tests_properties(guest_trap_env PROPERTIES SKIP_RETURN_CODE 77
                                                     TIMEOUT 300)
    endif()

    # The console needs the UART terminal backend, which lives on the Host; the
    # Guest program drives the device directly so the test does not depend on
    # the console driver being finished.
    if(YAN_BUILD_TOOLS AND UNIX AND YAN_RISCV_GCC)
        add_test(NAME guest_terminal
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_terminal.sh"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --work "${CMAKE_BINARY_DIR}/terminal")
        set_tests_properties(guest_terminal PROPERTIES SKIP_RETURN_CODE 77
                                                       TIMEOUT 300)
    endif()

    # The executor's default path is frozen as a golden baseline: a deterministic
    # Guest probe plus its trace and signature bytes, compared byte for byte. A
    # change to the default path has to be seen and reviewed, not discovered
    # later. It needs the toolchain and the executor, nothing else.
    if(YAN_BUILD_TOOLS AND UNIX AND YAN_RISCV_GCC)
        add_test(NAME guest_golden
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_golden.sh"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --work "${CMAKE_BINARY_DIR}/golden")
        set_tests_properties(guest_golden PROPERTIES SKIP_RETURN_CODE 77
                                                     TIMEOUT 300)
    endif()

    # The console driver runs inside the Guest but is plain C, so its test
    # drives it twice: once through yan_run --terminal (the real path) and once
    # through a scripted UART backend that can refuse a byte or detach the
    # terminal mid-line, which the production tool cannot be asked to do.
    if(YAN_BUILD_TOOLS AND UNIX AND YAN_RISCV_GCC)
        add_test(NAME guest_console
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_console.sh"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --work "${CMAKE_BINARY_DIR}/console")
        set_tests_properties(guest_console PROPERTIES SKIP_RETURN_CODE 77
                                                      TIMEOUT 300)
    endif()

    # The cooperative runtime is Guest-only C and assembly, and its hardest case
    # is a property of timing rather than of a value: an interrupt delivered
    # between the predicate check and the switch that follows it. That is why the
    # suite carries its own scripted driver instead of going through yan_run:
    # the tool advances a whole step at a time and cannot be asked to inject a
    # device line at one chosen instruction.
    if(YAN_BUILD_TOOLS AND UNIX AND YAN_RISCV_GCC)
        add_test(NAME guest_runtime
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_runtime.sh"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --work "${CMAKE_BINARY_DIR}/runtime")
        set_tests_properties(guest_runtime PROPERTIES SKIP_RETURN_CODE 77
                                                      TIMEOUT 300)
    endif()

    # The block request protocol runs on the channel, so this suite has three
    # faces and the script drives all of them: a host-only unit face that can
    # place chosen bytes in a ring (which no guest can be asked to do), a guest
    # face that speaks the protocol end to end, and a pass through yan_run
    # --disk for the production path.
    if(YAN_BUILD_TOOLS AND UNIX AND YAN_RISCV_GCC)
        add_test(NAME guest_block
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_block.sh"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --work "${CMAKE_BINARY_DIR}/block")
        set_tests_properties(guest_block PROPERTIES SKIP_RETURN_CODE 77
                                                    TIMEOUT 300)
    endif()

    # The M2a acceptance: block I/O issued by a task and awaited through the
    # cooperative runtime, on the production path (yan_run --disk), with the
    # device served by the host pump and the wake delivered as a real interrupt.
    #
    # Its central assertion is not "the read worked" - that also passes when the
    # runtime is replaced by yield plus busy-wait polling, which 0019 forbids.
    # The assertion is that the waiting task's predicate is asked exactly once
    # while another task makes progress: with a real block the waiter does not
    # run at all, and with polling it runs once per round. The suite also stands
    # in for the fix to yan_run's doorbell handling: it frees the response ring
    # from a second task and never rings, so an edge-triggered pump strands the
    # request and the run reaches no verdict.
    if(YAN_BUILD_TOOLS AND UNIX AND YAN_RISCV_GCC)
        add_test(NAME guest_m2a_combined
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_m2a_combined.sh"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --work "${CMAKE_BINARY_DIR}/m2a-combined")
        set_tests_properties(guest_m2a_combined PROPERTIES SKIP_RETURN_CODE 77
                                                           TIMEOUT 300)
    endif()

    # Mutation checks live behind one switch, not one switch per component. Both
    # suites plant defects in a private copy, rebuild and run the tests there,
    # and count a defect as detected only when an assertion fails; crashes and
    # sanitizer reports do not count.
    #
    # Off by default because they dominate the suite: the console check takes
    # about 50 s and the device check about 290 s, against roughly 13 s for
    # everything else. They must be run explicitly before a merge, which
    # CONTRIBUTING records.
    # Mutation runs copy the tree, relink and rerun dozens of times, and that is
    # dominated by filesystem traffic: under WSL the same five mutants took 78 s
    # with scratch space on the Windows mount and 11 s on a native filesystem.
    # Their scratch space therefore lives outside the build tree on purpose.
    # One definition point, so a native-Linux checkout can move it in one place.
    set(YAN_MUTATION_WORK_ROOT "/tmp/yan-mutation")

    option(YAN_ENABLE_MUTATION_TESTS
           "Register the mutation checks with CTest (slow)" OFF)
    if(YAN_BUILD_TOOLS AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND YAN_ENABLE_MUTATION_TESTS)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME yanfs_mutation
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_yanfs_mutation.py"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --cc "${CMAKE_C_COMPILER}"
                    --unity "${unity_SOURCE_DIR}"
                    --work "${YAN_MUTATION_WORK_ROOT}/yanfs")
        set_tests_properties(yanfs_mutation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
        add_test(NAME persistent_block_mutation
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_persistent_mutation.py"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --cc "${CMAKE_C_COMPILER}"
                    --unity "${unity_SOURCE_DIR}"
                    --work "${CMAKE_BINARY_DIR}/persistent-mutation")
        set_tests_properties(persistent_block_mutation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

        # Planted-defect run for the combined persistent case. Each mutant
        # declares the exact check that owns it: fake-wait and yield-once are
        # caught by the writer's own window assertions, omit-write by the
        # writer's payload assertion when the read-back finds the seed, and
        # wrong-block only by the host's independent byte comparison (reads and
        # writes shift together, so the Guest reads its own shifted write back
        # and passes). A no-behaviour-change mutant must survive and a wait that
        # hangs is inconclusive, never detected; a reader panic or an unfitting
        # expectation is a harness error. The gate's verdict rules have their
        # own negative controls in persistent_combined_gate.
        add_test(NAME persistent_combined_mutation
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_persistent_combined.py"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --cc "${CMAKE_C_COMPILER}"
                    --work "${YAN_MUTATION_WORK_ROOT}/persistent-combined"
                    --mutation)
        set_tests_properties(persistent_combined_mutation PROPERTIES
                             SKIP_RETURN_CODE 77 TIMEOUT 600)
    endif()
    if(YAN_BUILD_TOOLS AND UNIX AND YAN_RISCV_GCC AND YAN_ENABLE_MUTATION_TESTS)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME terminal_mutation
                COMMAND "${Python3_EXECUTABLE}" -B
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_terminal_mutation.py"
                        --source "${CMAKE_CURRENT_SOURCE_DIR}"
                        --work "${YAN_MUTATION_WORK_ROOT}/terminal"
                        --cc "${CMAKE_C_COMPILER}"
                        --unity "${unity_SOURCE_DIR}"
                        --gcc "${YAN_RISCV_GCC}")
            set_tests_properties(terminal_mutation PROPERTIES
                                 SKIP_RETURN_CODE 77 TIMEOUT 900)
        endif()
        # Planted-defect run for the console: every mutation must be caught, so
        # a surviving defect fails this test rather than passing quietly.
        add_test(NAME guest_console_mutation
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_console.sh"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --work "${YAN_MUTATION_WORK_ROOT}/console"
                    --mutation --drive scripted)
        set_tests_properties(guest_console_mutation PROPERTIES SKIP_RETURN_CODE 77
                                                              TIMEOUT 600)

        # Planted-defect run for the devices and the PLIC gateway.
        add_test(NAME device_mutation
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_device_mutation.sh"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --work "${YAN_MUTATION_WORK_ROOT}/device"
                    --unity "${unity_SOURCE_DIR}"
                    --jobs 8)
        set_tests_properties(device_mutation PROPERTIES SKIP_RETURN_CODE 77
                                                        TIMEOUT 1800)

        # Planted-defect run for the cooperative runtime. The six defects are the
        # ones docs/specs/0019 names; the seventh the specification offers -
        # "restore an unconditional 1" - is an equivalent mutant under the
        # precondition the same specification requires, and is recorded as such
        # rather than counted as a survivor.
        add_test(NAME guest_runtime_mutation
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_runtime_mutation.sh"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --work "${YAN_MUTATION_WORK_ROOT}/runtime")
        set_tests_properties(guest_runtime_mutation PROPERTIES
                                                        SKIP_RETURN_CODE 77
                                                        TIMEOUT 900)

        # Planted-defect run for the block protocol: eight on the host side, five
        # in the guest frame layer. The gate counts only assertion failures - a
        # hang, a crash or a sanitizer report is a harness error, not a detection.
        add_test(NAME guest_block_mutation
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_block_mutation.sh"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --work "${YAN_MUTATION_WORK_ROOT}/block"
                    --cc "${CMAKE_C_COMPILER}")
        set_tests_properties(guest_block_mutation PROPERTIES SKIP_RETURN_CODE 77
                                                           TIMEOUT 900)

        # Planted-defect run for the combined case. fake-wait replaces the
        # runtime's wait with yield plus busy-wait polling and passes every
        # functional check - only the blocked-window assertion separates it from
        # the real thing, which is why that assertion is the one under test here.
        add_test(NAME guest_m2a_combined_mutation
            COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_m2a_combined.sh"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --work "${CMAKE_BINARY_DIR}/m2a-combined-mutation"
                    --mutation)
        set_tests_properties(guest_m2a_combined_mutation PROPERTIES
                                                                SKIP_RETURN_CODE 77
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
