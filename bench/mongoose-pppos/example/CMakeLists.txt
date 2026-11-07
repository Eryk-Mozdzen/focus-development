cmake_minimum_required(VERSION 3.16)
project(mongoose_pppos_linux C)

set(MG_MONGOOSE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../mongoose" CACHE PATH
    "Path to the Mongoose source checkout")
if(NOT EXISTS "${MG_MONGOOSE_DIR}/mongoose.c" OR
   NOT EXISTS "${MG_MONGOOSE_DIR}/mongoose.h")
  message(FATAL_ERROR "Set MG_MONGOOSE_DIR to a Mongoose checkout")
endif()

add_executable(pppos-linux
  linux_example.c
  "${CMAKE_CURRENT_LIST_DIR}/../mg_pppos.c"
  "${MG_MONGOOSE_DIR}/mongoose.c")
target_include_directories(pppos-linux PRIVATE
  "${MG_MONGOOSE_DIR}"
  "${CMAKE_CURRENT_LIST_DIR}/.."
  "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(pppos-linux PRIVATE
  MG_ENABLE_TCPIP=1
  MG_ENABLE_LWIP=0)
target_compile_features(pppos-linux PRIVATE c_std_99)
target_compile_options(pppos-linux PRIVATE -Wall -Wextra)

find_program(SOCAT_EXECUTABLE socat)
find_program(PPPD_EXECUTABLE pppd PATHS /usr/sbin /sbin)
if(SOCAT_EXECUTABLE AND PPPD_EXECUTABLE)
  add_custom_target(run-pty
    COMMAND "${CMAKE_CURRENT_LIST_DIR}/run_pty.sh"
            "$<TARGET_FILE:pppos-linux>" "${SOCAT_EXECUTABLE}"
            "${PPPD_EXECUTABLE}"
    DEPENDS pppos-linux
    USES_TERMINAL)
else()
  message(STATUS "PTY runner requires socat and pppd")
endif()
