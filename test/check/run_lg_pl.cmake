# 用 --pl 从一份非法的 .pl 单独跑 parse,lg，再用 superplace-check 判定结果合法。
# 参数：CLI、CHECK、AUX、PL、OUT
file(REMOVE_RECURSE "${OUT}")
execute_process(
    COMMAND "${CLI}" --aux "${AUX}" --pl "${PL}" --stage parse,lg --no-plot --out "${OUT}"
    RESULT_VARIABLE rc OUTPUT_VARIABLE log ERROR_VARIABLE log)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "superplace-cli 退出码 ${rc}：\n${log}")
endif()

get_filename_component(design "${AUX}" NAME_WE)
file(READ "${OUT}/${design}/${design}.parse.pl" parsePl)
if(NOT parsePl MATCHES "c1\t0\\.400000\t0\\.300000")
    message(FATAL_ERROR "--pl 未生效：parse.pl 里不是 ${PL} 的位置\n${parsePl}")
endif()

execute_process(
    COMMAND "${CHECK}" "${AUX}" "${OUT}/${design}/${design}.lg.pl"
    RESULT_VARIABLE crc OUTPUT_VARIABLE report ERROR_VARIABLE report)
if(NOT crc EQUAL 0)
    message(FATAL_ERROR "合法化结果未通过检查器（退出码 ${crc}）：\n${report}")
endif()
