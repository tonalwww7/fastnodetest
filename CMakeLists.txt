cmake_minimum_required(VERSION 3.5)
project(testDexTools)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# We will use Boost.Beast (header-only) on top of Boost.Asio, and OpenSSL for TLS.
find_package(Boost REQUIRED COMPONENTS system json)
find_package(OpenSSL REQUIRED)

add_executable(testDexTools
    main.cpp
)

# Beast headers live under Boost/include, System library provides Asio, Beast, etc.
target_include_directories(testDexTools PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(testDexTools PRIVATE
    Boost::system
    OpenSSL::SSL
    Boost::json
    OpenSSL::Crypto
    pthread        # on Unix/macOS you also need pthread
)