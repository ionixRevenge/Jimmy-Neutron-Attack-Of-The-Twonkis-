cmake_minimum_required(VERSION 3.20)
project(twonkies CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The recompiler (host tool)
add_subdirectory(recomp)

# Runtime: CPU state, memory, services used by generated code
add_library(ps2runtime STATIC
  runtime/src/services.cpp
  runtime/src/kernel.cpp
  runtime/src/cop0.cpp
  runtime/src/hle_kernel.cpp
  runtime/src/iop.cpp
  runtime/src/iop_services.cpp
  runtime/src/iop_cdvd.cpp
  runtime/src/iop_his.cpp
  runtime/src/iop_pad.cpp
  runtime/src/iop_fileio.cpp
  runtime/src/disc.cpp
  runtime/src/hw.cpp
  runtime/src/vu.cpp
)
target_include_directories(ps2runtime PUBLIC runtime/include)
if(MSVC)
  target_compile_options(ps2runtime PRIVATE /W4 /permissive-)
endif()

# Recompiled game code, produced by: twonkrecomp gen ... <TWONK_GENERATED_DIR>
set(TWONK_GENERATED_DIR "" CACHE PATH "Directory containing twonkrecomp output")
if(TWONK_GENERATED_DIR)
  file(GLOB TWONK_GENERATED_SOURCES CONFIGURE_DEPENDS ${TWONK_GENERATED_DIR}/*.cpp)
  add_library(twonk_game STATIC ${TWONK_GENERATED_SOURCES})
  target_include_directories(twonk_game PRIVATE ${TWONK_GENERATED_DIR})
  target_link_libraries(twonk_game PUBLIC ps2runtime)
  if(MSVC)
    target_compile_options(twonk_game PRIVATE /bigobj /MP /W1)
  endif()
  option(TWONK_TRACE "Record guest function entries (shown on fatal errors)" ON)
  if(TWONK_TRACE)
    target_compile_definitions(twonk_game PRIVATE PS2_TRACE=1)
  endif()

  # The native game executable
  add_executable(twonk runtime/src/host_main.cpp)
  target_link_libraries(twonk PRIVATE twonk_game ps2runtime)
  if(MSVC)
    target_link_options(twonk PRIVATE /STACK:67108864)
  endif()
endif()
