cmake_minimum_required(VERSION 3.21)
project(kerminal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(KF6 REQUIRED COMPONENTS Parts CoreAddons)

add_executable(kerminal main.cpp)

target_link_libraries(kerminal
  PRIVATE
    Qt6::Widgets
    KF6::Parts
    KF6::CoreAddons
)
