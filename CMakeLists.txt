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

# CUDA 后端（cmake -B build_cuda -DENABLE_CUDA=ON）。架构与标准必须在 enable_language
# 与建目标之前设好：它们只在目标创建时拷进目标属性。
if(ENABLE_CUDA)
  set(CMAKE_CUDA_ARCHITECTURES 89)          # RTX 4060 = Ada, sm_89
  set(CMAKE_CUDA_STANDARD 17)
  set(CMAKE_CUDA_STANDARD_REQUIRED ON)
  enable_language(CUDA)
  find_package(CUDAToolkit REQUIRED)
endif()

# ---------------------------------------------------------------- core library
set(SUPERPLACE_SOURCES
    src/db/place_db.cpp
    src/db/db_stats.cpp
    src/db/hpwl.cpp
    src/io/bookshelf_reader.cpp
    src/io/bookshelf_writer.cpp
    src/io/viz_writer.cpp
    src/numeric/poisson_factory.cpp
    src/numeric/cpu/cpu_poisson.cpp
    src/numeric/cpu/nesterov.cpp
    src/plot/png_writer.cpp
    src/plot/cimg_renderer.cpp
    src/util/config.cpp
    src/util/metrics.cpp
    src/gp/bin_grid.cpp
    src/gp/filler.cpp
    src/gp/eplace.cpp
    src/lg/macro_sa.cpp
    src/gp/wa_wirelength.cpp
    src/init/initial_placer.cpp
    src/init/random_placer.cpp
    src/init/cluster_common.cpp
    src/init/cluster_fc.cpp
    src/init/cluster_bc.cpp
    src/init/quadratic_placer.cpp
    src/stages/parse_stage.cpp
    src/stages/init_stage.cpp
    src/stages/gp_stage.cpp
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

# 只作用于 C++：nvcc 不认识这些 gcc 选项
target_compile_options(superplace PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CONFIG:Release>>:-O3 -funroll-loops>
)
if(ENABLE_NATIVE)
  target_compile_options(superplace PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-march=native>)
endif()

if(ENABLE_CUDA)
  target_sources(superplace PRIVATE src/numeric/cuda/cuda_poisson.cu)
  target_compile_definitions(superplace PUBLIC ENABLE_CUDA)
  target_link_libraries(superplace PUBLIC CUDA::cufft CUDA::cudart)
endif()

# ------------------------------------------------------------------ executables
add_executable(superplace-cli src/main.cpp)
target_link_libraries(superplace-cli PRIVATE superplace)
target_compile_options(superplace-cli PRIVATE -Wall -Wextra)

# 课堂测试 1 的专用输出工具：独立二进制，不影响 superplace-cli 的任何行为
add_executable(superplace-quiz tools/quiz/main.cpp)
target_link_libraries(superplace-quiz PRIVATE superplace)
target_compile_options(superplace-quiz PRIVATE -Wall -Wextra)

# 诊断工具：以给定 .pl 为输入单独回放宏合法化（mLG），复现修复段的问题不必重跑 GP
add_executable(superplace-mlg-replay tools/mlg_replay/main.cpp)
target_link_libraries(superplace-mlg-replay PRIVATE superplace)
target_compile_options(superplace-mlg-replay PRIVATE -Wall -Wextra)

add_executable(superplace-cgp-ablation tools/cgp_ablation/main.cpp)
target_link_libraries(superplace-cgp-ablation PRIVATE superplace)
target_compile_options(superplace-cgp-ablation PRIVATE -Wall -Wextra)

# Optional diagnostic only: compile the pinned, unmodified macro legalizer
# under a distinct symbol. The production library/CLI is unchanged.
option(ENABLE_M3_PAIR "Build same-memory baseline/candidate M3 diagnostic" OFF)
set(M3_REFERENCE_COMMIT "06f7fc8" CACHE STRING "Pinned M3 reference source revision")
if(ENABLE_M3_PAIR)
  find_package(Git REQUIRED)
  execute_process(COMMAND ${GIT_EXECUTABLE} show ${M3_REFERENCE_COMMIT}:src/lg/macro_sa.cpp
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE M3_REFERENCE_TEXT RESULT_VARIABLE M3_REFERENCE_RC)
  if(NOT M3_REFERENCE_RC EQUAL 0)
    message(FATAL_ERROR "Cannot read pinned M3 macro reference ${M3_REFERENCE_COMMIT}")
  endif()
  string(FIND "${M3_REFERENCE_TEXT}" "MacroSaResult legalizeMacros(" M3_REFERENCE_SYMBOL)
  if(M3_REFERENCE_SYMBOL EQUAL -1)
    message(FATAL_ERROR "Unexpected reference legalizer source")
  endif()
  string(REPLACE "MacroSaResult legalizeMacros(" "MacroSaResult legalizeMacrosReference("
         M3_REFERENCE_TEXT "${M3_REFERENCE_TEXT}")
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/m3_reference.cpp "${M3_REFERENCE_TEXT}")
  add_executable(superplace-m3-pair tools/m3_pair/main.cpp
                 ${CMAKE_CURRENT_BINARY_DIR}/m3_reference.cpp)
  target_link_libraries(superplace-m3-pair PRIVATE superplace)
  target_compile_options(superplace-m3-pair PRIVATE -Wall -Wextra -funroll-loops)
  if(ENABLE_NATIVE)
    target_compile_options(superplace-m3-pair PRIVATE -march=native)
  endif()
endif()

# 合法性检查器（05 §4.6）：刻意不链接 libsuperplace，连 BookShelf 解析器也自带一份，
# 与布局器不共享任何代码
add_executable(superplace-check tools/check/main.cpp)
target_compile_options(superplace-check PRIVATE -Wall -Wextra)

# ------------------------------------------------------------------------ tests
enable_testing()
add_subdirectory(test)
