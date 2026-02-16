cmake_minimum_required(VERSION 3.28...3.30)

# 支持 vcpkg
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
endif()

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

# 检查是否存在 ONNX Runtime
option(ENABLE_ONNX "Enable ONNX Runtime support" ON)
if(ENABLE_ONNX)
    # 尝试使用 find_package 查找 ONNX Runtime
    find_package(onnxruntime QUIET)
    if(onnxruntime_FOUND)
        target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE onnxruntime::onnxruntime)
        target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAVE_ONNX)
        message(STATUS "Found ONNX Runtime via find_package")
    else()
        # 如果 find_package 失败，尝试使用环境变量
        if(DEFINED ENV{ORT_PATH})
            target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "$ENV{ORT_PATH}/include")
            target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE "$ENV{ORT_PATH}/lib")
            target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE onnxruntime)
            target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAVE_ONNX)
            message(STATUS "Using ONNX Runtime from ENV{ORT_PATH}: $ENV{ORT_PATH}")
        else()
            # 如果环境变量也没有设置，禁用 ONNX 支持
            message(STATUS "ONNX Runtime not found, disabling ONNX support")
            set(ENABLE_ONNX OFF)
        endif()
    endif()
endif()

# 检查是否存在 OpenCV
option(ENABLE_OPENCV "Enable OpenCV support" ON)
if(ENABLE_OPENCV)
    # 尝试使用 find_package 查找 OpenCV
    find_package(OpenCV QUIET)
    if(OpenCV_FOUND)
        target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${OpenCV_LIBS})
        target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAVE_OPENCV)
        message(STATUS "Found OpenCV via find_package: ${OpenCV_VERSION}")
    else()
        # 如果 find_package 失败，尝试使用环境变量
        if(DEFINED ENV{OPENCV_PATH})
            target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "$ENV{OPENCV_PATH}/include")
            target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE "$ENV{OPENCV_PATH}/x64/vc16/lib")
            target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE opencv_world4100)
            target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAVE_OPENCV)
            message(STATUS "Using OpenCV from ENV{OPENCV_PATH}: $ENV{OPENCV_PATH}")
        else()
            # 如果环境变量也没有设置，禁用 OpenCV 支持
            message(STATUS "OpenCV not found, disabling OpenCV support")
            set(ENABLE_OPENCV OFF)
        endif()
    endif()
endif()

# 配置编译定义
if(NOT ENABLE_ONNX OR NOT ENABLE_OPENCV)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NO_INFERENCE)
    message(STATUS "Inference disabled due to missing dependencies")
endif()

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
