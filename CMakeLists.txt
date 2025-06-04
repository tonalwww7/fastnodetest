cmake_minimum_required(VERSION 3.10)
project(bsc_mempool_latency)

set(CMAKE_CXX_STANDARD 17)
set(Boost_USE_MULTITHREADED ON)

find_package(Boost 1.75.0 REQUIRED COMPONENTS system thread json)
find_package(OpenSSL REQUIRED)

add_executable(testLatency main.cpp)
target_link_libraries(testLatency
    PRIVATE
      Boost::system
      Boost::thread
      Boost::json
      OpenSSL::SSL
      OpenSSL::Crypto
)
