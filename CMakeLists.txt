cmake_minimum_required(VERSION 3.21)
project(kerminal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

find_package(Qt6 QUIET COMPONENTS Widgets)
if (Qt6_FOUND)
  find_package(KF6 REQUIRED COMPONENTS Parts CoreAddons)
  set(QT_LIBS Qt6::Widgets)
  set(KF_LIBS KF6::Parts KF6::CoreAddons)
else()
  find_package(Qt5 REQUIRED COMPONENTS Widgets)
  find_package(KF5 REQUIRED COMPONENTS Parts CoreAddons)
  set(QT_LIBS Qt5::Widgets)
  set(KF_LIBS KF5::Parts KF5::CoreAddons)
endif()

add_executable(kerminal main.cpp)

target_link_libraries(kerminal
  PRIVATE
    ${QT_LIBS}
    ${KF_LIBS}
)
