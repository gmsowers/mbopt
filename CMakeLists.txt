cmake_minimum_required(VERSION 3.30)
project(mbopt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CMAKE_OSX_SYSROOT /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk CACHE FILEPATH "SDK dir" FORCE)
endif()

add_executable(mbopt
        src/main.cpp
        src/scripter.cpp
        src/Model.cpp
        src/Mixer.cpp
        src/Splitter.cpp
        src/Separator.cpp
        src/YieldReactor.cpp
        src/MultiYieldReactor.cpp
        src/StoicReactor.cpp
        )

add_library(mboptlib SHARED
        src/scripter.cpp
        src/Model.cpp
        src/Mixer.cpp
        src/Splitter.cpp
        src/Separator.cpp
        src/YieldReactor.cpp
        src/MultiYieldReactor.cpp
        src/StoicReactor.cpp
        )
set_target_properties(mboptlib PROPERTIES PREFIX "")

target_include_directories(mbopt PRIVATE src)
target_include_directories(mboptlib PRIVATE src)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(IPOPT_DIR "$ENV{USERPROFILE}\\filen\\dev\\cpp\\ipopt")
    set(IPOPT_INCLUDE "${IPOPT_DIR}\\include\\coin-or")
    set(IPOPT_LIB "${IPOPT_DIR}\\lib\\ipopt.dll.lib")
    set(MUMPS_LIB "${IPOPT_DIR}\\lib\\coinmumps.dll.lib")
    set(LUA_DIR "$ENV{USERPROFILE}\\filen\\dev\\cpp\\lua")
    set(LUA_INCLUDE "${LUA_DIR}\\include")
    set(LUA_LIB "${LUA_DIR}\\lib\\lua-5.4.8.lib")
    target_include_directories(mbopt PRIVATE ${IPOPT_INCLUDE} ${LUA_INCLUDE})
    target_include_directories(mboptlib PRIVATE ${IPOPT_INCLUDE} ${LUA_INCLUDE})
    target_link_libraries(mbopt ${IPOPT_LIB} ${MUMPS_LIB} ${LUA_LIB})
    target_link_libraries(mboptlib ${IPOPT_LIB} ${MUMPS_LIB} ${LUA_LIB})
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set_target_properties(mboptlib PROPERTIES SUFFIX ".so")
    target_include_directories(mbopt PRIVATE /opt/homebrew/include/coin-or /opt/homebrew/include/lua)
    target_include_directories(mboptlib PRIVATE /opt/homebrew/include/coin-or /opt/homebrew/include/lua)
    find_library(IPOPTLIB ipopt)
    find_library(LUALIB lua)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        set(LIBC_PATH "/opt/homebrew/opt/llvm/lib/c++/libc++.1.0.dylib")
    else()
        set(LIBC_PATH "")
    endif()
    target_link_libraries(mbopt ${IPOPTLIB} ${LUALIB} ${LIBC_PATH})
    target_link_libraries(mboptlib ${IPOPTLIB} ${LUALIB} ${LIBC_PATH})
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_include_directories(mbopt PRIVATE /usr/include/coin-or)
    target_include_directories(mboptlib PRIVATE /usr/include/coin-or)
    find_library(IPOPTLIB ipopt)
    find_library(LUALIB lua)
    target_link_libraries(mbopt ${IPOPTLIB} ${LUALIB})
    target_link_libraries(mboptlib ${IPOPTLIB} ${LUALIB})
endif()
