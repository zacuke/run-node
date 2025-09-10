cmake_minimum_required(VERSION 3.10)
project(run-node)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required dependencies
find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(LibArchive REQUIRED)

# Add executable
add_executable(run-node
    ./main.cpp
    util/https_download.cpp
    util/extract_tar_xz.cpp
)

# Include directories
target_include_directories(run-node PRIVATE
    ${Boost_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(run-node
    Boost::boost
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    LibArchive::LibArchive
)

# Strip symbols and optimize for size
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # Optimize for size
    target_compile_options(run-node PRIVATE -Os)
    
    # Strip symbols (removes debug information)
    if(CMAKE_STRIP)
        add_custom_command(TARGET run-node POST_BUILD
            COMMAND ${CMAKE_STRIP} $<TARGET_FILE:run-node>
            COMMENT "Stripping symbols from run-node"
        )
    endif()
    
    # Additional linker flags for size optimization
    target_link_options(run-node PRIVATE 
        -Wl,--gc-sections
        -Wl,--as-needed
    )
endif()

# Alternative: Use separate build types
set(CMAKE_CXX_FLAGS_RELEASE "-Os -DNDEBUG")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-Wl,--gc-sections -Wl,--as-needed")

# Install target with stripping
install(TARGETS run-node
    RUNTIME DESTINATION bin
    COMPONENT runtime
)

# Optional: Add install stripping
if(CMAKE_STRIP)
    install(CODE "
        message(STATUS \"Stripping installed executable\")
        execute_process(
            COMMAND ${CMAKE_STRIP} \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/run-node\"
        )
    " COMPONENT runtime)
endif()