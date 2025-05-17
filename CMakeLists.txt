cmake_minimum_required(VERSION 3.10)
project(efz_training_mode)

# Set C++ standard
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force 32-bit build
if(MSVC)
    # Proper way to force 32-bit build
    set(CMAKE_GENERATOR_PLATFORM Win32)
    # Use appropriate architecture flags for 32-bit
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MACHINE:X86")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /MACHINE:X86")
    
    # Static linking of C++ Runtime - this is the critical addition
    # Replace dynamic runtime (/MD) with static runtime (/MT)
    foreach(flag_var
            CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
            CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
        if(${flag_var} MATCHES "/MD")
            string(REGEX REPLACE "/MD" "/MT" ${flag_var} "${${flag_var}}")
        endif()
    endforeach()
    
    # Additional flags for static linking
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
    # Disable annoying secure CRT warnings
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
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
)

# Link required libraries including network libraries
target_link_libraries(efz_training_mode PRIVATE 
    user32 
    kernel32
    ws2_32
    iphlpapi
    comctl32
    # Add additional static linking options
    legacy_stdio_definitions
)

# Set output directories if needed
set_target_properties(efz_training_mode PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# Set the runtime library to static
# This ensures all dependencies are statically linked
set_property(TARGET efz_training_mode PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")