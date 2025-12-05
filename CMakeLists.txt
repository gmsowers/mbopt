cmake_minimum_required(VERSION 3.30)
project(mbopt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_OSX_SYSROOT /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk CACHE FILEPATH "SDK dir" FORCE)

add_executable(mbopt
        src/testModel.cpp
        src/Model.cpp
        src/Mixer.cpp)

target_include_directories(mbopt PRIVATE src)

if(APPLE)
        target_link_libraries(mbopt /opt/homebrew/opt/llvm/lib/c++/libc++.1.0.dylib)
endif()