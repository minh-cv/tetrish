cmake_minimum_required(VERSION 3.10)
project(tetrish LANGUAGES C)

option(TETRISH_DEV_MODE "Enable dev-mode macros" OFF)
option(TETRISH_TETRISD_NO_DAEMON "Disable daemonization of tetrisd" ${TETRISH_DEV_MODE})

add_compile_options(-g -Wall -Wextra -Wpedantic -Wconversion -Wno-strict-prototypes)

add_library(libhtttp src/libhtttp/htttp.c)
target_include_directories(libhtttp PUBLIC include)

add_library(libtetrissh src/libtetrissh/tetrissh.c)
target_include_directories(libtetrissh PUBLIC include)
target_link_libraries(libtetrissh PUBLIC common)

add_library(libtetrisbrain src/libtetrisbrain/tetrisbrain.c)
target_include_directories(libtetrisbrain PUBLIC include)

add_library(common src/common/common.c)
target_include_directories(common PUBLIC include)
target_link_libraries(common PUBLIC crypto)

add_executable(tetrisctl src/tetrisctl/main.c)
add_executable(tetrisd src/tetrisd/main.c src/tetrisd/client.c src/tetrisd/state.c src/tetrisd/epollmanip.c src/tetrisd/config_var.c src/tetrisd/daemon.c)
target_include_directories(tetrisd PRIVATE src/tetrisd)
target_compile_definitions(tetrisd PRIVATE 
    $<$<BOOL:${TETRISH_TETRISD_NO_DAEMON}>:TETRISH_TETRISD_NO_DAEMON>
)

add_executable(tetrislogd src/tetrislogd/main.c
    src/tetrislogd/config_var.c)
target_include_directories(tetrislogd PRIVATE include src/tetrislogd)
target_link_libraries(tetrislogd PRIVATE config libtetrissh)

add_executable(tetrisu src/tetrisu/main.c
    src/tetrisu/config_var.c)
target_include_directories(tetrisu PRIVATE src/tetrisu)

foreach(target tetrisd tetrisu)
    target_include_directories(${target} PRIVATE include)
    target_link_libraries(${target} PRIVATE libhtttp common libtetrissh config)
endforeach()

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

add_library(config src/config/config.c)
target_include_directories(config PRIVATE include)

install(
    TARGETS
        libhtttp
        libtetrissh
        libtetrisbrain
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
