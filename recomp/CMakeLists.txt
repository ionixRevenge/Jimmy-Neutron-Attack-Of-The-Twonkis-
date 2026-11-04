cmake_minimum_required(VERSION 3.20)
project(twonkrecomp CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra)
endif()

add_executable(twonkrecomp
  src/main.cpp
  src/image.cpp
  src/r5900.cpp
  src/codegen.cpp
  src/analysis.cpp
)
target_include_directories(twonkrecomp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/include)
