cmake_minimum_required(VERSION 3.28...3.30)

# 支持 vcpkg
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
elseif(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    # 如果没有设置 VCPKG_ROOT，使用 GitHub 上的 vcpkg
    set(VCPKG_REPO "https://github.com/microsoft/vcpkg.git")
    set(VCPKG_ROOT "${CMAKE_BINARY_DIR}/vcpkg")
    if(NOT EXISTS "${VCPKG_ROOT}")
        message(STATUS "Cloning vcpkg from GitHub...")
        execute_process(
            COMMAND git clone --depth 1 ${VCPKG_REPO} ${VCPKG_ROOT}
            RESULT_VARIABLE GIT_RESULT
            OUTPUT_QUIET
            ERROR_QUIET
        )
        if(GIT_RESULT EQUAL 0)
            message(STATUS "vcpkg cloned successfully")
        else()
            message(WARNING "Failed to clone vcpkg, using fallback method")
        endif()
    endif()
    if(EXISTS "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
        message(STATUS "Using vcpkg from GitHub: ${VCPKG_ROOT}")
    else()
        message(WARNING "vcpkg not found, using system dependencies")
    endif()
endif()

# vcpkg 依赖
set(VCPKG_MANIFEST_FEATURES "gpu")
set(VCPKG_OVERLAY_PORTS "${CMAKE_SOURCE_DIR}/vcpkg-ports")
set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_SOURCE_DIR}/vcpkg-triplets")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

# 查找 libobs（使用默认的 find_package，因为 CI 环境中会构建 OBS）
find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)
message(STATUS "Using libobs from CI build")

# 检查是否存在 ONNX Runtime（优先使用 vcpkg）
option(ENABLE_ONNX "Enable ONNX Runtime support" ON)
if(ENABLE_ONNX)
    # 强制使用 vcpkg 查找 ONNX Runtime
    find_package(onnxruntime REQUIRED)
    if(onnxruntime_FOUND)
        target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE onnxruntime::onnxruntime)
        target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAVE_ONNX)
        message(STATUS "Found ONNX Runtime via vcpkg: ${onnxruntime_VERSION}")
    else()
        # 如果 vcpkg 失败，禁用 ONNX 支持
        message(STATUS "ONNX Runtime not found in vcpkg, disabling ONNX support")
        set(ENABLE_ONNX OFF)
    endif()
endif()

# 检查是否存在 OpenCV（优先使用 vcpkg）
option(ENABLE_OPENCV "Enable OpenCV support" ON)
if(ENABLE_OPENCV)
    # 强制使用 vcpkg 查找 OpenCV
    find_package(OpenCV REQUIRED)
    if(OpenCV_FOUND)
        target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${OpenCV_LIBS})
        target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAVE_OPENCV)
        message(STATUS "Found OpenCV via vcpkg: ${OpenCV_VERSION}")
    else()
        # 如果 vcpkg 失败，禁用 OpenCV 支持
        message(STATUS "OpenCV not found in vcpkg, disabling OpenCV support")
        set(ENABLE_OPENCV OFF)
    endif()
endif()

# 配置编译定义
if(NOT ENABLE_ONNX OR NOT ENABLE_OPENCV)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NO_INFERENCE)
    message(STATUS "Inference disabled due to missing dependencies")
else()
    message(STATUS "Inference enabled with GPU acceleration")
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
