cmake_minimum_required(VERSION 3.18)
project(rtp)

enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
set(CMAKE_BUILD_TYPE "Debug")
add_compile_options("-Wall")

find_package(Threads REQUIRED)
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)

add_compile_definitions(SOURCE_DIR="${CMAKE_SOURCE_DIR}")

option(DL "debug logging" ON)

if(DL)
    message("Debug logging is on")
    add_compile_definitions(LDEBUG)
endif()

unset(DL CACHE)

include_directories(/usr/local/include)
link_directories(/usr/local/lib)

add_library(util src/util.c)
add_library(rtp src/rtp.cpp)
target_link_libraries(rtp PUBLIC util)

add_executable(sender src/sender.cpp)
target_link_libraries(sender PUBLIC util)
target_link_libraries(sender PUBLIC rtp)

add_executable(receiver src/receiver.cpp)
target_link_libraries(receiver PUBLIC util)
target_link_libraries(receiver PUBLIC rtp)
