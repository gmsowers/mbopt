cmake_minimum_required(VERSION 3.30)
project(test_mbopt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_OSX_SYSROOT /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk CACHE FILEPATH "SDK dir" FORCE)

add_executable(${CMAKE_PROJECT_NAME}
        src/test_mbopt.cpp
        src/lua_interface.cpp
        src/Model.cpp
        src/Mixer.cpp)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src ../)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(IPOPT_DIR "$ENV{USERPROFILE}\\filen\\dev\\cpp\\ipopt")
    set(LUA_INCLUDE "$ENV{USERPROFILE}\\filen\\dev\\cpp\\lua\include")
    set(IPOPT_INCLUDE "${IPOPT_DIR}\\include\\coin-or")
    set(IPOPT_LIB "${IPOPT_DIR}\\lib\\ipopt.dll.lib")
    set(MUMPS_LIB "${IPOPT_DIR}\\lib\\coinmumps.dll.lib")
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${IPOPT_INCLUDE})
    target_link_libraries(${CMAKE_PROJECT_NAME} ${IPOPT_LIB} ${MUMPS_LIB})
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE /opt/homebrew/include/coin-or ../ /opt/homebrew/include/lua)
    find_library(IPOPTLIB ipopt)
    find_library(LUALIB lua)
    target_link_libraries(${CMAKE_PROJECT_NAME} ${IPOPTLIB} ${LUALIB} /opt/homebrew/opt/llvm/lib/c++/libc++.1.0.dylib)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE /usr/include/coin-or)
    find_library(IPOPTLIB ipopt)
    find_library(LUALIB lua)
    target_link_libraries(${CMAKE_PROJECT_NAME} ${IPOPTLIB} ${LUALIB})
endif()
