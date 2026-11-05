# 黄金结果测试（07 §9 P0-1）：跑一遍 init,gp，把输出 .pl 的 SHA256 与入库的黄金值比对。
#
# 用法（由 test/CMakeLists.txt 注册，一般不手动调用）：
#   cmake -DCLI=<superplace-cli> -DAUX=<design.aux> -DCONFIG=<default.json>
#         -DTHREADS=<N> -DOUT=<输出目录> -DEXPECTED=<test/golden/<设计>.sha256>
#         -P run_golden.cmake
#
# 同一设计的所有线程数对同一份黄金值：铁律 7 要求结果与线程数无关，
# 因此这组测试同时守住"改动前后逐位不变"与"跨线程一致"（台账 U1）。
#
# 黄金值文件格式与 sha256sum 相同（"<hex>  <文件名>"），# 开头为注释。
# 有意改变计算结果时用 scripts/update_golden.sh 重新生成，并在提交说明里写明原因。
cmake_minimum_required(VERSION 3.20)

foreach(var CLI AUX CONFIG THREADS OUT EXPECTED)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "run_golden.cmake: 缺少参数 -D${var}=...")
    endif()
endforeach()

if(NOT EXISTS "${AUX}")
    message(FATAL_ERROR "找不到测试数据 ${AUX}（重新配置一次以解压 test_data/*.tar.xz）")
endif()
if(NOT EXISTS "${EXPECTED}")
    message(FATAL_ERROR "找不到黄金值 ${EXPECTED}（用 scripts/update_golden.sh 生成）")
endif()

# 与 Config::benchmark_name 的推导一致：aux 文件名去掉最后一个扩展名
get_filename_component(bench "${AUX}" NAME_WLE)

# 残留的旧输出会让"本次没写出文件"被误判为通过
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}")

execute_process(
    COMMAND "${CLI}" --aux "${AUX}" --stage init,gp --config "${CONFIG}"
            --threads "${THREADS}" --no-plot --out "${OUT}"
    RESULT_VARIABLE rc
    OUTPUT_FILE "${OUT}/run.log"
    ERROR_FILE "${OUT}/run.log")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "superplace-cli 退出码 ${rc}，日志见 ${OUT}/run.log")
endif()

# 不指定编码时 file(STRINGS) 会在非 ASCII 字节处截断，中文注释行会被拆成碎片
file(STRINGS "${EXPECTED}" lines ENCODING UTF-8)
set(checked 0)
set(failed "")
foreach(line IN LISTS lines)
    if(line MATCHES "^#" OR line STREQUAL "")
        continue()
    endif()
    if(NOT line MATCHES "^([0-9a-f]+)  (.+)$")
        message(FATAL_ERROR "黄金值文件格式错误：${line}")
    endif()
    set(want "${CMAKE_MATCH_1}")
    set(name "${CMAKE_MATCH_2}")
    set(path "${OUT}/${bench}/${name}")
    if(NOT EXISTS "${path}")
        list(APPEND failed "${name}：未生成")
        continue()
    endif()
    file(SHA256 "${path}" got)
    math(EXPR checked "${checked} + 1")
    if(got STREQUAL want)
        message(STATUS "OK   ${name}  ${got}")
    else()
        list(APPEND failed "${name}：得到 ${got}，期望 ${want}")
    endif()
endforeach()

if(checked EQUAL 0 AND failed STREQUAL "")
    message(FATAL_ERROR "黄金值文件 ${EXPECTED} 里没有任何条目")
endif()
if(NOT failed STREQUAL "")
    list(JOIN failed "\n  " msg)
    message(FATAL_ERROR "与黄金值不一致（${THREADS} 线程）：\n  ${msg}\n"
                        "若是有意改变结果，用 scripts/update_golden.sh 重新生成并写明原因。")
endif()
