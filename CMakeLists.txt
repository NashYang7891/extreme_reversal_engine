cmake_minimum_required(VERSION 3.16)
project(ExtremeReversalEngine VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS system)
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(engine
    main.cpp
    orderbook.cpp
    indicators.cpp
    signal_detector.cpp
    ml_optimizer.cpp
)

target_link_libraries(engine
    PRIVATE
    Boost::system
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)
