cmake_minimum_required(VERSION 3.28...3.30)

# 支持 vcpkg
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
endif()

# vcpkg 依赖
set(VCPKG_MANIFEST_FEATURES "gpu")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

# 使用 vcpkg 管理依赖
find_package(onnxruntime REQUIRED)
find_package(OpenCV REQUIRED)

# 链接依赖
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE onnxruntime::onnxruntime)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${OpenCV_LIBS})

# 添加编译定义
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAVE_ONNX)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAVE_OPENCV)

message(STATUS "Using ONNX Runtime from vcpkg: ${onnxruntime_VERSION}")
message(STATUS "Using OpenCV from vcpkg: ${OpenCV_VERSION}")

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

install(FILES data/center_roi_gpu.effect DESTINATION ${CMAKE_INSTALL_PREFIX}/data/obs-plugins/${_name})
