cmake_minimum_required(VERSION 3.16)
project(AntiHackerX VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Qt6
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)

# zlib (minizip 的依赖)
find_package(ZLIB REQUIRED)

# 内嵌的 minizip 源码 (用于 JAR 文件分析)
set(MINIZIP_SOURCES
    libs/minizip/ioapi.c
    libs/minizip/unzip.c
)

set(MINIZIP_HEADERS
    libs/minizip/ioapi.h
    libs/minizip/unzip.h
)

# 源文件
set(SOURCES
    src/main.cpp
    src/mainwindow.cpp
    src/obfuscator_controller.cpp
    src/downloader.cpp
    src/download_dialog.cpp
    src/archive.cpp
    src/runtime_bootstrap.cpp
    src/jar_analyzer.cpp
    src/class_scanner.cpp
    src/verify_module.cpp
    src/packer_pipeline.cpp
    src/pack_progress_dialog.cpp
    ${MINIZIP_SOURCES}
)

set(HEADERS
    src/mainwindow.h
    src/obfuscator_controller.h
    src/downloader.h
    src/download_dialog.h
    src/archive.h
    src/runtime_bootstrap.h
    src/app_logger.h
    src/jar_analyzer.h
    src/class_scanner.h
    src/verify_module.h
    src/packer_pipeline.h
    src/pack_progress_dialog.h
    ${MINIZIP_HEADERS}
)

# 内嵌资产:验证模块源码 + 入口模板。
# 资产清单在验证模块自己的工程里(AntiHackerXVerify/verify.qrc),便于其自描述。
# CMAKE_AUTORCC 已开启,把 .qrc 直接加入目标源即可。
set(RESOURCES
    ${CMAKE_SOURCE_DIR}/AntiHackerXVerify/verify.qrc
)

# 可执行文件
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS} ${RESOURCES})

# 添加 minizip 头文件路径 (内嵌源码)
target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_SOURCE_DIR}/libs/minizip
)

# 链接 Qt 库
# Network 用于启动引导阶段的 Java / 依赖下载
# (GPL 组件仍然只以独立进程方式调用,不产生链接,故不构成传染)
target_link_libraries(${PROJECT_NAME} PRIVATE 
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    ZLIB::ZLIB
)

# 安装规则
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
)

# Windows 特定设置
if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        WIN32_EXECUTABLE ON
    )
endif()

# 输出目录
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# 可选的命令行测试工具:直接验证类扫描结果(不启动 GUI)
#   cmake -DBUILD_SCANNER_TEST=ON .. && make test_scanner
# ---------------------------------------------------------------------------
option(BUILD_SCANNER_TEST "Build the CLI class scanner test tool" OFF)
if(BUILD_SCANNER_TEST)
    add_executable(test_scanner
        testp/test_scanner.cpp
        src/class_scanner.cpp
        ${MINIZIP_SOURCES}
    )
    target_include_directories(test_scanner PRIVATE ${CMAKE_SOURCE_DIR}/libs/minizip)
    target_link_libraries(test_scanner PRIVATE Qt6::Core ZLIB::ZLIB)
    set_target_properties(test_scanner PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

# 可选的命令行测试工具:验证 VerifyModule 的源码释放与入口生成
#   cmake -DBUILD_VERIFY_TEST=ON .. && make test_verify_module
option(BUILD_VERIFY_TEST "Build the VerifyModule CLI test tool" OFF)
if(BUILD_VERIFY_TEST)
    add_executable(test_verify_module
        testp/test_verify_module.cpp
        src/verify_module.cpp
        ${CMAKE_SOURCE_DIR}/AntiHackerXVerify/verify.qrc
    )
    target_link_libraries(test_verify_module PRIVATE Qt6::Core)
    set_target_properties(test_verify_module PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

# 可选的命令行测试工具:跑完整加壳流水线
#   cmake -DBUILD_PIPELINE_TEST=ON .. && make test_pipeline
option(BUILD_PIPELINE_TEST "Build the full packing pipeline CLI test tool" OFF)
if(BUILD_PIPELINE_TEST)
    add_executable(test_pipeline
        testp/test_pipeline.cpp
        src/verify_module.cpp
        src/packer_pipeline.cpp
        ${MINIZIP_SOURCES}
        ${CMAKE_SOURCE_DIR}/AntiHackerXVerify/verify.qrc
    )
    target_include_directories(test_pipeline PRIVATE ${CMAKE_SOURCE_DIR}/libs/minizip)
    target_link_libraries(test_pipeline PRIVATE Qt6::Core ZLIB::ZLIB)
    set_target_properties(test_pipeline PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()
