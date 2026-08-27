cmake_minimum_required(VERSION 3.13)
project(pugixml LANGUAGES CXX)

add_library(pugixml STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/pugixml.cpp)
target_include_directories(pugixml PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
  target_compile_definitions(pugixml PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
