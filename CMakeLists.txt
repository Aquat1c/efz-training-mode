cmake_minimum_required(VERSION 3.10)
project(efz_training_mode)

# Set C++ standard
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force 32-bit build
if(MSVC)
    # Proper way to force 32-bit build
    set(CMAKE_GENERATOR_PLATFORM Win32)
    # For Visual Studio compiler flags
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /arch:IA32")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MACHINE:X86")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /MACHINE:X86")
endif()

# Define source files
set(SOURCES
    src/dllmain.cpp
    src/utilities.cpp
    src/memory.cpp
    src/logger.cpp
    src/input_handler.cpp
    src/gui.cpp
    src/frame_monitor.cpp
    src/network.cpp
)

# Create a shared library (DLL)
add_library(efz_training_mode SHARED ${SOURCES})

# Include directories
target_include_directories(efz_training_mode PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/resource
)

# Link required libraries including network libraries
target_link_libraries(efz_training_mode PRIVATE 
    user32 
    kernel32
    ws2_32      # Required for Winsock networking functions
    iphlpapi    # Required for IP Helper API functions (GetExtendedTcpTable, etc.)
)

# Set output directories if needed
set_target_properties(efz_training_mode PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)