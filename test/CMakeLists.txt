set(SP_TEST_DATA_DIR "${CMAKE_SOURCE_DIR}/test_data")

# adaptec1 原始文件 40 MB（.nets 单个就 34 MB），以 adaptec1.tar.xz（2.9 MB）入库，
# 首次配置时解压到 test_data/adaptec1/（该目录被 .gitignore 忽略）。本地已有则不动。
if(NOT EXISTS "${SP_TEST_DATA_DIR}/adaptec1/adaptec1.aux")
    if(EXISTS "${SP_TEST_DATA_DIR}/adaptec1.tar.xz")
        message(STATUS "解压测试数据 test_data/adaptec1.tar.xz")
        file(ARCHIVE_EXTRACT INPUT "${SP_TEST_DATA_DIR}/adaptec1.tar.xz"
                             DESTINATION "${SP_TEST_DATA_DIR}")
    else()
        message(WARNING "缺少 test_data/adaptec1，test_parser 与 test_roundtrip 会失败")
    endif()
endif()

# 官方 MMS adaptec1（含可移动宏，52 MB）同理以 mms_adaptec1.tar.xz（3.3 MB）入库，
# 解压到 test_data/mms_adaptec1/。设计名仍是 adaptec1，与 ISPD 版靠目录区分。
# 它是黄金测试里唯一走完 mLG / FILLERONLY / cGP 四阶段的设计。
if(NOT EXISTS "${SP_TEST_DATA_DIR}/mms_adaptec1/adaptec1.aux")
    if(EXISTS "${SP_TEST_DATA_DIR}/mms_adaptec1.tar.xz")
        message(STATUS "解压测试数据 test_data/mms_adaptec1.tar.xz")
        file(ARCHIVE_EXTRACT INPUT "${SP_TEST_DATA_DIR}/mms_adaptec1.tar.xz"
                             DESTINATION "${SP_TEST_DATA_DIR}")
    else()
        message(WARNING "缺少 test_data/mms_adaptec1，golden_mms_adaptec1_* 会失败")
    endif()
endif()

function(sp_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE superplace)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${name} PRIVATE SP_TEST_DATA_DIR="${SP_TEST_DATA_DIR}")
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

sp_add_test(test_parser)
sp_add_test(test_csr)
sp_add_test(test_roundtrip)
sp_add_test(test_poisson)
sp_add_test(test_hpwl)
sp_add_test(test_b2b)
sp_add_test(test_nesterov)
sp_add_test(test_wirelength)
sp_add_test(test_density)
sp_add_test(test_filler)
sp_add_test(test_macro_sa)
sp_add_test(test_eplace)
sp_add_test(test_lambda_schedule)
sp_add_test(test_repair_candidates)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME test_m3_audit COMMAND ${Python3_EXECUTABLE}
             ${CMAKE_CURRENT_SOURCE_DIR}/test_m3_audit.py)
endif()
sp_add_test(test_viz_writer)

# ------------------------------------------------------- 合法性检查器（05 §4.6）
# 夹具 check/tiny.*：2 行 × 20 site，3 个单元、1 个跨两行的宏、1 个固定块、1 个零面积引脚。
# 每个 .pl 只犯一种错，断言具体数值而不只是 PASS / FAIL。单独跑：ctest -L check。
function(sp_check_case name pl regex)
    add_test(NAME check_${name}
             COMMAND superplace-check ${CMAKE_CURRENT_SOURCE_DIR}/check/tiny.aux
                     ${CMAKE_CURRENT_SOURCE_DIR}/check/${pl})
    set_tests_properties(check_${name} PROPERTIES LABELS check PASS_REGULAR_EXPRESSION "${regex}")
endfunction()
sp_check_case(legal      tiny.pl      "HPWL: +21\\.5\nResult: +PASS")
sp_check_case(overlap    overlap.pl   "Overlap: +1 pairs, 20 area.*Result: +FAIL")
sp_check_case(cell_fixed cellfixed.pl "单元-固定 1.*Result: +FAIL")
sp_check_case(off_site   offsite.pl   "Off-site: +1\n.*Result: +FAIL")
sp_check_case(off_row    offrow.pl    "Off-row: +1\n.*Result: +FAIL")
sp_check_case(out_core   outcore.pl   "Out of core: +1\n.*Result: +FAIL")
sp_check_case(macro_site macrosite.pl "Off-site: +1\n.*Result: +FAIL")
sp_check_case(flipped    flipped.pl   "HPWL: +17\\.5\nResult: +PASS")

if(ENABLE_CUDA)
    sp_add_test(test_poisson_parity)   # CPU 与 CUDA 泊松后端逐点对拍
endif()

# ----------------------------------------------------------------- 黄金结果测试
# 设计 × 线程数，全部对同一份黄金值（见 golden/run_golden.cmake）。单独跑：ctest -L golden。
# RUN_SERIAL：32 线程用例与其他重负载并行跑会让 WSL 满载重启。
set(SP_GOLDEN_DESIGNS thin1 adaptec1 mms_adaptec1)
set(SP_GOLDEN_THREADS 1 8 32)
option(ENABLE_LEGACY_GOLDEN "Register historical 0.07 golden checks" OFF)
if(ENABLE_LEGACY_GOLDEN)
foreach(design IN LISTS SP_GOLDEN_DESIGNS)
    file(GLOB aux "${SP_TEST_DATA_DIR}/${design}/*.aux")
    if(NOT aux)
        set(aux "${SP_TEST_DATA_DIR}/${design}/${design}.aux")   # 缺数据时由脚本报错
    endif()
    foreach(t IN LISTS SP_GOLDEN_THREADS)
        add_test(NAME golden_${design}_t${t}
                 COMMAND ${CMAKE_COMMAND}
                         -DCLI=$<TARGET_FILE:superplace-cli>
                         -DAUX=${aux}
                         -DCONFIG=${CMAKE_SOURCE_DIR}/configs/m3/legacy-golden.json
                         -DTHREADS=${t}
                         -DOUT=${CMAKE_BINARY_DIR}/golden/${design}_t${t}
                         -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/golden/${design}.sha256
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/golden/run_golden.cmake)
        set_tests_properties(golden_${design}_t${t} PROPERTIES LABELS "golden;legacy" RUN_SERIAL TRUE)
    endforeach()
endforeach()
endif()

# New .10-target baseline is independent of the historical .07 hashes.
foreach(design IN LISTS SP_GOLDEN_DESIGNS)
    foreach(t IN LISTS SP_GOLDEN_THREADS)
        file(GLOB aux "${SP_TEST_DATA_DIR}/${design}/*.aux")
        add_test(NAME m3_golden_${design}_t${t}
                 COMMAND ${CMAKE_COMMAND}
                         -DCLI=$<TARGET_FILE:superplace-cli>
                         -DAUX=${aux}
                         -DCONFIG=${CMAKE_SOURCE_DIR}/configs/m3/golden.json
                         -DTHREADS=${t}
                         -DOUT=${CMAKE_BINARY_DIR}/m3_golden/${design}_t${t}
                         -DEXPECTED=${CMAKE_SOURCE_DIR}/test/golden/m3/${design}.sha256
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/golden/run_golden.cmake)
        set_tests_properties(m3_golden_${design}_t${t} PROPERTIES LABELS "golden;m3" RUN_SERIAL TRUE)
    endforeach()
endforeach()
