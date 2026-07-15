cmake_minimum_required(VERSION 3.20)
project(ctx C)
enable_testing()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --------------------------------------------------------------------------
# Output directories
# --------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# --------------------------------------------------------------------------
# Platform definitions
# --------------------------------------------------------------------------
if(APPLE)
    add_compile_definitions(CTX_PLATFORM_MACOS)
    enable_language(OBJC)
elseif(WIN32)
    add_compile_definitions(CTX_PLATFORM_WINDOWS NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(UNIX)
    add_compile_definitions(CTX_PLATFORM_LINUX)
endif()

# --------------------------------------------------------------------------
# Threads (required by job system and file watcher)
# --------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# --------------------------------------------------------------------------
# tree-sitter core (force static)
# --------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(TREE_SITTER_FEATURE_WASM OFF CACHE BOOL "" FORCE)
add_subdirectory(vendors/tree-sitter EXCLUDE_FROM_ALL)

# --------------------------------------------------------------------------
# tree-sitter language grammars (each is a tiny static lib)
# --------------------------------------------------------------------------
function(add_ts_grammar TARGET GRAMMAR_DIR)
    set(SRCS ${CMAKE_SOURCE_DIR}/${GRAMMAR_DIR}/src/parser.c)
    if(EXISTS ${CMAKE_SOURCE_DIR}/${GRAMMAR_DIR}/src/scanner.c)
        list(APPEND SRCS ${CMAKE_SOURCE_DIR}/${GRAMMAR_DIR}/src/scanner.c)
    endif()
    if(EXISTS ${CMAKE_SOURCE_DIR}/${GRAMMAR_DIR}/src/scanner.cc)
        list(APPEND SRCS ${CMAKE_SOURCE_DIR}/${GRAMMAR_DIR}/src/scanner.cc)
    endif()
    add_library(${TARGET} STATIC ${SRCS})
    target_include_directories(${TARGET} PRIVATE
        ${CMAKE_SOURCE_DIR}/${GRAMMAR_DIR}/src
        ${CMAKE_SOURCE_DIR}/vendors/tree-sitter/lib/include
    )
    set_target_properties(${TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

add_ts_grammar(ts_grammar_c        vendors/tree-sitter-c)
add_ts_grammar(ts_grammar_cpp      vendors/tree-sitter-cpp)
add_ts_grammar(ts_grammar_python   vendors/tree-sitter-python)
add_ts_grammar(ts_grammar_js       vendors/tree-sitter-javascript)
add_ts_grammar(ts_grammar_ts       vendors/tree-sitter-typescript/typescript)
add_ts_grammar(ts_grammar_tsx      vendors/tree-sitter-typescript/tsx)
add_ts_grammar(ts_grammar_go       vendors/tree-sitter-go)
add_ts_grammar(ts_grammar_rust     vendors/tree-sitter-rust)

# --------------------------------------------------------------------------
# SQLite
# --------------------------------------------------------------------------
set(SQLITE_SRC_DIR ${CMAKE_SOURCE_DIR}/vendors/sqlite)
set(SQLITE_AMALG_C ${SQLITE_SRC_DIR}/sqlite3.c)
set(SQLITE_AMALG_H ${SQLITE_SRC_DIR}/sqlite3.h)

if(EXISTS ${SQLITE_AMALG_C} AND EXISTS ${SQLITE_AMALG_H})
    set(SQLITE_BUILD_DIR ${CMAKE_BINARY_DIR}/sqlite_amalg)
    file(MAKE_DIRECTORY ${SQLITE_BUILD_DIR})
    configure_file(${SQLITE_AMALG_C} ${SQLITE_BUILD_DIR}/sqlite3.c COPYONLY)
    configure_file(${SQLITE_AMALG_H} ${SQLITE_BUILD_DIR}/sqlite3.h COPYONLY)

    add_library(ctx_sqlite STATIC ${SQLITE_BUILD_DIR}/sqlite3.c)
    target_include_directories(ctx_sqlite PUBLIC ${SQLITE_BUILD_DIR})
    target_compile_definitions(ctx_sqlite PRIVATE
        SQLITE_THREADSAFE=2
        SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
        SQLITE_OMIT_LOAD_EXTENSION
    )
    set_target_properties(ctx_sqlite PROPERTIES POSITION_INDEPENDENT_CODE ON)
else()
    find_package(SQLite3 REQUIRED)
    add_library(ctx_sqlite INTERFACE)
    target_link_libraries(ctx_sqlite INTERFACE SQLite::SQLite3)
endif()

# --------------------------------------------------------------------------
# cJSON (static, disable shared + utils + tests)
# --------------------------------------------------------------------------
set(ENABLE_CJSON_TEST     OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_AND_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_UTILS    OFF CACHE BOOL "" FORCE)
add_subdirectory(vendors/cjson EXCLUDE_FROM_ALL)

# --------------------------------------------------------------------------
# uthash (header-only — just expose the include path)
# --------------------------------------------------------------------------
add_library(uthash INTERFACE)
target_include_directories(uthash INTERFACE vendors/uthash/src)

# --------------------------------------------------------------------------
# causality (optional UI — requires Vulkan and initialized nested submodules)
# --------------------------------------------------------------------------
find_package(Vulkan QUIET)
set(CAUSALITY_VENDORS_READY TRUE)
foreach(_cv glfw glm freetype vma stb)
    if(NOT EXISTS ${CMAKE_SOURCE_DIR}/vendors/causality/vendors/${_cv}/CMakeLists.txt
       AND NOT EXISTS ${CMAKE_SOURCE_DIR}/vendors/causality/vendors/${_cv}/CMakeLists.txt)
        # stb and vma may not have CMakeLists — check for any file instead
        file(GLOB _cv_files ${CMAKE_SOURCE_DIR}/vendors/causality/vendors/${_cv}/*)
        if(NOT _cv_files)
            message(STATUS "causality sub-vendor '${_cv}' not initialized — skipping causality")
            set(CAUSALITY_VENDORS_READY FALSE)
            break()
        endif()
    endif()
endforeach()

if(Vulkan_FOUND AND CAUSALITY_VENDORS_READY)
    message(STATUS "Vulkan found — causality visualization will be built")
    add_compile_definitions(CTX_HAS_CAUSALITY)
    add_subdirectory(vendors/causality EXCLUDE_FROM_ALL)
    target_compile_definitions(causality PRIVATE NDEBUG)
    set(CTX_CAUSALITY_LIB causality)
else()
    if(NOT Vulkan_FOUND)
        message(STATUS "Vulkan not found — causality visualization disabled")
    else()
        message(STATUS "causality sub-vendors not initialized — run: git submodule update --init --recursive vendors/causality")
    endif()
    set(CTX_CAUSALITY_LIB "")
endif()

# --------------------------------------------------------------------------
# ctx sources
# --------------------------------------------------------------------------
file(GLOB_RECURSE CTX_SRCS CONFIGURE_DEPENDS src/*.c)

add_executable(ctx ${CTX_SRCS})

target_precompile_headers(ctx PRIVATE src/pch.h)

target_include_directories(ctx PRIVATE
    src
    vendors/tree-sitter/lib/include
    vendors/uthash/src
    vendors/cjson
    vendors/sqlite
    vendors/sqlite/src
)

if(Vulkan_FOUND)
    target_include_directories(ctx PRIVATE
        vendors/causality/causality/include
    )
endif()

target_link_libraries(ctx PRIVATE
    tree-sitter
    ts_grammar_c
    ts_grammar_cpp
    ts_grammar_python
    ts_grammar_js
    ts_grammar_ts
    ts_grammar_tsx
    ts_grammar_go
    ts_grammar_rust
    ctx_sqlite
    cjson
    uthash
    Threads::Threads
)

# --------------------------------------------------------------------------
# causality on Linux: all causality-related archives have circular deps
# between shaderc_combined / glslang / SPIRV. Resolve by linking the whole
# set via target_link_options rather than target_link_libraries, so we fully
# control the order and --start-group/--end-group placement.
# --------------------------------------------------------------------------
if(Vulkan_FOUND AND CAUSALITY_VENDORS_READY)
    if(UNIX AND NOT APPLE)
        find_library(GLSLANG_LIB              glslang                          REQUIRED)
        find_library(GLSLANG_MACHINE_LIB      MachineIndependent               REQUIRED)
        find_library(GLSLANG_OSDEP_LIB        OSDependent                      REQUIRED)
        find_library(GLSLANG_GENERICCG_LIB    GenericCodeGen                   REQUIRED)
        find_library(GLSLANG_LIMITS_LIB       glslang-default-resource-limits  REQUIRED)
        find_library(SPIRV_LIB                SPIRV                            REQUIRED)
        find_library(SPIRV_TOOLS_LIB          SPIRV-Tools                      REQUIRED)
        find_library(SPIRV_TOOLS_OPT_LIB      SPIRV-Tools-opt                  REQUIRED)
        find_library(SHADERC_LIB              shaderc_combined                 REQUIRED)

        find_package(X11 QUIET)
        set(CTX_X11_LIBS "")
        if(X11_FOUND)
            list(APPEND CTX_X11_LIBS ${X11_LIBRARIES})
            if(X11_Xrandr_LIB)
                list(APPEND CTX_X11_LIBS ${X11_Xrandr_LIB})
            endif()
            if(X11_Xcursor_LIB)
                list(APPEND CTX_X11_LIBS ${X11_Xcursor_LIB})
            endif()
            if(X11_Xinerama_LIB)
                list(APPEND CTX_X11_LIBS ${X11_Xinerama_LIB})
            endif()
            if(X11_Xi_LIB)
                list(APPEND CTX_X11_LIBS ${X11_Xi_LIB})
            endif()
        endif()

        # Link causality and all its transitive deps via target_link_libraries.
        # The entire set is wrapped in --start-group/--end-group by injecting
        # sentinel libraries that expand to the linker flags.
        target_link_libraries(ctx PRIVATE
            -Wl,--start-group
            causality
            glfw
            freetype
            ${SHADERC_LIB}
            ${GLSLANG_LIB}
            ${GLSLANG_MACHINE_LIB}
            ${GLSLANG_OSDEP_LIB}
            ${GLSLANG_GENERICCG_LIB}
            ${GLSLANG_LIMITS_LIB}
            ${SPIRV_LIB}
            ${SPIRV_TOOLS_LIB}
            ${SPIRV_TOOLS_OPT_LIB}
            -Wl,--end-group
            ${CTX_X11_LIBS}
            stdc++
            Vulkan::Vulkan
        )
    else()
        # macOS / Windows — no circular dep issues
        target_link_libraries(ctx PRIVATE ${CTX_CAUSALITY_LIB})
    endif()
endif()

# Platform-specific link libs for file watcher
if(APPLE)
    target_link_libraries(ctx PRIVATE "-framework CoreServices")
elseif(WIN32)
    target_link_libraries(ctx PRIVATE kernel32)
endif()

# --------------------------------------------------------------------------
# Compiler warnings
# --------------------------------------------------------------------------
if(MSVC)
    target_compile_options(ctx PRIVATE /W4 /WX /wd4996)
else()
    target_compile_options(ctx PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(UNIX)
    add_test(
        NAME live_update_smoke
        COMMAND /bin/sh ${CMAKE_SOURCE_DIR}/tests/live_update_smoke.sh $<TARGET_FILE:ctx>
    )
    add_test(
        NAME mcp_smoke
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/mcp_smoke.py $<TARGET_FILE:ctx>
    )
    add_test(
        NAME install_smoke
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/install_smoke.py $<TARGET_FILE:ctx>
    )
endif()
