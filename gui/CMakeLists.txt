# superPlace 布局查看器 —— 独立 CMake 工程
#
# 关键约定：**本工程是顶层工程，根目录的 CMakeLists.txt 永远不会 add_subdirectory(gui)。**
# 依赖方向是单向的：gui 可以（可选地）去找 libsuperplace，算法侧永远不知道 gui 存在。
# 因此在 gui/ 里做任何改动都不可能让算法侧的 `cmake --build build` 或 `ctest` 失败。
#
# 两种构建模式
# -----------
#   独立模式（同伴日常开发用，零算法依赖）：
#       cmake -S gui -B gui/build && cmake --build gui/build -j
#       产出 superplace-viz：读 viz bundle 的离线查看器
#
#   集成模式（需要实时观察迭代时才用，要求算法侧已构建）：
#       cmake -S gui -B gui/build -DSUPERPLACE_ROOT=$PWD/build
#       额外产出 superplace-gui：内嵌引擎、worker 线程实时出帧
#
# 离线编译：若无法联网，先把依赖放到 gui/third_party/<name>，再用
#   -DFETCHCONTENT_SOURCE_DIR_IMGUI=${CMAKE_SOURCE_DIR}/third_party/imgui
# 指过去即可（FetchContent 的内置机制，不需要改本文件）。

cmake_minimum_required(VERSION 3.20)
project(superplace_gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ----------------------------------------------------------------- 系统依赖
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)
find_package(glfw3 3.3 REQUIRED)       # apt install libglfw3-dev

# -------------------------------------------------------------- vendor 的 UI 库
#
# ImGui（docking 分支）与 ImPlot 直接 vendor 进 gui/third_party/，与项目既有做法
# 一致（根目录 third_party/ 里的 CImg 与 nlohmann 也是 vendor 的，见 01 §9.2）。
#
# 为什么不用 FetchContent：本机到 GitHub 的 git clone 与 codeload 实测均不通
# （clone 100 秒零进展、tarball 90 秒 0 字节）。vendor 之后队友 clone 仓库即可
# 构建，不需要任何网络，这比"配置时现拉"更可靠。
#
# 获取方式见 gui/third_party/README.md。
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
set(IMPLOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot)

if(NOT EXISTS ${IMGUI_DIR}/imgui.cpp)
  message(FATAL_ERROR
    "缺少 ImGui。请按 gui/third_party/README.md 把 ImGui（docking 分支）放到\n"
    "  ${IMGUI_DIR}\n"
    "判据：${IMGUI_DIR}/imgui.cpp 存在。")
endif()
if(NOT EXISTS ${IMPLOT_DIR}/implot.cpp)
  message(FATAL_ERROR
    "缺少 ImPlot。请按 gui/third_party/README.md 把 ImPlot 放到\n  ${IMPLOT_DIR}")
endif()

# nlohmann/json：优先用 gui 自己 vendor 的，退回项目根目录那份（已存在，不必重复一份）
find_file(VIZ_JSON_HPP json.hpp
  PATHS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nlohmann
  NO_DEFAULT_PATH)
if(NOT VIZ_JSON_HPP)
  message(FATAL_ERROR "找不到 nlohmann/json.hpp（gui/third_party/nlohmann/ 或 ../third_party/nlohmann/）")
endif()
get_filename_component(VIZ_JSON_DIR ${VIZ_JSON_HPP} DIRECTORY)
get_filename_component(VIZ_JSON_INC ${VIZ_JSON_DIR} DIRECTORY)
message(STATUS "nlohmann/json: ${VIZ_JSON_HPP}")

# ImGui / ImPlot 不自带 CMake 工程，这里手工建库
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_stacklayout.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
  ${IMPLOT_DIR}/implot.cpp
  ${IMPLOT_DIR}/implot_items.cpp
)
target_include_directories(imgui SYSTEM PUBLIC
  ${IMGUI_DIR} ${IMGUI_DIR}/backends ${IMPLOT_DIR})
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# ------------------------------------------------------------------ 查看器内核
# core 是与"数据从哪来"无关的那部分：场景模型、渲染、相机、面板。
# 独立版与集成版共用同一份 core，差别只在注入哪个 IPlacementSource。
add_library(viz_core STATIC
  src/viz/bundle_source.cpp
  src/viz/summary.cpp
  src/viz/spatial_index.cpp
  src/viz/density.cpp
  src/viz/frame_map.cpp
  src/render/gl_loader.cpp
  src/render/canvas.cpp
  src/app.cpp
)
target_include_directories(viz_core PUBLIC src)
target_link_libraries(viz_core PUBLIC imgui OpenGL::GL Threads::Threads)
target_include_directories(viz_core SYSTEM PUBLIC ${VIZ_JSON_INC})
target_compile_options(viz_core PRIVATE -Wall -Wextra)

# ------------------------------------------------------------------ 零依赖测试（T13）
# 不链接 ImGui/GL/窗口：只编译与渲染无关的纯逻辑源文件，可进 CI、可无头运行。
enable_testing()
add_executable(viz_tests
  test/test_main.cpp
  test/test_camera.cpp
  test/test_kind.cpp
  test/test_bundle.cpp
  test/test_spatial.cpp
  test/test_density.cpp
  test/test_frame_map.cpp
  test/test_scale.cpp
  src/viz/bundle_source.cpp
  src/viz/summary.cpp
  src/viz/spatial_index.cpp
  src/viz/density.cpp
  src/viz/frame_map.cpp
)
target_include_directories(viz_tests PRIVATE src)
target_include_directories(viz_tests SYSTEM PRIVATE ${VIZ_JSON_INC})
target_compile_definitions(viz_tests PRIVATE
  SYNTHETIC_BUNDLE="${CMAKE_CURRENT_SOURCE_DIR}/sample-data/synthetic")
add_test(NAME viz_tests COMMAND viz_tests)

# ------------------------------------------------------- 目标一：独立查看器
add_executable(superplace-viz src/main_viz.cpp)
target_link_libraries(superplace-viz PRIVATE viz_core)

# ------------------------------------- 目标二：集成版（仅在显式给出算法侧时构建）
#
# SUPERPLACE_ROOT 指向算法侧的构建目录。给了才会多出 superplace-gui 这个目标，
# 不给就完全当它不存在——这保证了同伴的日常构建永远不碰算法依赖
# （FFTW / Eigen / libpng / OpenMP 一个都不需要）。
if(SUPERPLACE_ROOT)
  find_library(SUPERPLACE_LIB superplace PATHS ${SUPERPLACE_ROOT} REQUIRED)
  message(STATUS "集成模式：libsuperplace = ${SUPERPLACE_LIB}")

  # libsuperplace 是静态库，它的传递依赖必须由使用方自己链上。
  # 这几行正是"集成模式的代价"的具体形态——也正因如此，独立模式才值得保留：
  # 同伴日常开发完全不需要下面这些东西。
  find_package(OpenMP REQUIRED)
  find_package(PNG REQUIRED)
  find_package(Eigen3 3.3 REQUIRED NO_MODULE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFTW3F IMPORTED_TARGET fftw3f)
  pkg_check_modules(FFTW3 IMPORTED_TARGET fftw3)

  add_executable(superplace-gui src/main_gui.cpp src/viz/live_source.cpp)
  target_include_directories(superplace-gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
  target_link_libraries(superplace-gui PRIVATE viz_core ${SUPERPLACE_LIB}
    OpenMP::OpenMP_CXX PNG::PNG Eigen3::Eigen)
  if(TARGET PkgConfig::FFTW3F)
    target_link_libraries(superplace-gui PRIVATE PkgConfig::FFTW3F)
  elseif(TARGET PkgConfig::FFTW3)
    target_link_libraries(superplace-gui PRIVATE PkgConfig::FFTW3)
  endif()

  # T11：实时源四阶段序列的无头测试（不开窗口、不建 GL 上下文）。
  # 只在集成模式下存在——它要链 libsuperplace 才能真正跑引擎。
  add_executable(viz_live_tests
    test/test_live_stages.cpp
    src/viz/live_source.cpp
  )
  target_include_directories(viz_live_tests PRIVATE
    src ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
  target_compile_definitions(viz_live_tests PRIVATE
    TINY_MACROS_AUX="${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/tiny_macros/tiny.aux")
  target_link_libraries(viz_live_tests PRIVATE ${SUPERPLACE_LIB}
    OpenMP::OpenMP_CXX PNG::PNG Eigen3::Eigen Threads::Threads)
  if(TARGET PkgConfig::FFTW3F)
    target_link_libraries(viz_live_tests PRIVATE PkgConfig::FFTW3F)
  elseif(TARGET PkgConfig::FFTW3)
    target_link_libraries(viz_live_tests PRIVATE PkgConfig::FFTW3)
  endif()
  add_test(NAME viz_live_tests COMMAND viz_live_tests)
else()
  message(STATUS "独立模式：只构建 superplace-viz（不需要任何算法侧依赖）")
endif()
