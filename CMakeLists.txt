cmake_minimum_required(VERSION 3.15)

project(justjoy VERSION 0.1.1 LANGUAGES CXX)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()
cmake_policy(SET CMP0074 NEW)
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED On)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(FetchContent)

set(params
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-reorder
    -Wno-strict-aliasing
    -Wno-unused-but-set-variable
)

find_package(PkgConfig REQUIRED)
pkg_search_module(LIBEVDEV REQUIRED libevdev)

FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz)
FetchContent_MakeAvailable(json)


install(
    DIRECTORY include/
    DESTINATION include
)
include_directories(include)

set(exe
  src/client.cpp
)

set(lib
)

set(dependencies
    ${LIBEVDEV_LIBRARIES}
    nlohmann_json::nlohmann_json
)

set(exec_names)


foreach(src_file IN LISTS exe)
  get_filename_component(exec_name "${src_file}" NAME_WE)
  add_executable(${exec_name} "${src_file}")
  foreach(lib_file IN LISTS lib)
    target_compile_options(${exec_name} PRIVATE ${params})
    target_sources(${exec_name} PRIVATE "${lib_file}")
  endforeach()
  target_link_libraries(${exec_name} ${dependencies})
  install(TARGETS ${exec_name} DESTINATION bin)
  list(APPEND exec_names ${exec_name})
endforeach()


foreach(exec IN LISTS exec_names)
  file(REMOVE "${CMAKE_CURRENT_LIST_DIR}/.execs")
  file(WRITE "${CMAKE_CURRENT_LIST_DIR}/.execs")
  get_filename_component(exec_name "${exec}" NAME)
  file(APPEND "${CMAKE_CURRENT_LIST_DIR}/.execs" "${CMAKE_CURRENT_LIST_DIR}/build/${exec_name}\n")
endforeach()

install(TARGETS
  ${exec_names}
  DESTINATION lib/${PROJECT_NAME}
)
