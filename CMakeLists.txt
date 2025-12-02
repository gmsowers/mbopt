cmake_minimum_required(VERSION 3.30)
project(mbopt)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_OSX_SYSROOT /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk CACHE FILEPATH "SDK dir" FORCE)

add_executable(mbopt
        testModel.cpp
        Model.cpp
        Mixer.cpp)

if(APPLE)
        target_link_libraries(mbopt /opt/homebrew/opt/llvm/lib/c++/libc++.1.0.dylib)
endif()