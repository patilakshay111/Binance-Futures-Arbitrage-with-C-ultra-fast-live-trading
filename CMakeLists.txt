cmake_minimum_required(VERSION 3.10)
project(QuarterlyArbLive)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# We require several networking/security libraries for Binances' API:
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# We also need Boost for Asio/Beast WebSockets
find_package(Boost REQUIRED COMPONENTS system)

# nlohmann_json is usually header-only or installed via vcpkg/MSYS2
find_path(NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp)

include_directories(
    ${CURL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${Boost_INCLUDE_DIRS}
    ${NLOHMANN_JSON_INCLUDE_DIR}
)

add_executable(quarterly_arb_live quarterly_arb_live.cpp)

target_link_libraries(quarterly_arb_live
    PRIVATE
        ${CURL_LIBRARIES}
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        Boost::system
        ws2_32
        crypt32
        wsock32
)
