cmake_minimum_required(VERSION 3.21)
project(YanOS LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MSVC)
    set(YAN_WARNINGS /W4 /WX /utf-8)
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
option(YAN_BUILD_SEMANTIC "Build optional Linux Host semantic-search tool" OFF)
set(YAN_MCP_SDK_PYTHON "" CACHE FILEPATH
    "Optional Python interpreter with official mcp==2.3.0 for mcp_sdk")


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
if(YAN_RISCV_GCC)
    get_filename_component(_yan_riscv_dir "${YAN_RISCV_GCC}" DIRECTORY)
    # An empty YAN_RISCV_PREFIX would expand to a bare `readelf`, which
    # find_program would resolve to the host tool -- wrong format for RISC-V.
    set(_yan_readelf_names riscv64-linux-gnu-readelf
        riscv64-unknown-elf-readelf riscv32-unknown-elf-readelf)
    if(YAN_RISCV_PREFIX)
        list(INSERT _yan_readelf_names 0 "${YAN_RISCV_PREFIX}readelf")
    endif()
    find_program(YAN_RISCV_READELF
                 NAMES ${_yan_readelf_names}
                 HINTS "${_yan_riscv_dir}")
    # Issue #106: the Guest ELF is made reproducible with `strip --strip-debug`.
    # NOT `-s`: the linker emits a FILE symbol per object named after its random
    # temp file (ccXXXXXX.o), and `-s` removes .symtab wholesale, which also
    # deletes `tohost` -- yan_run locates the halt word through
    # yan_image_find_symbol(), so a fully stripped ELF never halts (measured:
    # "stopped after 5000000 steps without reaching tohost"). --strip-debug
    # drops exactly the debug/FILE entries, leaving .symtab and `tohost` intact.
    # Same empty-prefix hazard as readelf above: a bare `strip` in NAMES would
    # match the host tool, which cannot read a RISC-V ELF.
    set(_yan_strip_names riscv64-linux-gnu-strip
        riscv64-unknown-elf-strip riscv32-unknown-elf-strip)
    if(YAN_RISCV_PREFIX)
        list(INSERT _yan_strip_names 0 "${YAN_RISCV_PREFIX}strip")
    endif()
    find_program(YAN_RISCV_STRIP
                 NAMES ${_yan_strip_names}
                 HINTS "${_yan_riscv_dir}")
endif()

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

# The unified knowledge-search facade plus its no-index linear backend. The
# facade is filesystem-independent; only the linear backend borrows a YanFs, so
# the whole library is the one link edge to yan_fs.
add_library(yan_search STATIC os/search.c os/search_linear.c)
target_link_libraries(yan_search PUBLIC yan_fs)
target_compile_options(yan_search PRIVATE ${YAN_WARNINGS})

# 0026 term search: the facade, the shared text primitives, the no-index linear
# backend and the in-memory index object. The facade is filesystem-independent;
# the linear and index backends borrow a YanFs, so the library is the one link
# edge to yan_fs.
add_library(yan_search_text STATIC os/search_text.c)
target_include_directories(yan_search_text PUBLIC os)
target_compile_options(yan_search_text PRIVATE ${YAN_WARNINGS})

add_library(yan_search_terms STATIC os/search_terms.c os/search_terms_linear.c
                                 os/search_terms_core.c
                                 os/search_terms_index.c)
target_include_directories(yan_search_terms PUBLIC os)
target_link_libraries(yan_search_terms PUBLIC yan_fs yan_search_text)
target_compile_options(yan_search_terms PRIVATE ${YAN_WARNINGS})

# The shell consumes the two unified facades (literal and term) and never
# instantiates or selects a backend, so it links the facades rather than yan_fs
# directly. 0026 adds the term facade for search/rebuild/index; 0027 adds the
# note-metadata service for the optional binding and its scratch types.
add_library(yan_shell STATIC os/shell.c)
target_link_libraries(yan_shell PUBLIC yan_search yan_search_terms yan_note)
target_compile_options(yan_shell PRIVATE ${YAN_WARNINGS})

add_library(yan_line STATIC os/line.c)
target_include_directories(yan_line PUBLIC os)
target_compile_options(yan_line PRIVATE ${YAN_WARNINGS})

add_library(yan_editor STATIC os/editor.c)
target_link_libraries(yan_editor PUBLIC yan_fs)
target_compile_options(yan_editor PRIVATE ${YAN_WARNINGS})

# 0027 pure note-metadata format: the shared info/filter/result types plus the
# bounded parser, the canonical encoder and the field validators. The library
# is deliberately filesystem-free and shell-free so the term facade can reuse
# YanNoteTagFilter and the same source still compiles for the Guest.
add_library(yan_note_format STATIC os/note_metadata_format.c)
target_include_directories(yan_note_format PUBLIC os)
target_compile_options(yan_note_format PRIVATE ${YAN_WARNINGS})
target_link_libraries(yan_search_terms PUBLIC yan_note_format)

# 0027 note-metadata service: the filesystem-observing half. It borrows a YanFs
# and reuses only the pure format types, so it links yan_fs and yan_note_format
# and stays independent of the shell, the console and the Host. It adds no
# on-disk format and no yan_fs_meta_* entry point.
add_library(yan_note STATIC os/note_metadata.c)
target_include_directories(yan_note PUBLIC os)
target_link_libraries(yan_note PUBLIC yan_note_format yan_fs)
target_compile_options(yan_note PRIVATE ${YAN_WARNINGS})

# 0028 read-only relation queries borrow YanFs and share only the pure format.
add_library(yan_relation STATIC os/note_relations.c)
target_include_directories(yan_relation PUBLIC os)
target_link_libraries(yan_relation PUBLIC yan_note_format yan_fs)
target_compile_options(yan_relation PRIVATE ${YAN_WARNINGS})
target_link_libraries(yan_shell PUBLIC yan_relation)

# 0032: pure streaming syntax and read-only navigation are independent of the
# shell; the shared UTF-8 library keeps the format target free of filesystem I/O.
add_library(yan_blocks_format STATIC os/note_blocks_format.c)
target_include_directories(yan_blocks_format PUBLIC os)
target_link_libraries(yan_blocks_format PUBLIC yan_note_format yan_search_text)
target_compile_options(yan_blocks_format PRIVATE ${YAN_WARNINGS})
add_library(yan_blocks STATIC os/note_blocks.c)
target_include_directories(yan_blocks PUBLIC os)
target_link_libraries(yan_blocks PUBLIC yan_blocks_format yan_fs)
target_compile_options(yan_blocks PRIVATE ${YAN_WARNINGS})
target_link_libraries(yan_shell PUBLIC yan_blocks)

# 0029 pure Host preprocessing has no model, HTTP or Guest dependency.
add_library(yan_semantic_text STATIC host/semantic_sha256.c host/semantic_text.c)
target_include_directories(yan_semantic_text PUBLIC host)
target_compile_options(yan_semantic_text PRIVATE ${YAN_WARNINGS})
add_library(yan_semantic_corpus STATIC host/semantic_corpus.c)
target_link_libraries(yan_semantic_corpus PUBLIC yan_semantic_text yan_note_format)
target_compile_options(yan_semantic_corpus PRIVATE ${YAN_WARNINGS})
add_library(yan_semantic_core STATIC host/semantic.c)
target_link_libraries(yan_semantic_core PUBLIC yan_semantic_corpus)
if(UNIX)
    target_link_libraries(yan_semantic_core PUBLIC m)
endif()
target_compile_options(yan_semantic_core PRIVATE ${YAN_WARNINGS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(yan_semantic_posix STATIC host/semantic_posix.c)
    target_link_libraries(yan_semantic_posix PUBLIC yan_semantic_core yan_fs)
    target_compile_options(yan_semantic_posix PRIVATE ${YAN_WARNINGS})
    # 0033 Task B Host query/paging; Linux-only because Task A's image adapter
    # is POSIX-only. This is the library seam for the later MCP worker, not a
    # block-only production executable.
    add_library(yan_block_query STATIC host/block_query.c)
    target_link_libraries(yan_block_query PUBLIC yan_semantic_posix yan_blocks)
    target_compile_options(yan_block_query PRIVATE ${YAN_WARNINGS})
    # 0035 PR-A Host write adapter: the side lock, the R7 preflight and the
    # atomic copy/rename path. The text qualification is the 0032 validator.
    add_library(yan_note_write STATIC host/note_write.c)
    target_include_directories(yan_note_write PUBLIC host)
    target_link_libraries(yan_note_write PUBLIC yan_fs yan_note_format yan_blocks_format)
    target_compile_options(yan_note_write PRIVATE ${YAN_WARNINGS})
    add_library(yan_image_migrate STATIC host/image_migrate.c)
    target_link_libraries(yan_image_migrate PUBLIC yan_note_write yan_semantic_posix)
    target_compile_options(yan_image_migrate PRIVATE ${YAN_WARNINGS})
endif()

if(YAN_BUILD_TOOLS)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(yan_mkfs tools/yan_mkfs.c)
        target_link_libraries(yan_mkfs PRIVATE yan_fs)
        target_compile_options(yan_mkfs PRIVATE ${YAN_WARNINGS})
        add_executable(yan_migrate tools/yan_migrate.c)
        target_link_libraries(yan_migrate PRIVATE yan_image_migrate)
        target_compile_options(yan_migrate PRIVATE ${YAN_WARNINGS})
    endif()
    add_executable(yan_run tools/yan_run.c tools/host_file.c tools/host_terminal.c
                            tools/host_block.c tools/host_disk.c)
    target_link_libraries(yan_run PRIVATE yan_machine)
    target_compile_options(yan_run PRIVATE ${YAN_WARNINGS})
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(yan_run PRIVATE yan_note_write)
    endif()

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
        "${CMAKE_CURRENT_SOURCE_DIR}/os/editor.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/search.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/search_linear.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/search_terms.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/note_metadata_format.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/note_metadata.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/note_relations.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/note_blocks_format.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/note_blocks.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/search_terms_linear.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/search_terms_core.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/search_terms_index.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/search_text.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/block.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/arena.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/yanfs.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/os/yanfs_block.c")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/os/memory.c")
        list(APPEND _yan_terminal_sources "${CMAKE_CURRENT_SOURCE_DIR}/os/memory.c")
    endif()
    set(_yan_terminal_headers)
    foreach(_header arena note_blocks note_blocks_format note_relations note_metadata note_metadata_format block console editor memory line platform search search_linear search_terms search_terms_core search_terms_index search_terms_linear search_text shell sort task terminal yanfs yanfs_block)
        list(APPEND _yan_terminal_headers "${CMAKE_CURRENT_SOURCE_DIR}/os/${_header}.h")
    endforeach()
    list(APPEND _yan_terminal_headers
         "${CMAKE_CURRENT_SOURCE_DIR}/os/block_peek.h"
         "${CMAKE_CURRENT_SOURCE_DIR}/os/note_metadata_format_internal.h")

    # The link and strip command lines live in variables so the build and the
    # reproducibility test (#106) share ONE definition. If the test re-spelled
    # them it could drift from what the build actually does and then "prove" a
    # property the real artefact does not have.
    set(YAN_TERMINAL_LINK_FLAGS
        -std=c17 -march=rv32im -mabi=ilp32
        -mcmodel=medany -static -nostdlib -nostartfiles -ffreestanding
        -fno-builtin -fno-stack-protector -O2
        -Wall -Wextra -Wpedantic -Werror)
    add_custom_command(OUTPUT "${YAN_TERMINAL_ELF}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/guest-apps"
        COMMAND "${YAN_RISCV_GCC}" ${YAN_TERMINAL_LINK_FLAGS}
                -I "${CMAKE_CURRENT_SOURCE_DIR}/os"
                -T "${CMAKE_CURRENT_SOURCE_DIR}/os/guest.ld"
                ${_yan_terminal_sources} -Wl,--build-id=none -o "${YAN_TERMINAL_ELF}.unstripped"
        # Issue #106: make the ELF byte-reproducible. `--strip-debug` removes
        # the linker's per-object FILE symbols (named after its random temp
        # files) and keeps .symtab, so `tohost` survives for yan_run; `-s`
        # would delete it and the Guest would never halt. Done here rather than
        # in the test so every consumer of build/ sees the same bytes.
        COMMAND "${YAN_RISCV_STRIP}" --strip-debug
                "${YAN_TERMINAL_ELF}.unstripped" -o "${YAN_TERMINAL_ELF}"
        COMMAND "${CMAKE_COMMAND}" -E remove -f "${YAN_TERMINAL_ELF}.unstripped"
        DEPENDS ${_yan_terminal_sources} ${_yan_terminal_headers}
                "${CMAKE_CURRENT_SOURCE_DIR}/os/guest.ld"
        VERBATIM)
    add_custom_target(yanfs_terminal ALL DEPENDS "${YAN_TERMINAL_ELF}")

    # Everything the reproducibility test needs to replay the same recipe in a
    # private directory. `__IN__` marks where the unstripped input goes, and
    # `__OUT__` where the result goes; the test substitutes both.
    string(REPLACE ";" " " _yan_link_flags_str "${YAN_TERMINAL_LINK_FLAGS}")
    string(REPLACE ";" " " _yan_sources_str "${_yan_terminal_sources}")
    set(YAN_TERMINAL_LINK_COMMAND
        "${YAN_RISCV_GCC} ${_yan_link_flags_str} -I ${CMAKE_CURRENT_SOURCE_DIR}/os -T ${CMAKE_CURRENT_SOURCE_DIR}/os/guest.ld ${_yan_sources_str} -Wl,--build-id=none -o __OUT__")
    set(YAN_TERMINAL_STRIP_COMMAND
        "${YAN_RISCV_STRIP} --strip-debug __IN__ -o __OUT__")
endif()

if(YAN_BUILD_SEMANTIC)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "YAN_BUILD_SEMANTIC requires Linux/WSL")
    endif()
    find_package(CURL CONFIG REQUIRED)
    find_package(yyjson CONFIG REQUIRED)
    add_library(yan_semantic_llama STATIC host/semantic_llama.c)
    target_link_libraries(yan_semantic_llama PUBLIC yan_semantic_core
                          PRIVATE CURL::libcurl yyjson::yyjson)
    target_compile_options(yan_semantic_llama PRIVATE ${YAN_WARNINGS})
    add_library(yan_semantic_json STATIC tools/semantic_json.c)
    target_link_libraries(yan_semantic_json PUBLIC yan_semantic_core PRIVATE yyjson::yyjson)
    target_compile_options(yan_semantic_json PRIVATE ${YAN_WARNINGS})
    add_executable(yan_semantic tools/yan_semantic.c)
    target_link_libraries(yan_semantic PRIVATE yan_semantic_posix yan_semantic_llama
                          yan_semantic_json yyjson::yyjson)
    target_compile_options(yan_semantic PRIVATE ${YAN_WARNINGS})
    add_library(yan_mcp_note STATIC tools/mcp_note.c)
    target_include_directories(yan_mcp_note PUBLIC tools)
    target_link_libraries(yan_mcp_note PUBLIC yan_note_write yan_semantic_posix
                          PRIVATE yyjson::yyjson)
    target_compile_options(yan_mcp_note PRIVATE ${YAN_WARNINGS})
    # 0027 metadata observation over MCP, gated by --allow-meta so the default
    # tools/list stays byte-identical to the 0033 six-tool array.
    add_library(yan_mcp_meta STATIC tools/mcp_meta.c)
    target_include_directories(yan_mcp_meta PUBLIC tools)
    target_link_libraries(yan_mcp_meta PUBLIC yan_note yan_semantic_posix
                          PRIVATE yyjson::yyjson)
    target_compile_options(yan_mcp_meta PRIVATE ${YAN_WARNINGS})
    add_library(yan_mcp_protocol STATIC tools/mcp_protocol.c)
    target_include_directories(yan_mcp_protocol PUBLIC tools)
    target_link_libraries(yan_mcp_protocol PUBLIC yan_semantic_text yan_blocks_format
                          yan_note_format yan_mcp_note yan_mcp_meta yyjson::yyjson)
    target_compile_options(yan_mcp_protocol PRIVATE ${YAN_WARNINGS})
    add_executable(yan_mcp tools/yan_mcp.c tools/mcp_worker.c)
    target_link_libraries(yan_mcp PRIVATE yan_mcp_protocol yan_semantic_posix yan_block_query
                          yan_semantic_llama yan_semantic_json yyjson::yyjson)
    target_compile_options(yan_mcp PRIVATE ${YAN_WARNINGS})
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

    add_executable(test_semantic_corpus tests/test_semantic_corpus.c)
    target_link_libraries(test_semantic_corpus PRIVATE yan_semantic_corpus unity::framework)
    target_compile_options(test_semantic_corpus PRIVATE ${YAN_WARNINGS})
    add_test(NAME semantic_corpus COMMAND test_semantic_corpus)
    set_tests_properties(semantic_corpus PROPERTIES TIMEOUT 30)
    add_executable(test_semantic_digest_alloc tests/test_semantic_digest_alloc.c)
    target_include_directories(test_semantic_digest_alloc PRIVATE host)
    target_link_libraries(test_semantic_digest_alloc PRIVATE
                          yan_semantic_text yan_note_format unity::framework)
    target_compile_options(test_semantic_digest_alloc PRIVATE ${YAN_WARNINGS})
    add_test(NAME semantic_digest_alloc COMMAND test_semantic_digest_alloc)
    set_tests_properties(semantic_digest_alloc PROPERTIES TIMEOUT 30
                        LABELS "0036-R4-AC4;0036-R6-AC1")

    add_executable(test_semantic tests/test_semantic.c)
    target_link_libraries(test_semantic PRIVATE yan_semantic_core unity::framework)
    target_compile_options(test_semantic PRIVATE ${YAN_WARNINGS})
    add_test(NAME semantic COMMAND test_semantic)
    set_tests_properties(semantic PROPERTIES TIMEOUT 30)
    add_executable(semantic_text_probe tests/semantic_text_probe.c)
    target_link_libraries(semantic_text_probe PRIVATE yan_semantic_text)
    target_compile_options(semantic_text_probe PRIVATE ${YAN_WARNINGS})
    add_executable(semantic_rank_probe tests/semantic_rank_probe.c)
    target_link_libraries(semantic_rank_probe PRIVATE yan_semantic_core)
    target_compile_options(semantic_rank_probe PRIVATE ${YAN_WARNINGS})
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME semantic_text_oracle COMMAND "${Python3_EXECUTABLE}" -B
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_text_oracle.py"
                 $<TARGET_FILE:semantic_text_probe>)
        set_tests_properties(semantic_text_oracle PROPERTIES TIMEOUT 120)
        add_test(NAME semantic_rank_oracle COMMAND "${Python3_EXECUTABLE}" -B
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_rank_oracle.py"
                 $<TARGET_FILE:semantic_rank_probe>)
        set_tests_properties(semantic_rank_oracle PROPERTIES TIMEOUT 120)
        add_test(NAME semantic_mutation_gate COMMAND "${Python3_EXECUTABLE}" -B
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_semantic_mutation_gate.py")
        set_tests_properties(semantic_mutation_gate PROPERTIES TIMEOUT 120)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME large_capacity_gate COMMAND "${Python3_EXECUTABLE}" -B
                     "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_large_capacity_gate.py")
            set_tests_properties(large_capacity_gate PROPERTIES TIMEOUT 30)
            add_test(NAME semantic_runner_pid COMMAND "${Python3_EXECUTABLE}" -B
                     "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_semantic_runner_pid.py")
            set_tests_properties(semantic_runner_pid PROPERTIES TIMEOUT 30)
        endif()
        if(YAN_BUILD_SEMANTIC)
            add_executable(test_mcp_protocol tests/test_mcp_protocol.c tools/mcp_worker.c)
            target_link_libraries(test_mcp_protocol PRIVATE yan_mcp_protocol yan_block_query
                                  yan_semantic_llama yan_semantic_json yyjson::yyjson
                                  unity::framework)
            target_compile_options(test_mcp_protocol PRIVATE ${YAN_WARNINGS})
            add_executable(test_mcp_block_worker tests/test_mcp_block_worker.c tools/mcp_worker.c)
            target_link_libraries(test_mcp_block_worker PRIVATE yan_mcp_protocol yan_block_query
                                  yan_semantic_llama yan_semantic_json yyjson::yyjson
                                  unity::framework)
            target_compile_options(test_mcp_block_worker PRIVATE ${YAN_WARNINGS})
            add_test(NAME mcp_block_worker COMMAND test_mcp_block_worker)
            set_tests_properties(mcp_block_worker PROPERTIES TIMEOUT 30)
            add_executable(test_mcp_note tests/test_mcp_note.c)
            target_link_libraries(test_mcp_note PRIVATE yan_mcp_protocol
                                  yan_note_write yan_semantic_posix yyjson::yyjson
                                  unity::framework)
            target_compile_options(test_mcp_note PRIVATE ${YAN_WARNINGS})
            add_test(NAME mcp_note_tool COMMAND test_mcp_note)
            set_tests_properties(mcp_note_tool PROPERTIES TIMEOUT 30 LABELS
                "0035-R4-AC4;0035-R6-AC3;0035-R7-AC1;0035-R10-AC5")
            add_test(NAME mcp_protocol COMMAND test_mcp_protocol)
            if(YAN_MCP_SDK_PYTHON)
                set(_mcp_sdk_python "${YAN_MCP_SDK_PYTHON}")
            else()
                set(_mcp_sdk_python "${Python3_EXECUTABLE}")
            endif()
            add_test(NAME mcp_sdk COMMAND "${_mcp_sdk_python}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_sdk_check.py"
                $<TARGET_FILE:yan_mcp> $<TARGET_FILE:yan_semantic>
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_fake_runner.py")
            set_tests_properties(mcp_sdk PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77
                LABELS "0035-R2-AC1;0035-R12-AC1")
            add_test(NAME mcp_stdio COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_checks.py"
                $<TARGET_FILE:yan_mcp> $<TARGET_FILE:yan_semantic>
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_fake_runner.py")
            set_tests_properties(mcp_protocol PROPERTIES TIMEOUT 120
                LABELS "0035-R12-AC1")
            set_tests_properties(mcp_stdio PROPERTIES TIMEOUT 120
                LABELS "0035-R1-AC1;0035-R1-AC2;0035-R12-AC1")
            add_executable(mcp_fault_probe tools/yan_mcp.c tools/mcp_worker.c tests/mcp_fault_hooks.c)
            target_compile_definitions(mcp_fault_probe PRIVATE _exit=yan_mcp_test_exit
                clock_gettime=yan_mcp_test_clock_gettime)
            target_link_libraries(mcp_fault_probe PRIVATE yan_mcp_protocol yan_semantic_posix
                                  yan_block_query yan_semantic_llama yan_semantic_json
                                  yyjson::yyjson)
            target_link_options(mcp_fault_probe PRIVATE -Wl,--wrap=yan_semantic_posix_cleanup
                                -Wl,--wrap=yan_semantic_llama_stop -Wl,--wrap=calloc
                                -Wl,--wrap=write -Wl,--wrap=read -Wl,--wrap=close -Wl,--wrap=fopen -Wl,--wrap=poll)
            target_compile_options(mcp_fault_probe PRIVATE ${YAN_WARNINGS})
            add_test(NAME mcp_faults COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_fault_checks.py"
                $<TARGET_FILE:yan_mcp> $<TARGET_FILE:mcp_fault_probe> $<TARGET_FILE:yan_semantic>
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_fake_runner.py")
            set_tests_properties(mcp_faults PROPERTIES TIMEOUT 120)
            add_test(NAME mcp_mutation_gate COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mcp_mutation_gate.py")
            set_tests_properties(mcp_mutation_gate PROPERTIES TIMEOUT 120)
            if(YAN_ENABLE_MUTATION_TESTS)
                add_test(NAME mcp_mutation COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_mcp_mutation.py"
                    --repo "${CMAKE_CURRENT_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}"
                    --unity "${unity_SOURCE_DIR}" --cc "${CMAKE_C_COMPILER}"
                    --yyjson-include "$<JOIN:$<TARGET_PROPERTY:yyjson::yyjson,INTERFACE_INCLUDE_DIRECTORIES>,|>"
                    --yyjson-lib "$<TARGET_FILE:yyjson::yyjson>"
                    --curl-lib "$<TARGET_FILE:CURL::libcurl>"
                    --work "${CMAKE_BINARY_DIR}/mcp-mutation")
                set_tests_properties(mcp_mutation PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77)
            endif()
            add_executable(test_semantic_json tests/test_semantic_json.c)
            target_include_directories(test_semantic_json PRIVATE tools)
            target_link_libraries(test_semantic_json PRIVATE yan_semantic_json yyjson::yyjson unity::framework)
            target_compile_options(test_semantic_json PRIVATE ${YAN_WARNINGS})
            add_test(NAME semantic_json_memory COMMAND test_semantic_json)
            add_executable(semantic_json_memory_probe tools/yan_semantic.c tests/semantic_json_memory.c)
            target_include_directories(semantic_json_memory_probe PRIVATE tools)
            target_compile_definitions(semantic_json_memory_probe PRIVATE calloc=yan_semantic_test_calloc)
            target_link_libraries(semantic_json_memory_probe PRIVATE yan_semantic_posix yan_semantic_llama
                                  yan_semantic_json yyjson::yyjson)
            target_compile_options(semantic_json_memory_probe PRIVATE ${YAN_WARNINGS})
            add_executable(semantic_provider_probe tests/semantic_provider_probe.c)
            target_link_libraries(semantic_provider_probe PRIVATE yan_semantic_llama)
            target_compile_options(semantic_provider_probe PRIVATE ${YAN_WARNINGS})
            add_test(NAME semantic_provider COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_provider_checks.py"
                $<TARGET_FILE:semantic_provider_probe>
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_fake_runner.py")
            set_tests_properties(semantic_provider PROPERTIES TIMEOUT 120)
            add_test(NAME semantic_cli COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_cli_checks.py"
                $<TARGET_FILE:yan_semantic>
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_fake_runner.py")
            set_tests_properties(semantic_cli PROPERTIES TIMEOUT 120)
            add_test(NAME semantic_json COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_json_checks.py"
                $<TARGET_FILE:yan_semantic>
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_fake_runner.py"
                $<TARGET_FILE:semantic_json_memory_probe>)
            set_tests_properties(semantic_json PROPERTIES TIMEOUT 120)
        endif()
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(test_semantic_posix tests/test_semantic_posix.c)
        target_link_libraries(test_semantic_posix PRIVATE yan_semantic_posix unity::framework)
        target_compile_options(test_semantic_posix PRIVATE ${YAN_WARNINGS})
        add_test(NAME semantic_posix COMMAND test_semantic_posix)
        set_tests_properties(semantic_posix PROPERTIES TIMEOUT 30)

        # 0035 PR-A: side lock, R7 preflight, atomic write and fault injection.
        add_executable(test_note_write tests/test_note_write.c)
        target_link_libraries(test_note_write PRIVATE yan_note_write
                              yan_semantic_posix unity::framework)
        target_compile_options(test_note_write PRIVATE ${YAN_WARNINGS})
        add_test(NAME note_write COMMAND test_note_write)
        set_tests_properties(note_write PROPERTIES TIMEOUT 120 LABELS
            "0035-R5-AC1;0035-R5-AC4;0035-R5-AC5;0035-R6-AC1;0035-R6-AC2;0035-R7-AC1")

        add_executable(test_note_write_faults tests/test_note_write_faults.c)
        target_link_libraries(test_note_write_faults PRIVATE yan_note_write
                              unity::framework)
        target_compile_options(test_note_write_faults PRIVATE ${YAN_WARNINGS})
        target_link_options(test_note_write_faults PRIVATE -Wl,--wrap=write
                            -Wl,--wrap=pwrite -Wl,--wrap=fsync -Wl,--wrap=rename)
        add_test(NAME note_write_faults COMMAND test_note_write_faults)
        set_tests_properties(note_write_faults PROPERTIES TIMEOUT 120 LABELS
            "0035-R6-AC1;0035-R6-AC3")

        if(Python3_Interpreter_FOUND AND TARGET yan_migrate)
            add_executable(yanfs_migrate_fault tests/yanfs_migrate_fault.c)
            target_link_libraries(yanfs_migrate_fault PRIVATE yan_image_migrate)
            target_compile_options(yanfs_migrate_fault PRIVATE ${YAN_WARNINGS})
            target_link_options(yanfs_migrate_fault PRIVATE
                -Wl,--wrap=open -Wl,--wrap=openat -Wl,--wrap=pwrite
                -Wl,--wrap=pread -Wl,--wrap=read -Wl,--wrap=write
                -Wl,--wrap=fsync -Wl,--wrap=close -Wl,--wrap=mkdirat
                -Wl,--wrap=unlinkat -Wl,--wrap=readdir)
            add_test(NAME yanfs_migrate_symbols
                COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_yanfs_migrate_symbols.py"
                    --nm "${CMAKE_NM}"
                    --migrate $<TARGET_FILE:yan_migrate>
                    --fault $<TARGET_FILE:yanfs_migrate_fault>
                    --link-options "$<TARGET_PROPERTY:yanfs_migrate_fault,LINK_OPTIONS>")
            set_tests_properties(yanfs_migrate_symbols PROPERTIES TIMEOUT 60
                LABELS "issue84;0036-R7-symbol-coverage")
            add_executable(yanfs_migrate_terms_probe tests/yanfs_migrate_terms_probe.c)
            target_link_libraries(yanfs_migrate_terms_probe PRIVATE
                                  yan_semantic_posix yan_search_terms)
            target_compile_options(yanfs_migrate_terms_probe PRIVATE ${YAN_WARNINGS})

            # The full Linux configuration verifies on-disk index reuse with the
            # repository fake runner; no local model or prebuilt bad candidate is
            # needed. The tools-only configuration explicitly skips that one case.
            set(_yan_migrate_semantic_args)
            if(TARGET yan_semantic)
                list(APPEND _yan_migrate_semantic_args
                    --semantic $<TARGET_FILE:yan_semantic>
                    --runner "${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_fake_runner.py"
                    --require-semantic)
            endif()
            add_test(NAME yanfs_migrate_acceptance
                COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_yanfs_migrate.py"
                    --migrate $<TARGET_FILE:yan_migrate>
                    --terms-probe $<TARGET_FILE:yanfs_migrate_terms_probe>
                    --fault $<TARGET_FILE:yanfs_migrate_fault>
                    --source-root "${CMAKE_CURRENT_SOURCE_DIR}"
                    --cc "${CMAKE_C_COMPILER}"
                    --require-detection --require-cli-fault
                    ${_yan_migrate_semantic_args}
                    --work "${CMAKE_BINARY_DIR}/yanfs-migrate")
            set_tests_properties(yanfs_migrate_acceptance PROPERTIES TIMEOUT 600
                SKIP_RETURN_CODE 77 LABELS "0036-R7-AC1;0036-R7-AC2")
            add_test(NAME yanfs_migrate_work_gate
                COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_yanfs_migrate_gate.py"
                    --migrate $<TARGET_FILE:yan_migrate>
                    --terms-probe $<TARGET_FILE:yanfs_migrate_terms_probe>
                    --fault $<TARGET_FILE:yanfs_migrate_fault>
                    --work "${CMAKE_BINARY_DIR}/yanfs-migrate-gate")
            set_tests_properties(yanfs_migrate_work_gate PROPERTIES TIMEOUT 300
                LABELS "0036-R7-AC2")
            add_test(NAME yanfs_migrate_symlink
                COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_yanfs_migrate_symlink.py"
                    --migrate $<TARGET_FILE:yan_migrate>
                    --work "${CMAKE_BINARY_DIR}/yanfs-migrate-symlink")
            set_tests_properties(yanfs_migrate_symlink PROPERTIES TIMEOUT 180
                LABELS "0036-R7-AC1;issue88")
            # Issue #83: deterministic post-copy asset EOF probe matrix.  The
            # old-RED mode is baseline evidence only and is deliberately not
            # registered here.
            add_test(NAME yanfs_migrate_eintr
                COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_yanfs_migrate_eintr.py"
                    --migrate $<TARGET_FILE:yan_migrate>
                    --fault $<TARGET_FILE:yanfs_migrate_fault>
                    --work "${CMAKE_BINARY_DIR}/yanfs-migrate-eintr")
            set_tests_properties(yanfs_migrate_eintr PROPERTIES TIMEOUT 300
                LABELS "0036-R7-AC1;0036-R7-AC2")
        endif()

        if(Python3_Interpreter_FOUND AND TARGET yan_run AND TARGET yan_mkfs
           AND TARGET yanfs_terminal)
            add_test(NAME yan_run_lock COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/note_write_lock_checks.py"
                $<TARGET_FILE:yan_run> $<TARGET_FILE:yan_mkfs>
                "${YAN_TERMINAL_ELF}")
            set_tests_properties(yan_run_lock PROPERTIES TIMEOUT 60
                SKIP_RETURN_CODE 77 LABELS "0035-R5-AC3")
        endif()

        # 0033 Task B: focused native query/paging suite and independent oracle.
        add_executable(test_block_query tests/test_block_query.c)
        target_link_libraries(test_block_query PRIVATE yan_block_query unity::framework)
        target_compile_options(test_block_query PRIVATE ${YAN_WARNINGS})
        add_test(NAME block_query COMMAND test_block_query)
        set_tests_properties(block_query PROPERTIES TIMEOUT 60)

        add_executable(block_query_probe tests/block_query_probe.c)
        target_link_libraries(block_query_probe PRIVATE yan_block_query)
        target_compile_options(block_query_probe PRIVATE ${YAN_WARNINGS})
        add_executable(note_read_probe tests/note_read_probe.c)
        target_link_libraries(note_read_probe PRIVATE yan_semantic_posix)
        target_compile_options(note_read_probe PRIVATE ${YAN_WARNINGS})
        add_library(mcp_note_fsync_fault SHARED tests/mcp_note_fsync_fault.c)
        target_link_libraries(mcp_note_fsync_fault PRIVATE ${CMAKE_DL_LIBS})
        target_compile_options(mcp_note_fsync_fault PRIVATE ${YAN_WARNINGS})
        if(Python3_Interpreter_FOUND AND TARGET yan_mcp AND TARGET yan_run
           AND TARGET yanfs_terminal AND TARGET note_read_probe
           AND TARGET mcp_note_fsync_fault)
            add_test(NAME mcp_note COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_note_checks.py"
                $<TARGET_FILE:yan_mcp> $<TARGET_FILE:yan_run>
                "${YAN_TERMINAL_ELF}" $<TARGET_FILE:note_read_probe>
                $<TARGET_FILE:mcp_note_fsync_fault>)
            set_tests_properties(mcp_note PROPERTIES TIMEOUT 240
                SKIP_RETURN_CODE 77 LABELS
                "0035-R3-AC1;0035-R3-AC2;0035-R3-AC3;0035-R3-AC4;0035-R4-AC1;0035-R4-AC2;0035-R4-AC3;0035-R4-AC4;0035-R5-AC1;0035-R5-AC2;0035-R5-AC5;0035-R6-AC2;0035-R6-AC3;0035-R10-AC1;0035-R10-AC2;0035-R10-AC3;0035-R10-AC4;0035-R11-AC1;0035-R11-AC2;0035-R11-AC3;0035-R11-AC4")
        endif()
        # 0041-R1: the gated yanos_note_meta tool. This drives a real yan_mcp
        # over stdio and is the only automated guard on the four-state
        # tools/list contract, so it must stay registered whenever yan_mcp is
        # built.
        if(Python3_Interpreter_FOUND AND TARGET yan_mcp)
            add_test(NAME mcp_meta COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_meta_checks.py"
                $<TARGET_FILE:yan_mcp>)
            set_tests_properties(mcp_meta PROPERTIES TIMEOUT 240
                SKIP_RETURN_CODE 77 LABELS
                "0041-R1-AC1;0041-R1-AC2;0041-R1-AC3;0041-R1-AC4;0041-R1-AC5;0041-R1-AC6;0041-R1-AC7;0041-R1-AC8;0041-R1-AC9;0041-R1-AC10;0041-R1-AC11")
        endif()
        if(Python3_Interpreter_FOUND AND TARGET yan_mcp)
            # Two shapes are registered on purpose:
            #   mcp_note_list        every case in one run (the aggregate)
            #   mcp_note_list_ac<n>  only the functions that guard AC<n>
            # A single test carrying eleven AC labels cannot prove any of them:
            # the label selects the whole script, so deleting one AC's
            # assertions still leaves a green `ctest -L 0041-R2-AC<n>`. The
            # per-AC entries pass `--case`, which runs just that AC's functions
            # and exits non-zero on an unknown name, so a mislabelled AC fails
            # instead of passing empty.
            # CTest's -L is an UNANCHORED regex: a bare query for AC1 also
            # matches AC10 and AC11. Use -L '^0041-R2-AC1$' when the question
            # is "did exactly this AC run". The aggregate entry keeps only the
            # 0027-list label, so an AC query never falls back to it.
            add_test(NAME mcp_note_list COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_note_list_checks.py"
                $<TARGET_FILE:yan_mcp>)
            set_tests_properties(mcp_note_list PROPERTIES TIMEOUT 240
                SKIP_RETURN_CODE 77 LABELS "0027-list")
            foreach(_note_list_ac RANGE 1 11)
                add_test(NAME mcp_note_list_ac${_note_list_ac}
                    COMMAND "${Python3_EXECUTABLE}" -B
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_note_list_checks.py"
                        $<TARGET_FILE:yan_mcp>
                        --case 0041-R2-AC${_note_list_ac})
                set_tests_properties(mcp_note_list_ac${_note_list_ac}
                    PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
                        LABELS "0027-list;0041-R2-AC${_note_list_ac}")
            endforeach()
        endif()
        if(Python3_Interpreter_FOUND)
            add_test(NAME block_query_oracle COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/block_page_oracle.py"
                $<TARGET_FILE:block_query_probe>)
            set_tests_properties(block_query_oracle PROPERTIES TIMEOUT 180)
        endif()
    endif()

    foreach(_semantic_suite sha256 text)
        add_executable(test_semantic_${_semantic_suite}
                       tests/test_semantic_${_semantic_suite}.c)
        target_link_libraries(test_semantic_${_semantic_suite} PRIVATE
                             yan_semantic_text unity::framework)
        target_compile_options(test_semantic_${_semantic_suite} PRIVATE ${YAN_WARNINGS})
        add_test(NAME semantic_${_semantic_suite} COMMAND test_semantic_${_semantic_suite})
        set_tests_properties(semantic_${_semantic_suite} PROPERTIES TIMEOUT 30)
    endforeach()

    add_executable(test_yanfs tests/test_yanfs.c)
    target_link_libraries(test_yanfs PRIVATE yan_fs unity::framework)
    target_compile_options(test_yanfs PRIVATE ${YAN_WARNINGS})
    add_test(NAME yanfs COMMAND test_yanfs)
    set_tests_properties(yanfs PROPERTIES TIMEOUT 30)

    # 0036-R3-AC1: the one-shot bump arena is plain C over uintptr_t, so the
    # Guest's allocation rule is also a native Unity suite.
    add_executable(test_arena tests/test_arena.c os/arena.c)
    target_include_directories(test_arena PRIVATE os)
    target_link_libraries(test_arena PRIVATE unity::framework)
    target_compile_options(test_arena PRIVATE ${YAN_WARNINGS})
    add_test(NAME arena COMMAND test_arena)
    set_tests_properties(arena PROPERTIES TIMEOUT 30 LABELS "0036-R3-AC1")

    # Issue 78/79 support: the Host observer's entry/return state machine.
    # This is a support control, not a claim that the whole AC is covered here.
    add_executable(test_arena_trace tests/test_arena_trace.c tests/arena_trace.c)
    target_include_directories(test_arena_trace PRIVATE
                               "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(test_arena_trace PRIVATE yan_machine unity::framework)
    target_compile_options(test_arena_trace PRIVATE ${YAN_WARNINGS})
    add_test(NAME arena_trace COMMAND test_arena_trace)
    set_tests_properties(arena_trace PROPERTIES TIMEOUT 30
                         LABELS "issue78;issue79;support")

    add_executable(test_sort_index tests/test_sort_index.c)
    target_include_directories(test_sort_index PRIVATE os)
    target_compile_options(test_sort_index PRIVATE ${YAN_WARNINGS})
    add_test(NAME sort_index COMMAND test_sort_index)
    set_tests_properties(sort_index PROPERTIES TIMEOUT 120 LABELS "0036-R8-AC3")

    add_executable(test_ring_copy tests/test_ring_copy.c)
    target_include_directories(test_ring_copy PRIVATE os)
    target_compile_options(test_ring_copy PRIVATE ${YAN_WARNINGS})
    add_test(NAME ring_copy COMMAND test_ring_copy)
    set_tests_properties(ring_copy PROPERTIES TIMEOUT 120 LABELS "0036-R8-AC1")

    # Compile the real freestanding helpers under distinct names. Replacing
    # libc's symbols here would also replace the host runtime and the oracle.
    add_library(yan_guest_memory_test_impl OBJECT os/memory.c)
    target_compile_definitions(yan_guest_memory_test_impl PRIVATE
        memcpy=yan_guest_memcpy memmove=yan_guest_memmove memset=yan_guest_memset
        memcmp=yan_guest_memcmp strlen=yan_guest_strlen)
    target_compile_options(yan_guest_memory_test_impl PRIVATE ${YAN_WARNINGS})
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(yan_guest_memory_test_impl PRIVATE -fno-builtin)
    endif()
    add_executable(test_guest_memory tests/test_guest_memory.c
        $<TARGET_OBJECTS:yan_guest_memory_test_impl>)
    target_compile_options(test_guest_memory PRIVATE ${YAN_WARNINGS})
    add_test(NAME os_memory_helpers COMMAND test_guest_memory)
    set_tests_properties(os_memory_helpers PROPERTIES TIMEOUT 60
                             LABELS "0036-R8-AC1;0040-R2-AC1")
    if(YAN_ENABLE_SANITIZERS)
        target_compile_options(yan_guest_memory_test_impl PRIVATE
            -fsanitize=address,pointer-compare)
        target_link_options(test_guest_memory PRIVATE -fsanitize=address,pointer-compare)
        set_tests_properties(os_memory_helpers PROPERTIES
            ENVIRONMENT "ASAN_OPTIONS=detect_invalid_pointer_pairs=2:detect_leaks=1")
    endif()

    # 0036-R2: multi-block v2 directory. The suite allocates the memory device
    # and the external directory with calloc, so it runs the same contract the
    # Guest uses now that 0036-R3 introduces the arena.
    add_executable(test_yanfs_v2 tests/test_yanfs_v2.c)
    target_link_libraries(test_yanfs_v2 PRIVATE yan_fs unity::framework)
    target_compile_options(test_yanfs_v2 PRIVATE ${YAN_WARNINGS})
    add_test(NAME yanfs_v2 COMMAND test_yanfs_v2)
    set_tests_properties(yanfs_v2 PROPERTIES TIMEOUT 900
                         LABELS "0036-R2-AC1;0036-R2-AC2;0036-R2-AC3")

    add_executable(test_yanfs_name_order tests/test_yanfs_name_order.c os/yanfs.c)
    target_include_directories(test_yanfs_name_order PRIVATE os)
    target_compile_definitions(test_yanfs_name_order PRIVATE YAN_FS_TEST_HOOKS)
    target_link_libraries(test_yanfs_name_order PRIVATE unity::framework)
    target_compile_options(test_yanfs_name_order PRIVATE ${YAN_WARNINGS})
    add_test(NAME yanfs_name_order COMMAND test_yanfs_name_order)
    set_tests_properties(yanfs_name_order PROPERTIES TIMEOUT 600 LABELS "0036-R8-AC2")

    # 0026 source-identity exhaustion: the same os/yanfs.c compiled a second time
    # with a near-UINT64_MAX allocator seed. It builds its own copy of the source
    # instead of linking yan_fs, because linking both would define yanfs.c twice.
    # The seed is a plain integer literal so MSVC's /D parses it the same way as
    # GCC's -D; the suite is registered on every platform, not only Linux.
    add_executable(test_yanfs_source_exhaustion
                   tests/test_yanfs_source_exhaustion.c os/yanfs.c)
    target_include_directories(test_yanfs_source_exhaustion PRIVATE os)
    target_compile_definitions(test_yanfs_source_exhaustion PRIVATE
        "YAN_FS_SOURCE_TOKEN_SEED=18446744073709551551ULL")
    target_link_libraries(test_yanfs_source_exhaustion PRIVATE unity::framework)
    target_compile_options(test_yanfs_source_exhaustion PRIVATE ${YAN_WARNINGS})
    add_test(NAME yanfs_source_exhaustion COMMAND test_yanfs_source_exhaustion)
    set_tests_properties(yanfs_source_exhaustion PROPERTIES TIMEOUT 30)

    add_executable(test_shell tests/test_shell.c)
    target_link_libraries(test_shell PRIVATE yan_shell unity::framework)
    target_compile_options(test_shell PRIVATE ${YAN_WARNINGS})
    add_test(NAME shell COMMAND test_shell)
    set_tests_properties(shell PROPERTIES TIMEOUT 30)

    # 0026 CLI front ends over a real injected index backend plus a fake term
    # backend for precise output/fault/reentry fixtures.
    add_executable(test_shell_terms tests/test_shell_terms.c)
    target_link_libraries(test_shell_terms PRIVATE yan_shell unity::framework)
    target_compile_options(test_shell_terms PRIVATE ${YAN_WARNINGS})
    add_test(NAME shell_terms COMMAND test_shell_terms)
    set_tests_properties(shell_terms PROPERTIES TIMEOUT 30)

    # 0027 shell binding, metadata grammar and output over a mounted YanFS.
    add_executable(test_shell_metadata tests/test_shell_metadata.c)
    target_link_libraries(test_shell_metadata PRIVATE yan_shell
                                                      unity::framework)
    target_compile_options(test_shell_metadata PRIVATE ${YAN_WARNINGS})
    add_test(NAME shell_metadata COMMAND test_shell_metadata)
    set_tests_properties(shell_metadata PROPERTIES TIMEOUT 60)

    add_executable(test_line tests/test_line.c)
    target_link_libraries(test_line PRIVATE yan_line unity::framework)
    target_compile_options(test_line PRIVATE ${YAN_WARNINGS})
    add_test(NAME line COMMAND test_line)
    set_tests_properties(line PROPERTIES TIMEOUT 30)

    add_executable(test_editor tests/test_editor.c)
    target_link_libraries(test_editor PRIVATE yan_editor unity::framework)
    target_compile_options(test_editor PRIVATE ${YAN_WARNINGS})
    add_test(NAME editor COMMAND test_editor)
    set_tests_properties(editor PROPERTIES TIMEOUT 30)

    add_executable(test_search tests/test_search.c)
    target_link_libraries(test_search PRIVATE yan_search unity::framework)
    target_compile_options(test_search PRIVATE ${YAN_WARNINGS})
    add_test(NAME search COMMAND test_search)
    set_tests_properties(search PROPERTIES TIMEOUT 30)

    # 0026 term search: facade lifecycle plus the shared streaming core over a
    # real in-memory YanFS, with a fake backend for the pure facade contracts.
    add_executable(test_search_terms tests/test_search_terms.c)
    target_link_libraries(test_search_terms PRIVATE yan_search_terms
                                                    unity::framework)
    target_compile_options(test_search_terms PRIVATE ${YAN_WARNINGS})
    add_test(NAME search_terms COMMAND test_search_terms)
    set_tests_properties(search_terms PROPERTIES TIMEOUT 30)

    # 0027 filtered qualification and ranking over a real in-memory YanFS.
    add_executable(test_search_metadata tests/test_search_metadata.c)
    target_link_libraries(test_search_metadata PRIVATE yan_search_terms
                                                      unity::framework)
    target_compile_options(test_search_metadata PRIVATE ${YAN_WARNINGS})
    add_test(NAME search_metadata COMMAND test_search_metadata)
    set_tests_properties(search_metadata PROPERTIES TIMEOUT 60)

    # 0027 pure header parser, canonical encoder and field boundaries.
    add_executable(test_note_metadata_format tests/test_note_metadata_format.c)
    target_link_libraries(test_note_metadata_format PRIVATE yan_note_format
                                                          unity::framework)
    target_compile_options(test_note_metadata_format PRIVATE ${YAN_WARNINGS})
    add_test(NAME note_metadata_format COMMAND test_note_metadata_format)
    set_tests_properties(note_metadata_format PROPERTIES TIMEOUT 30)

    add_executable(test_note_relations tests/test_note_relations.c)

    add_executable(test_note_blocks_format tests/test_note_blocks_format.c)
    target_link_libraries(test_note_blocks_format PRIVATE yan_blocks_format unity::framework)
    target_compile_options(test_note_blocks_format PRIVATE ${YAN_WARNINGS})
    add_test(NAME note_blocks_format COMMAND test_note_blocks_format)
    set_tests_properties(note_blocks_format PROPERTIES TIMEOUT 30
                         LABELS "0040-R3-AC1")
    add_executable(test_note_blocks tests/test_note_blocks.c)
    target_link_libraries(test_note_blocks PRIVATE yan_blocks unity::framework)
    target_compile_options(test_note_blocks PRIVATE ${YAN_WARNINGS})
    add_test(NAME note_blocks COMMAND test_note_blocks)
    set_tests_properties(note_blocks PROPERTIES TIMEOUT 60)
    add_executable(test_shell_blocks tests/test_shell_blocks.c)
    target_link_libraries(test_shell_blocks PRIVATE yan_shell unity::framework)
    target_compile_options(test_shell_blocks PRIVATE ${YAN_WARNINGS})
    add_test(NAME shell_blocks COMMAND test_shell_blocks)
    set_tests_properties(shell_blocks PROPERTIES TIMEOUT 60)
    target_link_libraries(test_note_relations PRIVATE yan_relation unity::framework)
    target_compile_options(test_note_relations PRIVATE ${YAN_WARNINGS})
    add_test(NAME note_relations COMMAND test_note_relations)
    set_tests_properties(note_relations PROPERTIES TIMEOUT 30)
    add_executable(test_note_relations_source_exhaustion
                   tests/test_note_relations_source_exhaustion.c
                   os/note_relations.c os/yanfs.c)
    target_include_directories(test_note_relations_source_exhaustion PRIVATE os)
    target_compile_definitions(test_note_relations_source_exhaustion PRIVATE
        "YAN_FS_SOURCE_TOKEN_SEED=18446744073709551615ULL")
    target_link_libraries(test_note_relations_source_exhaustion PRIVATE yan_note_format unity::framework)
    target_compile_options(test_note_relations_source_exhaustion PRIVATE ${YAN_WARNINGS})
    add_test(NAME note_relations_source_exhaustion COMMAND test_note_relations_source_exhaustion)
    set_tests_properties(note_relations_source_exhaustion PROPERTIES TIMEOUT 30)
    add_executable(test_shell_relations tests/test_shell_relations.c)
    target_link_libraries(test_shell_relations PRIVATE yan_shell unity::framework)
    target_compile_options(test_shell_relations PRIVATE ${YAN_WARNINGS})
    add_test(NAME shell_relations COMMAND test_shell_relations)
    set_tests_properties(shell_relations PROPERTIES TIMEOUT 30)

    # 0027 implemented service suite, introduced against RED stubs. It drives
    # the service over a real mounted YanFS on a caller-owned static device.
    add_executable(test_note_metadata tests/test_note_metadata.c)
    target_link_libraries(test_note_metadata PRIVATE yan_note unity::framework)
    target_compile_options(test_note_metadata PRIVATE ${YAN_WARNINGS})
    add_test(NAME note_metadata COMMAND test_note_metadata)
    set_tests_properties(note_metadata PROPERTIES TIMEOUT 60)
    add_executable(test_note_metadata_faults tests/test_note_metadata_faults.c)
    target_link_libraries(test_note_metadata_faults PRIVATE yan_note unity::framework)
    target_compile_options(test_note_metadata_faults PRIVATE ${YAN_WARNINGS})
    add_test(NAME note_metadata_faults COMMAND test_note_metadata_faults)
    set_tests_properties(note_metadata_faults PROPERTIES TIMEOUT 30)

    # 0026 in-memory index contract: the index object, its vtable seam and the
    # approved physical caps over a real 8 MiB in-memory YanFS. Indexed-only
    # expectations exercise the implemented index; SCAN parity and the pure
    # lifecycle/alias cases guard the same public contract.
    add_executable(test_search_index tests/test_search_index.c)
    target_link_libraries(test_search_index PRIVATE yan_search_terms
                                                   unity::framework)
    target_compile_options(test_search_index PRIVATE ${YAN_WARNINGS})
    add_test(NAME search_index COMMAND test_search_index)
    set_tests_properties(search_index PROPERTIES TIMEOUT 60)

    # 0026 index source-exhaustion variant: os/yanfs.c compiled a second time
    # with a near-UINT64_MAX allocator seed instead of linking yan_fs (which
    # would define yanfs.c twice). It builds its own copy of the production term
    # sources plus Unity and is registered on every platform.
    add_executable(test_search_index_exhaustion
                   tests/test_search_index_exhaustion.c os/yanfs.c
                   os/search_terms.c os/search_terms_linear.c
                   os/search_terms_core.c os/search_text.c
                   os/search_terms_index.c)
    target_include_directories(test_search_index_exhaustion PRIVATE os)
    target_compile_definitions(test_search_index_exhaustion PRIVATE
        "YAN_FS_SOURCE_TOKEN_SEED=18446744073709551551ULL")
    target_link_libraries(test_search_index_exhaustion PRIVATE yan_note_format unity::framework)
    target_compile_options(test_search_index_exhaustion PRIVATE ${YAN_WARNINGS})
    add_test(NAME search_index_exhaustion
             COMMAND test_search_index_exhaustion)
    set_tests_properties(search_index_exhaustion PROPERTIES TIMEOUT 60)

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
        target_link_libraries(yan_run_disk_close_fault PRIVATE yan_machine
                              yan_note_write)
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
        # issue #89: harness controls only. The historical cross-version run is
        # operator-explicit; this gate proves the fixture/comparator checks
        # without Git history, a cross compiler, a model, or MCP dependencies.
        add_test(NAME yanfs_r8_parity_harness
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_yanfs_r8_parity.py")
        set_tests_properties(yanfs_r8_parity_harness PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 60
            LABELS "issue89;0036-R8-AC1-harness")
        add_test(NAME yanfs_4095_performance_harness
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_yanfs_4095_performance.py")
        set_tests_properties(yanfs_4095_performance_harness PROPERTIES TIMEOUT 120
            LABELS "0040-R4-AC1;0040-R4-AC2;0040-R5-AC3")
        # Issue 78/79 support: synthetic boundary-decision controls only. The
        # real Guest measurement and detection proof are separate.
        add_test(NAME arena_budget_boundary_harness
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_arena_budget_boundary.py")
        set_tests_properties(arena_budget_boundary_harness PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1" TIMEOUT 30
            LABELS "issue78;issue79;harness")
        # Independent Unicode/regex reference checks complete rows, scores,
        # snippets and summaries. The warm index case also bounds disk reads;
        # it cannot pass by silently choosing the linear backend.
        add_executable(test_search_terms_oracle_probe
                       tests/search_terms_oracle_probe.c)
        target_link_libraries(test_search_terms_oracle_probe PRIVATE yan_search_terms)
        target_compile_options(test_search_terms_oracle_probe PRIVATE ${YAN_WARNINGS})
        foreach(_backend scan index index-repeat)
            add_test(NAME search_terms_oracle_${_backend}
                COMMAND "${Python3_EXECUTABLE}" -B
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/search_terms_oracle.py"
                        "$<TARGET_FILE:test_search_terms_oracle_probe>" "${_backend}")
            set_tests_properties(search_terms_oracle_${_backend} PROPERTIES TIMEOUT 120)
        endforeach()

        # 0027 pure note-metadata oracle: the root-provided native probe is
        # driven over stdin by the independent Python reference. The probe links
        # the pure format library only, so this adds no dependency cycle.
        add_executable(test_note_metadata_oracle_probe
                       tests/note_metadata_oracle_probe.c)
        target_link_libraries(test_note_metadata_oracle_probe PRIVATE
                              yan_note_format)
        target_compile_options(test_note_metadata_oracle_probe PRIVATE
                               ${YAN_WARNINGS})
        add_test(NAME note_metadata_oracle
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/note_metadata_oracle.py"
                    "$<TARGET_FILE:test_note_metadata_oracle_probe>")
        set_tests_properties(note_metadata_oracle PROPERTIES TIMEOUT 120)
        add_executable(test_search_metadata_oracle_probe tests/search_metadata_oracle_probe.c)
        target_link_libraries(test_search_metadata_oracle_probe PRIVATE yan_search_terms)
        target_compile_options(test_search_metadata_oracle_probe PRIVATE ${YAN_WARNINGS})
        foreach(_mode scan index index-repeat)
            add_test(NAME search_metadata_oracle_${_mode}
                COMMAND "${Python3_EXECUTABLE}" -B
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/search_metadata_oracle.py"
                        "$<TARGET_FILE:test_search_metadata_oracle_probe>" "${_mode}")
            set_tests_properties(search_metadata_oracle_${_mode} PROPERTIES TIMEOUT 240)
        endforeach()
        add_test(NAME note_metadata_mutation_gate
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_note_metadata_mutation_gate.py")
        set_tests_properties(note_metadata_mutation_gate PROPERTIES TIMEOUT 30)

        add_executable(test_note_relations_oracle_probe tests/note_relations_oracle_probe.c)
        target_link_libraries(test_note_relations_oracle_probe PRIVATE yan_relation)
        target_compile_options(test_note_relations_oracle_probe PRIVATE ${YAN_WARNINGS})
        add_test(NAME note_relations_oracle
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/note_relations_oracle.py"
                    "$<TARGET_FILE:test_note_relations_oracle_probe>")
        set_tests_properties(note_relations_oracle PROPERTIES TIMEOUT 180)
        add_test(NAME note_relations_guest_gate
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_note_relations_guest_gate.py")
        set_tests_properties(note_relations_guest_gate PROPERTIES TIMEOUT 180)
        add_test(NAME note_relations_mutation_gate
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_note_relations_mutation_gate.py")
        set_tests_properties(note_relations_mutation_gate PROPERTIES TIMEOUT 30)
        add_test(NAME note_blocks_guest_gate
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_note_blocks_guest_gate.py")
        set_tests_properties(note_blocks_guest_gate PROPERTIES TIMEOUT 180)
        # 0035-R8 negative controls: wrong expected bytes, a mutated MCP
        # response, and a missing fixture/binary must all fail before any
        # cross toolchain check. Pure Python, so it stays in the default suite.
        add_test(NAME learning_note_gate
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_learning_note_gate.py")
        set_tests_properties(learning_note_gate PROPERTIES TIMEOUT 60
                             LABELS "0035-R10-AC2")
        # issue61 / 0035 R2/R9 documentation alignment: the public write-tool
        # document is compared with actual tools/list fields by an independent
        # pure-Python checker, so every platform with Python can verify the
        # checker's own controls. This supports the existing ACs; mcp_sdk
        # remains the official-SDK acceptance for 0035-R2-AC1.
        add_test(NAME mcp_write_docs
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mcp_write_docs.py")
        set_tests_properties(mcp_write_docs PROPERTIES TIMEOUT 60
                             LABELS "issue61;0035-doc-alignment")
        add_test(NAME note_blocks_mutation_gate
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_note_blocks_mutation_gate.py")
        set_tests_properties(note_blocks_mutation_gate PROPERTIES TIMEOUT 30)
        add_test(NAME note_metadata_guest_gate
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_note_metadata_guest_gate.py")
        set_tests_properties(note_metadata_guest_gate PROPERTIES TIMEOUT 180)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_test(NAME shell_launcher
                COMMAND "${Python3_EXECUTABLE}" -B
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_shell_launcher.py"
                        --launcher "${CMAKE_CURRENT_SOURCE_DIR}/tools/yan_shell.py"
                        --work "${CMAKE_BINARY_DIR}/shell-launcher")
            set_tests_properties(shell_launcher PROPERTIES TIMEOUT 180)
            if(TARGET yanfs_terminal AND TARGET yan_mkfs AND TARGET yan_run)
                # Issue #106: the Guest ELF must be byte-reproducible. The test
                # replays the terminal link+strip recipe into a PRIVATE temp
                # directory three times and compares the bytes, so it measures
                # build determinism rather than strip idempotence, and it never
                # deletes or rewrites the shared ELF. That matters: an earlier
                # revision re-linked ${YAN_TERMINAL_ELF} in place and the file
                # was absent for ~3.5 s, so parallel Guest tests read a missing
                # ELF and died with ERROR OUTPUT_OR_IO. It also asserts `tohost`
                # is still in .symtab, because `strip -s` would be reproducible
                # but would break yan_run's halt-word lookup.
                if(YAN_RISCV_STRIP AND YAN_RISCV_READELF)
                    add_test(NAME guest_elf_reproducible
                        COMMAND "${Python3_EXECUTABLE}" -B
                                "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_guest_reproducible.py"
                                --elf "${YAN_TERMINAL_ELF}"
                                --readelf "${YAN_RISCV_READELF}"
                                --link-command "${YAN_TERMINAL_LINK_COMMAND}"
                                --strip-command "${YAN_TERMINAL_STRIP_COMMAND}")
                    set_tests_properties(guest_elf_reproducible PROPERTIES
                                         TIMEOUT 300 LABELS "issue106")
                endif()
                # 0025 healthy grep acceptance on the same production ELF: the
                # approved grammar, byte/line/NUL semantics, long lines and the
                # two-process edit -> rename/copy -> grep lifecycle. The real
                # ELF is provided, so there is no skip code.
                add_test(NAME guest_search
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_search.py"
                            --source "${CMAKE_CURRENT_SOURCE_DIR}"
                            --work "${CMAKE_BINARY_DIR}/guest-search"
                            --run "$<TARGET_FILE:yan_run>"
                            --mkfs "$<TARGET_FILE:yan_mkfs>"
                            --guest "${YAN_TERMINAL_ELF}")
                set_tests_properties(guest_search PROPERTIES TIMEOUT 600)
                add_test(NAME guest_search_terms
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_search_terms.py"
                            --source "${CMAKE_CURRENT_SOURCE_DIR}"
                            --work "${CMAKE_BINARY_DIR}/guest-search-terms"
                            --run "$<TARGET_FILE:yan_run>"
                            --mkfs "$<TARGET_FILE:yan_mkfs>"
                            --guest "${YAN_TERMINAL_ELF}")
                set_tests_properties(guest_search_terms PROPERTIES TIMEOUT 600)
                add_test(NAME guest_note_metadata
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_note_metadata.py"
                            --source "${CMAKE_CURRENT_SOURCE_DIR}"
                            --work "${CMAKE_BINARY_DIR}/guest-note-metadata"
                            --run "$<TARGET_FILE:yan_run>"
                            --mkfs "$<TARGET_FILE:yan_mkfs>"
                            --guest "${YAN_TERMINAL_ELF}")
                set_tests_properties(guest_note_metadata PROPERTIES TIMEOUT 600)
                add_test(NAME guest_note_relations
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_note_relations.py"
                            --source "${CMAKE_CURRENT_SOURCE_DIR}"
                            --work "${CMAKE_BINARY_DIR}/guest-note-relations"
                            --run "$<TARGET_FILE:yan_run>"
                            --mkfs "$<TARGET_FILE:yan_mkfs>"
                            --guest "${YAN_TERMINAL_ELF}")
                set_tests_properties(guest_note_relations PROPERTIES TIMEOUT 600)
                add_test(NAME guest_note_blocks
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_note_blocks.py"
                            --source "${CMAKE_CURRENT_SOURCE_DIR}"
                            --work "${CMAKE_BINARY_DIR}/guest-note-blocks"
                            --run "$<TARGET_FILE:yan_run>"
                            --mkfs "$<TARGET_FILE:yan_mkfs>"
                            --guest "${YAN_TERMINAL_ELF}")
                set_tests_properties(guest_note_blocks PROPERTIES TIMEOUT 600)
                if(TARGET yan_mcp)
                    add_test(NAME guest_note_blocks_host
                        COMMAND "${Python3_EXECUTABLE}" -B
                                "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_note_blocks_host.py"
                                --source "${CMAKE_CURRENT_SOURCE_DIR}"
                                --work "${CMAKE_BINARY_DIR}/guest-note-blocks-host"
                                --run "$<TARGET_FILE:yan_run>"
                                --mkfs "$<TARGET_FILE:yan_mkfs>"
                                --guest "${YAN_TERMINAL_ELF}"
                                --server "$<TARGET_FILE:yan_mcp>")
                    set_tests_properties(guest_note_blocks_host PROPERTIES TIMEOUT 600)
                    # 0035-R8: real learning + ordinary MCP instances and the
                    # same production Guest ELF. No skip code: the enclosing
                    # targets already prove the toolchain exists.
                    add_test(NAME guest_learning_note
                        COMMAND "${Python3_EXECUTABLE}" -B
                                "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_learning_note.py"
                                --source "${CMAKE_CURRENT_SOURCE_DIR}"
                                --work "${CMAKE_BINARY_DIR}/guest-learning-note"
                                --run "$<TARGET_FILE:yan_run>"
                                --guest "${YAN_TERMINAL_ELF}"
                                --server "$<TARGET_FILE:yan_mcp>")
                    set_tests_properties(guest_learning_note PROPERTIES TIMEOUT 600
                                         LABELS "0035-R8-AC1;0035-R8-AC2;0035-R10-AC3;0035-R11-AC4")
                    # 0041-R2-AC3 Guest half: the MCP listing names must equal
                    # `meta list tag:<X>` on the real terminal. Registered only
                    # where the Guest targets exist, so a platform without the
                    # toolchain omits it entirely instead of skipping silently.
                    add_test(NAME guest_note_list_agree
                        COMMAND "${Python3_EXECUTABLE}" -B
                                "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_note_list_agree.py"
                                --server "$<TARGET_FILE:yan_mcp>"
                                --run "$<TARGET_FILE:yan_run>"
                                --guest "${YAN_TERMINAL_ELF}")
                    set_tests_properties(guest_note_list_agree PROPERTIES TIMEOUT 300
                                         LABELS "0041-R2-AC3")
                endif()
            endif()
            if(TARGET yanfs_terminal)
                set(_yan_search_fault_args)
                if(YAN_ENABLE_SANITIZERS)
                    list(APPEND _yan_search_fault_args --sanitizers)
                endif()
                # 0025 fault surface: the protocol tag faults at the exact
                # preflight/scan/reader block requests, the first IO after the
                # prompt, and the output boundaries. It reuses the runtime
                # acceptance driver and classifier; only the host C compiler is
                # external, hence the 77 skip code.
                add_test(NAME guest_search_faults
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_search_faults.py"
                            --source "${CMAKE_CURRENT_SOURCE_DIR}"
                            --work "${CMAKE_BINARY_DIR}/guest-search-faults"
                            --cc "${CMAKE_C_COMPILER}"
                            --guest "${YAN_TERMINAL_ELF}"
                            ${_yan_search_fault_args})
                set_tests_properties(guest_search_faults PROPERTIES
                                     SKIP_RETURN_CODE 77 TIMEOUT 600)
                add_test(NAME guest_search_terms_faults
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_search_terms_faults.py"
                            --source "${CMAKE_CURRENT_SOURCE_DIR}"
                            --work "${CMAKE_BINARY_DIR}/guest-search-terms-faults"
                            --cc "${CMAKE_C_COMPILER}"
                            --guest "${YAN_TERMINAL_ELF}"
                            ${_yan_search_fault_args})
                set_tests_properties(guest_search_terms_faults PROPERTIES
                                     SKIP_RETURN_CODE 77 TIMEOUT 600)
                add_test(NAME guest_note_metadata_faults
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_note_metadata_faults.py"
                            --source "${CMAKE_CURRENT_SOURCE_DIR}"
                            --work "${CMAKE_BINARY_DIR}/guest-note-metadata-faults"
                            --cc "${CMAKE_C_COMPILER}"
                            --guest "${YAN_TERMINAL_ELF}"
                            ${_yan_search_fault_args})
                set_tests_properties(guest_note_metadata_faults PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
                add_test(NAME guest_note_relations_faults
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_note_relations_faults.py"
                            --source "${CMAKE_CURRENT_SOURCE_DIR}"
                            --work "${CMAKE_BINARY_DIR}/guest-note-relations-faults"
                            --cc "${CMAKE_C_COMPILER}"
                            --gcc "${YAN_RISCV_GCC}"
                            --guest "${YAN_TERMINAL_ELF}"
                            ${_yan_search_fault_args})
                set_tests_properties(guest_note_relations_faults PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
                add_test(NAME guest_note_blocks_faults
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_note_blocks_faults.py"
                            --source "${CMAKE_CURRENT_SOURCE_DIR}"
                            --work "${CMAKE_BINARY_DIR}/guest-note-blocks-faults"
                            --cc "${CMAKE_C_COMPILER}"
                            --gcc "${YAN_RISCV_GCC}"
                            --guest "${YAN_TERMINAL_ELF}"
                            ${_yan_search_fault_args})
                set_tests_properties(guest_note_blocks_faults PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
            endif()
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
                add_test(NAME guest_editor
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_editor.py"
                            --source "${CMAKE_CURRENT_SOURCE_DIR}"
                            --work "${CMAKE_BINARY_DIR}/guest-editor"
                            --run "$<TARGET_FILE:yan_run>"
                            --mkfs "$<TARGET_FILE:yan_mkfs>"
                            --guest "${YAN_TERMINAL_ELF}")
                set_tests_properties(guest_editor PROPERTIES TIMEOUT 600)
                # 0024 two-name file management on the same production ELF; the
                # script returns 1 on any missing in-repo input and 2 on a
                # harness error, so it has no skip code.
                add_test(NAME guest_file_management
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_file_management.py"
                            --source "${CMAKE_CURRENT_SOURCE_DIR}"
                            --work "${CMAKE_BINARY_DIR}/guest-file-management"
                            --run "$<TARGET_FILE:yan_run>"
                            --mkfs "$<TARGET_FILE:yan_mkfs>"
                            --guest "${YAN_TERMINAL_ELF}")
                set_tests_properties(guest_file_management PROPERTIES TIMEOUT 600)
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
                if(TARGET yan_mkfs AND TARGET yan_run AND YAN_RISCV_READELF)
                    add_test(NAME yanfs_r8_performance
                        COMMAND "${Python3_EXECUTABLE}" -B
                                "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_yanfs_r8_performance.py"
                                --source "${CMAKE_CURRENT_SOURCE_DIR}"
                                --work "${CMAKE_BINARY_DIR}/yanfs-r8-performance"
                                --mkfs "$<TARGET_FILE:yan_mkfs>"
                                --guest "${YAN_TERMINAL_ELF}"
                                --cc "${CMAKE_C_COMPILER}")
                    add_test(NAME yanfs_4095_performance
                        COMMAND "${Python3_EXECUTABLE}" -B
                                "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_yanfs_4095_performance.py"
                                --source "${CMAKE_CURRENT_SOURCE_DIR}"
                                --work "${CMAKE_BINARY_DIR}/yanfs-4095-performance"
                                --mkfs "$<TARGET_FILE:yan_mkfs>"
                                --guest "${YAN_TERMINAL_ELF}"
                                --cc "${CMAKE_C_COMPILER}")
                    set_tests_properties(yanfs_4095_performance PROPERTIES
                        SKIP_RETURN_CODE 77 TIMEOUT 1800
                        LABELS "0040-R4-AC1;0040-R4-AC3;0040-R5-AC1;0040-R5-AC2")
                    # 0040-R5-AC1: the spec claims "两次计步完全一致" (two runs
                    # count identically) and nothing tested it. Guest step counts
                    # come from the simulator's instruction counter, not a clock,
                    # so they must repeat bit-for-bit; if they ever drift, that
                    # sentence becomes false and the 0.42% R5 margin loses its
                    # meaning. Only the 4095 row is repeated, which is the row
                    # the gate is about, so this costs one extra row rather than
                    # a second full matrix.
                    add_test(NAME yanfs_4095_step_determinism
                        COMMAND "${Python3_EXECUTABLE}" -B
                                "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_yanfs_4095_performance.py"
                                --source "${CMAKE_CURRENT_SOURCE_DIR}"
                                --work "${CMAKE_BINARY_DIR}/yanfs-4095-determinism"
                                --mkfs "$<TARGET_FILE:yan_mkfs>"
                                --guest "${YAN_TERMINAL_ELF}"
                                --cc "${CMAKE_C_COMPILER}"
                                --sizes 4095
                                --repeat 2)
                    set_tests_properties(yanfs_4095_step_determinism PROPERTIES
                        SKIP_RETURN_CODE 77 TIMEOUT 1800
                        LABELS "0040-R5-AC1")
                    set_tests_properties(yanfs_r8_performance PROPERTIES
                                         SKIP_RETURN_CODE 77 TIMEOUT 900
                                         LABELS "0036-R8-AC4")
                    if(TARGET yan_mcp)
                        add_executable(large_terms_probe tests/large_terms_probe.c)
                        target_link_libraries(large_terms_probe PRIVATE yan_semantic_posix yan_search_terms)
                        target_compile_options(large_terms_probe PRIVATE ${YAN_WARNINGS})
                        add_test(NAME yanfs_large_capacity
                            COMMAND "${Python3_EXECUTABLE}" -B
                                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_large_capacity.py"
                                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                                    --work "${CMAKE_BINARY_DIR}/yanfs-large-capacity"
                                    --cc "${CMAKE_C_COMPILER}"
                                    --mkfs "$<TARGET_FILE:yan_mkfs>"
                                    --run "$<TARGET_FILE:yan_run>"
                                    --guest "${YAN_TERMINAL_ELF}"
                                    --semantic "$<TARGET_FILE:yan_semantic>"
                                    --mcp "$<TARGET_FILE:yan_mcp>"
                                    --terms-probe "$<TARGET_FILE:large_terms_probe>"
                                    --semantic-runner "${CMAKE_CURRENT_SOURCE_DIR}/tests/large_semantic_runner.py")
                        set_tests_properties(yanfs_large_capacity PROPERTIES
                            SKIP_RETURN_CODE 77 TIMEOUT 1200
                            LABELS "0036-R4-AC1;0036-R4-AC2;0036-R4-AC3;0036-R4-AC4;0036-R6-AC1;0036-R6-AC2")
                    endif()
                    add_test(NAME arena_accounting
                        COMMAND "${Python3_EXECUTABLE}" -B
                                "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_arena_accounting.py"
                                --source "${CMAKE_CURRENT_SOURCE_DIR}"
                                --work "${CMAKE_BINARY_DIR}/arena-accounting"
                                --mkfs "$<TARGET_FILE:yan_mkfs>"
                                --guest "${YAN_TERMINAL_ELF}"
                                --cc "${CMAKE_C_COMPILER}")
                    set_tests_properties(arena_accounting PROPERTIES
                                         SKIP_RETURN_CODE 77 TIMEOUT 1800
                                         LABELS "0036-R3-AC1")
                    add_test(NAME arena_reject
                        COMMAND "${Python3_EXECUTABLE}" -B
                                "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_arena_reject.py"
                                --source "${CMAKE_CURRENT_SOURCE_DIR}"
                                --work "${CMAKE_BINARY_DIR}/arena-reject"
                                --gcc "${YAN_RISCV_GCC}"
                                --run "$<TARGET_FILE:yan_run>"
                                --mkfs "$<TARGET_FILE:yan_mkfs>"
                                --guest "${YAN_TERMINAL_ELF}")
                    set_tests_properties(arena_reject PROPERTIES
                                         SKIP_RETURN_CODE 77 TIMEOUT 900
                                         LABELS "0036-R3-AC3")
                    # 0036-R3-AC5: both arena symbols or neither. The branch in
                    # tools/yan_run.c that rejects a program exporting exactly
                    # one of them had no test before this one. No image is
                    # needed: all three cases are bare loaders, so --mkfs and
                    # --guest are deliberately absent.
                    add_test(NAME arena_pair
                        COMMAND "${Python3_EXECUTABLE}" -B
                                "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_arena_pair.py"
                                --source "${CMAKE_CURRENT_SOURCE_DIR}"
                                --work "${CMAKE_BINARY_DIR}/arena-pair"
                                --gcc "${YAN_RISCV_GCC}"
                                --readelf "${YAN_RISCV_READELF}"
                                --run "$<TARGET_FILE:yan_run>")
                    set_tests_properties(arena_pair PROPERTIES
                                         SKIP_RETURN_CODE 77 TIMEOUT 900
                                         LABELS "0036-R3-AC5")
                    add_test(NAME arena_budget
                        COMMAND "${Python3_EXECUTABLE}" -B
                                "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_arena_budget.py"
                                --source "${CMAKE_CURRENT_SOURCE_DIR}"
                                --work "${CMAKE_BINARY_DIR}/arena-budget"
                                --mkfs "$<TARGET_FILE:yan_mkfs>"
                                --guest "${YAN_TERMINAL_ELF}"
                                --cc "${CMAKE_C_COMPILER}")
                    set_tests_properties(arena_budget PROPERTIES
                                         SKIP_RETURN_CODE 77 TIMEOUT 900
                                         LABELS "0036-R3-AC2")
                    add_test(NAME ram_default
                        COMMAND "${Python3_EXECUTABLE}" -B
                                "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_ram_default.py"
                                --source "${CMAKE_CURRENT_SOURCE_DIR}"
                                --work "${CMAKE_BINARY_DIR}/ram-default"
                                --gcc "${YAN_RISCV_GCC}"
                                --readelf "${YAN_RISCV_READELF}"
                                --run "$<TARGET_FILE:yan_run>")
                    set_tests_properties(ram_default PROPERTIES
                                         SKIP_RETURN_CODE 77 TIMEOUT 300
                                         LABELS "0036-R3-AC4")
                endif()
                # 0036-R3: an N=127 (D=2) image mounts and boots normally now
                # that the services size their tables from the image. No cross
                # compiler is needed.
                #
                # Labelled 0036-R3 only, with no AC number on purpose. This
                # test checks that a >63-file image boots, reports no
                # ERROR CAPACITY and leaves the image unmodified; it never
                # touches the arena, so it is NOT evidence for R3-AC1 (the
                # four capacity points) or R3-AC2 (the 32 MiB budget). Those
                # are covered by arena_accounting and arena_budget. An AC label
                # here made `ctest -L '^0036-R3-AC1$'` return this test too and
                # let it pose as AC coverage.
                add_test(NAME capacity_guard
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_capacity_guard.py"
                            --work "${CMAKE_BINARY_DIR}/capacity-guard"
                            --run "$<TARGET_FILE:yan_run>"
                            --mkfs "$<TARGET_FILE:yan_mkfs>"
                            --guest "${YAN_TERMINAL_ELF}")
                set_tests_properties(capacity_guard PROPERTIES TIMEOUT 300
                                     LABELS "0036-R3")
                add_test(NAME guest_editor_faults
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_editor_faults.py"
                            --source "${CMAKE_CURRENT_SOURCE_DIR}"
                            --work "${CMAKE_BINARY_DIR}/guest-editor-faults"
                            --cc "${CMAKE_C_COMPILER}"
                            --guest "${YAN_TERMINAL_ELF}"
                            ${_yan_terminal_runtime_args})
                set_tests_properties(guest_editor_faults PROPERTIES
                                     SKIP_RETURN_CODE 77 TIMEOUT 600)
                # 0024 file-management fault surface on the same production ELF:
                # output refusal on the success receipt, the first IO request and
                # the scheduled corrupt-response protocol faults. It reuses the
                # runtime acceptance driver and oracle, and only the host C
                # compiler is external, hence the 77 skip code.
                add_test(NAME guest_file_management_faults
                    COMMAND "${Python3_EXECUTABLE}" -B
                            "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_file_management_faults.py"
                            --source "${CMAKE_CURRENT_SOURCE_DIR}"
                            --work "${CMAKE_BINARY_DIR}/guest-file-management-faults"
                            --cc "${CMAKE_C_COMPILER}"
                            --guest "${YAN_TERMINAL_ELF}"
                            ${_yan_terminal_runtime_args})
                set_tests_properties(guest_file_management_faults PROPERTIES
                                     SKIP_RETURN_CODE 77 TIMEOUT 600)
            endif()
        endif()
        # Renamed from yanfs_mutation_gate: this item only exercises the verdict
        # classifier (classify_run) on synthetic runs plus the mocked run_suites
        # controls. It never compiles a mutant, so a green result here says
        # nothing about whether the tests can detect a real defect. The real
        # check is the CTest item `yanfs_mutation`, registered only under
        # YAN_ENABLE_MUTATION_TESTS (mutation block near the end of this file)
        # and run by the Linux CI job.
        add_test(NAME yanfs_mutation_classifier
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_yanfs_mutation_gate.py")
        set_tests_properties(yanfs_mutation_classifier PROPERTIES TIMEOUT 120)
        add_test(NAME terminal_mutation_gate
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_terminal_mutation_gate.py")
        set_tests_properties(terminal_mutation_gate PROPERTIES TIMEOUT 120)
        add_test(NAME editor_mutation_gate
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_editor_mutation_gate.py")
        set_tests_properties(editor_mutation_gate PROPERTIES TIMEOUT 120)
        # Pure classifier/CLI guard for the 0025 search mutation gate: no
        # compiler, so it is in the default suite on every configuration.
        add_test(NAME search_mutation_gate
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_search_mutation_gate.py")
        set_tests_properties(search_mutation_gate PROPERTIES TIMEOUT 180)
        # Pure classifier/CLI guard for the 0026 search-term mutation gate: no
        # compiler, so it is in the default suite on every configuration.
        add_test(NAME search_terms_mutation_gate
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_search_terms_mutation_gate.py")
        set_tests_properties(search_terms_mutation_gate PROPERTIES TIMEOUT 240)
        # Pure CLI guard for the 0025 Guest search scripts: missing in-repo
        # inputs and the provided ELF must be hard failures before any external
        # tool is consulted. No compiler is needed, so it is in the default
        # suite on every configuration.
        add_test(NAME search_guest_gate
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_search_guest_gate.py")
        set_tests_properties(search_guest_gate PROPERTIES TIMEOUT 180)
        if(TARGET yan_mkfs_fault)
            add_test(NAME yanfs_mkfs
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_yanfs_mkfs.py"
                        --mkfs "$<TARGET_FILE:yan_mkfs>"
                        --fault-mkfs "$<TARGET_FILE:yan_mkfs_fault>"
                        --work "${CMAKE_BINARY_DIR}/yanfs-mkfs")
            set_tests_properties(yanfs_mkfs PROPERTIES TIMEOUT 180
                                 LABELS "0036-R1-AC1;0036-R1-AC2;0036-R1-AC3")
            # 0036-R2-AC4: fixed v1 write sequence vs the 41b83b5 SHA-256.
            add_executable(yanfs_write_probe tests/yanfs_write_probe.c)
            target_link_libraries(yanfs_write_probe PRIVATE yan_fs)
            target_compile_options(yanfs_write_probe PRIVATE ${YAN_WARNINGS})
            add_test(NAME yanfs_write_sha
                COMMAND "${Python3_EXECUTABLE}"
                        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_yanfs_write_sha.py"
                        --mkfs "$<TARGET_FILE:yan_mkfs>"
                        --probe "$<TARGET_FILE:yanfs_write_probe>"
                        --work "${CMAKE_BINARY_DIR}/yanfs-write-sha")
            set_tests_properties(yanfs_write_sha PROPERTIES TIMEOUT 60
                                 LABELS "0036-R2-AC4")
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

        # 0025 public-API probe on RV32IM: the unified Search facade over a fake
        # backend with no filesystem. It links only search.c plus the shared
        # Guest start/memory, emits the SEARCH_API_PASS signature and ends with
        # tohost 1. Only the cross compiler and the symbol tool are external.
        add_test(NAME guest_search_api
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_search_api.py"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --work "${CMAKE_BINARY_DIR}/search-api")
        set_tests_properties(guest_search_api PROPERTIES SKIP_RETURN_CODE 77
                                                       TIMEOUT 300)

        # 0026 term public-API probe on RV32IM: the term facade, the shared core
        # and text primitives over a fake backend, with no filesystem and no
        # index linked. It proves the RV32-only span-wrap, four-byte holder and
        # busy/sticky rules, emits the SEARCH_TERMS_API_PASS signature and ends
        # with tohost 1. Only the cross compiler and the symbol tool are
        # external.
        add_test(NAME guest_search_terms_api
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_search_terms_api.py"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --work "${CMAKE_BINARY_DIR}/search-terms-api")
        set_tests_properties(guest_search_terms_api PROPERTIES SKIP_RETURN_CODE 77
                                                             TIMEOUT 300)
        add_test(NAME guest_note_metadata_api
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_note_metadata_api.py"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --work "${CMAKE_BINARY_DIR}/note-metadata-api")
        set_tests_properties(guest_note_metadata_api PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
        add_test(NAME guest_note_relations_api
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_note_relations_api.py"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --work "${CMAKE_BINARY_DIR}/note-relations-api")
        set_tests_properties(guest_note_relations_api PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
        add_test(NAME guest_note_blocks_api
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/run_note_blocks_api.py"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --gcc "${YAN_RISCV_GCC}"
                    --run "$<TARGET_FILE:yan_run>"
                    --work "${CMAKE_BINARY_DIR}/note-blocks-api")
        set_tests_properties(guest_note_blocks_api PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
    endif()

    # 0036-R5-AC2: N is a runtime value, so one -fstack-usage build of the RV32
    # core is enough to prove allocate_extent's frame is static and small.
    if(YAN_RISCV_GCC)
        add_test(NAME stack_usage
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_stack_usage.py"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --work "${CMAKE_BINARY_DIR}/stack-usage"
                    --gcc "${YAN_RISCV_GCC}")
        set_tests_properties(stack_usage PROPERTIES SKIP_RETURN_CODE 77
                                                     TIMEOUT 120
                                                     LABELS "0036-R5-AC2")
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
        add_test(NAME semantic_mutation COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_semantic_mutation.py"
            --repo "${CMAKE_CURRENT_SOURCE_DIR}"
            --unity "${unity_SOURCE_DIR}" --cc "${CMAKE_C_COMPILER}"
            --work "${CMAKE_BINARY_DIR}/semantic-mutation")
        set_tests_properties(semantic_mutation PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME yanfs_mutation
            COMMAND "${Python3_EXECUTABLE}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_yanfs_mutation.py"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --cc "${CMAKE_C_COMPILER}"
                    --unity "${unity_SOURCE_DIR}"
                    --work "${YAN_MUTATION_WORK_ROOT}/yanfs")
        set_tests_properties(yanfs_mutation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
        add_test(NAME editor_mutation
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_editor_mutation.py"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --cc "${CMAKE_C_COMPILER}"
                    --unity "${unity_SOURCE_DIR}"
                    --work "${YAN_MUTATION_WORK_ROOT}/editor")
        set_tests_properties(editor_mutation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
        # 0025 search mutation: the two native suites only (the search core and
        # the shell front end over it), so it needs the host compiler and Unity
        # but no cross toolchain.
        add_test(NAME search_mutation
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_search_mutation.py"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --cc "${CMAKE_C_COMPILER}"
                    --unity "${unity_SOURCE_DIR}"
                    --work "${YAN_MUTATION_WORK_ROOT}/search")
        set_tests_properties(search_mutation PROPERTIES
                             SKIP_RETURN_CODE 77 TIMEOUT 900)
        # 0026 term/index/shell mutation: the four native term suites only, so it
        # needs the host compiler and Unity but no cross toolchain.
        add_test(NAME search_terms_mutation
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_search_terms_mutation.py"
                    --source "${CMAKE_CURRENT_SOURCE_DIR}"
                    --cc "${CMAKE_C_COMPILER}"
                    --unity "${unity_SOURCE_DIR}"
                    --work "${YAN_MUTATION_WORK_ROOT}/search-terms")
        set_tests_properties(search_terms_mutation PROPERTIES
                             SKIP_RETURN_CODE 77 TIMEOUT 1800)
        add_test(NAME note_metadata_mutation
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_note_metadata_mutation.py"
                    --repo "${CMAKE_CURRENT_SOURCE_DIR}"
                    --cc "${CMAKE_C_COMPILER}"
                    --unity "${unity_SOURCE_DIR}"
                    --work "${YAN_MUTATION_WORK_ROOT}/note-metadata")
        set_tests_properties(note_metadata_mutation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240)
        add_test(NAME note_relations_mutation
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_note_relations_mutation.py"
                    --repo "${CMAKE_CURRENT_SOURCE_DIR}"
                    --cc "${CMAKE_C_COMPILER}"
                    --unity "${unity_SOURCE_DIR}"
                    --work "${YAN_MUTATION_WORK_ROOT}/note-relations")
        set_tests_properties(note_relations_mutation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240)
        add_test(NAME note_blocks_mutation
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_note_blocks_mutation.py"
                    --repo "${CMAKE_CURRENT_SOURCE_DIR}"
                    --cc "${CMAKE_C_COMPILER}"
                    --unity "${unity_SOURCE_DIR}"
                    --work "${YAN_MUTATION_WORK_ROOT}/note-blocks")
        set_tests_properties(note_blocks_mutation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
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
        add_test(NAME guest_console_mutation_gate
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/guest/console_mutation_gate.py"
                    --gcc "${YAN_RISCV_GCC}")
        set_tests_properties(guest_console_mutation_gate PROPERTIES
                             SKIP_RETURN_CODE 77 TIMEOUT 1800)

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
