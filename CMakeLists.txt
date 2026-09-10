cmake_minimum_required(VERSION 3.20)
project(superPlace LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(ENABLE_CUDA   "Build CUDA backends" OFF)
option(ENABLE_GUI    "Build ImGui viewer"  OFF)
option(ENABLE_NATIVE "Use -march=native"   ON)

# ccache 可用时自动启用：CImg.h 有 2.9 MB，重复编译代价明显
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")
endif()

find_package(OpenMP REQUIRED)
find_package(PNG REQUIRED)      # 可视化输出（png_writer）
find_package(Eigen3 3.3 REQUIRED NO_MODULE)   # 稀疏矩阵 + BiCGSTAB（M2 二次布局）

# FFTW3 没有官方 CMake config，走 pkg-config（M2'/M3 的 DCT/泊松求解）
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW3F IMPORTED_TARGET fftw3f)   # 单精度，与 PlaceDB 的 float 一致
pkg_check_modules(FFTW3  IMPORTED_TARGET fftw3)
if(FFTW3F_FOUND)
  message(STATUS "FFTW3 (single precision) found: ${FFTW3F_VERSION}")
elseif(FFTW3_FOUND)
  message(STATUS "FFTW3 (double precision) found: ${FFTW3_VERSION}")
else()
  message(WARNING "FFTW3 not found; M2'/M3 will need to vendor Ooura fftsg instead")
endif()

# ---------------------------------------------------------------- core library
set(SUPERPLACE_SOURCES
    src/db/place_db.cpp
    src/db/db_stats.cpp
    src/io/bookshelf_reader.cpp
    src/io/bookshelf_writer.cpp
    src/plot/png_writer.cpp
    src/plot/cimg_renderer.cpp
    src/util/config.cpp
    src/util/metrics.cpp
    src/stages/parse_stage.cpp
)

add_library(superplace STATIC ${SUPERPLACE_SOURCES})

target_include_directories(superplace PUBLIC ${CMAKE_SOURCE_DIR}/src)
# 第三方头文件以 SYSTEM 方式引入，避免它们的警告淹没我们自己的
target_include_directories(superplace SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party)

target_link_libraries(superplace PUBLIC OpenMP::OpenMP_CXX PNG::PNG Eigen3::Eigen)

# FFTW 在 M2'（泊松求解器）落地时才会被引用，这里先接好目标，届时无需改动构建
if(TARGET PkgConfig::FFTW3F)
  target_link_libraries(superplace PUBLIC PkgConfig::FFTW3F)
elseif(TARGET PkgConfig::FFTW3)
  target_link_libraries(superplace PUBLIC PkgConfig::FFTW3)
endif()

# CImg：仅作图像绘制，不开显示后端（无 X11 依赖）
target_compile_definitions(superplace PUBLIC cimg_display=0 cimg_use_zlib=0)

target_compile_options(superplace PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<CONFIG:Release>:-O3 -funroll-loops>
)
if(ENABLE_NATIVE)
  target_compile_options(superplace PRIVATE -march=native)
endif()

if(ENABLE_CUDA)
  enable_language(CUDA)
  set(CMAKE_CUDA_ARCHITECTURES 89)          # RTX 4060 = Ada, sm_89
  target_compile_definitions(superplace PUBLIC ENABLE_CUDA)
endif()

# ------------------------------------------------------------------ executables
add_executable(superplace-cli src/main.cpp)
target_link_libraries(superplace-cli PRIVATE superplace)
target_compile_options(superplace-cli PRIVATE -Wall -Wextra)

# ------------------------------------------------------------------------ tests
enable_testing()
add_subdirectory(test)
