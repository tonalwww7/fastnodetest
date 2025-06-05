cmake_minimum_required(VERSION 3.14)
project(GateOrderConsole)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1) Найти Boost, OpenSSL и nlohmann_json
find_package(Boost REQUIRED COMPONENTS system thread regex)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)

# 2) Указать заголовочные пути (Boost и OpenSSL подтянутся автоматически)
include_directories(
    ${Boost_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${nlohmann_json_INCLUDE_DIRS}
)

# 3) Собрать одиночный исполняемый файл из main.cpp
add_executable(GateOrderConsole main.cpp)

# 4) Линкуем с нужными библиотеками
target_link_libraries(GateOrderConsole
    Boost::system
    Boost::thread
    Boost::regex
    OpenSSL::SSL
    ${CMAKE_THREAD_LIBS_INIT}
    nlohmann_json::nlohmann_json
)
