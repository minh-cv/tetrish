cmake_minimum_required(VERSION 3.10)
project(tetrish LANGUAGES C)

option(TETRISH_DEV_MODE "Enable dev-mode macros" OFF)
option(TETRISH_TETRISD_NO_DAEMON "Disable daemonization of tetrisd" ${TETRISH_DEV_MODE})
option(TETRISH_TETRISLOGD_NO_DAEMON "Disable daemonization of tetrislogd" ${TETRISH_DEV_MODE})

add_compile_options(-g -Wall -Wextra -Wpedantic -Wconversion -Wno-strict-prototypes)

find_package(Threads REQUIRED)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/corestack)

# cJSON's CMakeLists has no target_include_directories and defaults to a shared
# lib plus its own test/install rules, so configure it and patch the interface.
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(CJSON_OVERRIDE_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_TARGET_EXPORT OFF CACHE BOOL "" FORCE)
set(ENABLE_CUSTOM_COMPILER_FLAGS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/cjson EXCLUDE_FROM_ALL)
target_include_directories(cjson INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/external/cjson)

add_library(tetrisbrain
    src/libtetrisbrain/control.c
    src/libtetrisbrain/state.c
    src/libtetrisbrain/static.c)
target_include_directories(tetrisbrain PRIVATE include)
target_compile_options(tetrisbrain PRIVATE -Wno-missing-braces)

add_executable(tetrisctl src/tetrisctl/main.c)
add_executable(tetrisd src/tetrisd/main.c src/tetrisd/server.c src/tetrisd/config_var.c src/tetrisd/sig.c src/tetrisd/socket.c src/tetrisd/acceptor.c src/tetrisd/epoll.c src/tetrisd/player_io.c src/tetrisd/auth.c src/tetrisd/htttp_layer.c src/tetrisd/app_layer.c)
target_include_directories(tetrisd PRIVATE src/tetrisd)
target_link_libraries(tetrisd PRIVATE daemon logger network common tetrissh config htttp)
target_compile_definitions(tetrisd PRIVATE
    $<$<BOOL:${TETRISH_TETRISD_NO_DAEMON}>:TETRISH_TETRISD_NO_DAEMON>
)
target_include_directories(tetrisd PRIVATE include)

add_executable(tetrislogd src/tetrislogd/main.c
    src/tetrislogd/config_var.c)
target_include_directories(tetrislogd PRIVATE include src/tetrislogd)
target_link_libraries(tetrislogd PRIVATE config tetrissh daemon Threads::Threads)
target_compile_definitions(tetrislogd PRIVATE
    $<$<BOOL:${TETRISH_TETRISLOGD_NO_DAEMON}>:TETRISH_TETRISLOGD_NO_DAEMON>
)

add_executable(tetrisu src/tetrisu/main.c
    src/tetrisu/config_var.c)
target_include_directories(tetrisu PRIVATE src/tetrisu)
target_link_libraries(tetrisu PRIVATE cjson htttp common tetrissh config)
target_include_directories(tetrisu PRIVATE include)

add_library(perms src/tetrish/libs/perms.c)
target_include_directories(perms PUBLIC src/tetrish)

add_executable(tetrish src/tetrish/main.c src/tetrish/shell.c)
target_include_directories(tetrish PRIVATE src/tetrish)

add_executable(ld src/tetrish/system_programs/ld.c)
target_include_directories(ld PRIVATE src/tetrish)
target_link_libraries(ld PRIVATE perms)

add_executable(ldr src/tetrish/system_programs/ldr.c)
target_include_directories(ldr PRIVATE src/tetrish)
target_link_libraries(ldr PRIVATE perms)

add_executable(find src/tetrish/system_programs/find.c)
target_include_directories(find PRIVATE src/tetrish)
target_link_libraries(find PRIVATE perms)

add_executable(backup src/tetrish/system_programs/backup.c)
target_include_directories(backup PRIVATE src/tetrish)

add_executable(sys src/tetrish/system_programs/sys.c)
target_include_directories(sys PRIVATE src/tetrish)

add_executable(hash src/tetrish/system_programs/hash.c)
set_target_properties(hash PROPERTIES OUTPUT_NAME "#")

install(
    TARGETS
        tetrisbrain
        tetrisctl
        tetrisd
        tetrislogd
        tetrisu
        tetrish
        ld
        ldr
        find
        backup
        hash
    RUNTIME DESTINATION bin           # for executables (and DLLs on Windows)
    LIBRARY DESTINATION lib           # for shared libraries on Unix
    ARCHIVE DESTINATION lib           # for static libs (.a, .lib)
)
