string(RANDOM LENGTH 8 ALPHABET "0123456789abcdef" _sx_rand)
if(DEFINED ENV{TEMP} AND NOT "$ENV{TEMP}" STREQUAL "")
    set(_sx_tmp_base "$ENV{TEMP}")
elseif(DEFINED ENV{TMP} AND NOT "$ENV{TMP}" STREQUAL "")
    set(_sx_tmp_base "$ENV{TMP}")
else()
    set(_sx_tmp_base "/tmp")
endif()
file(TO_CMAKE_PATH "${_sx_tmp_base}/scarlettx_boot_rt_${_sx_rand}" _sx_tmp_dir)
file(MAKE_DIRECTORY "${_sx_tmp_dir}")
set(_sx_ppm "${_sx_tmp_dir}/rt_showcase_frame.ppm")

execute_process(
    COMMAND "${SCARLETTX_EXE}" boot "${PACKAGE_DIR}" --headless --warp --frames 2 --rt-dump "${_sx_ppm}"
    RESULT_VARIABLE _sx_rc
)

set(_sx_exists FALSE)
if(EXISTS "${_sx_ppm}")
    set(_sx_exists TRUE)
endif()

file(REMOVE_RECURSE "${_sx_tmp_dir}")

if(NOT _sx_rc EQUAL 0)
    message(FATAL_ERROR "scarlettx boot rt_showcase failed with exit code ${_sx_rc}")
endif()
if(NOT _sx_exists)
    message(FATAL_ERROR "Expected RT dump file was not written: ${_sx_ppm}")
endif()
