cmake_minimum_required(VERSION 3.10)
project(tetrish LANGUAGES C)

add_library(libhtttp SHARED src/libhtttp/htttp.c)
target_include_directories(libhtttp PUBLIC include)

add_library(libtetrissh SHARED src/libtetrissh/tetrissh.c)
target_include_directories(libtetrissh PUBLIC include)

add_library(libtetrisbrain SHARED src/libtetrisbrain/tetrisbrain.c)
target_include_directories(libtetrisbrain PUBLIC include)

add_library(common src/common/common.c)
target_include_directories(common PUBLIC include)

add_library(vector src/vector/vector.c)

add_executable(tetrisctl src/tetrisctl/main.c)
add_executable(tetrisd src/tetrisd/main.c)
add_executable(tetrislogd src/tetrislogd/main.c)
add_executable(tetrisu src/tetrisu/main.c)
add_executable(tetrish src/tetrish/main.c)

# using target_link_libraries to link relevant libraries

install(
    TARGETS
        libhtttp
        libtetrissh
        libtetrisbrain
        common
        vector
        tetrisctl
        tetrisd
        tetrislogd
        tetrisu
        tetrish
    RUNTIME DESTINATION bin           # for executables (and DLLs on Windows)
    LIBRARY DESTINATION lib           # for shared libraries on Unix
    ARCHIVE DESTINATION lib           # for static libs (.a, .lib)
)
