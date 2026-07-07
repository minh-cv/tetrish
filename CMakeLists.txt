cmake_minimum_required(VERSION 3.10)
project(tetrish LANGUAGES C)

add_compile_options(-g -Wall -Wextra -Wpedantic -Wconversion -Wno-strict-prototypes)

add_library(libhtttp SHARED src/libhtttp/htttp.c)
target_include_directories(libhtttp PRIVATE include)

add_library(libtetrissh SHARED src/libtetrissh/tetrissh.c)
target_include_directories(libtetrissh PRIVATE include)

add_library(libtetrisbrain SHARED src/libtetrisbrain/tetrisbrain.c)
target_include_directories(libtetrisbrain PRIVATE include)

add_library(common src/common/common.c)
target_include_directories(common PRIVATE include)

add_executable(tetrisctl src/tetrisctl/main.c)
add_executable(tetrisd src/tetrisd/main.c)
add_executable(tetrislogd src/tetrislogd/main.c)
add_executable(tetrisu src/tetrisu/main.c)

add_library(perms src/tetrish/libs/perms.c)
target_include_directories(perms PRIVATE include)

add_executable(tetrish src/tetrish/main.c src/tetrish/shell.c)
target_include_directories(tetrish PRIVATE include)

add_executable(ld src/tetrish/system_programs/ld.c)
target_include_directories(ld PRIVATE include)
target_link_libraries(ld PRIVATE perms)

add_executable(ldr src/tetrish/system_programs/ldr.c)
target_include_directories(ldr PRIVATE include)
target_link_libraries(ldr PRIVATE perms)

add_executable(find src/tetrish/system_programs/find.c)
target_include_directories(find PRIVATE include)
target_link_libraries(find PRIVATE perms)

add_executable(backup src/tetrish/system_programs/backup.c)
target_include_directories(backup PRIVATE include)

add_executable(sys src/tetrish/system_programs/sys.c)
target_include_directories(sys PRIVATE include)

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
    RUNTIME DESTINATION bin           # for executables (and DLLs on Windows)
    LIBRARY DESTINATION lib           # for shared libraries on Unix
    ARCHIVE DESTINATION lib           # for static libs (.a, .lib)
)
