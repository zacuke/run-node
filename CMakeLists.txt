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
    main.cpp
    https_download.cpp
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

# Install target (optional)
install(TARGETS run-node DESTINATION bin)