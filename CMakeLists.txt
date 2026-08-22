cmake_minimum_required(VERSION 3.23)
project(WaterStructureCpp VERSION 0.1.7 LANGUAGES CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX LEVELDB_PLATFORM_WINDOWS LEVELDB_COMPILE_LIBRARY)
else()
    add_compile_definitions(LEVELDB_PLATFORM_POSIX LEVELDB_COMPILE_LIBRARY)
endif()

option(WATER_STRUCTURE_BUILD_SHARED "Build the shared C ABI library" ON)
option(WATER_STRUCTURE_BUILD_TOOLS "Build CLI and tests" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match xmake's Release intent without enabling non-standard floating-point
# semantics: conversion output and canonical hashes must remain reproducible
# across build systems and platforms.
if(MSVC)
    add_compile_options(/utf-8 /bigobj /wd4838 /wd4828)
    add_compile_options($<$<CONFIG:Release>:/O2>)
    add_compile_options($<$<CONFIG:RelWithDebInfo>:/O2>)
else()
    add_compile_options($<$<CONFIG:Release>:-O3>)
    add_compile_options($<$<CONFIG:RelWithDebInfo>:-O3>)
endif()

find_package(nlohmann_json 3.11 REQUIRED)
find_package(msgpack-cxx 7 REQUIRED)
find_package(Brotli REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

file(GLOB WATER_STRUCTURE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/world/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/formats/*.cpp")
file(GLOB LEVELDB_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/LevelDB/db/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/LevelDB/table/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/LevelDB/util/*.cc")
list(FILTER LEVELDB_SOURCES EXCLUDE REGEX "(_test\\.cc|leveldbutil\\.cc|testutil\\.cc)$")
if(WIN32)
    list(FILTER LEVELDB_SOURCES EXCLUDE REGEX "env_posix\\.cc$")
else()
    list(FILTER LEVELDB_SOURCES EXCLUDE REGEX "env_windows\\.cc$")
endif()
file(GLOB LIBNBT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libnbt/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libnbt/src/io/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libnbt/src/text/*.cpp")
file(GLOB BWO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/BedrockWorldOperator/src/*.cpp")

add_library(water_structure_leveldb STATIC ${LEVELDB_SOURCES})
target_include_directories(water_structure_leveldb PRIVATE
    thirdparty/LevelDB/include thirdparty/LevelDB)
target_link_libraries(water_structure_leveldb PUBLIC ZLIB::ZLIB)
target_link_libraries(water_structure_leveldb PUBLIC Threads::Threads)
if(WIN32)
    target_compile_definitions(water_structure_leveldb PUBLIC LEVELDB_PLATFORM_WINDOWS)
else()
    target_compile_definitions(water_structure_leveldb PUBLIC LEVELDB_PLATFORM_POSIX)
endif()

add_library(water_structure_nbt STATIC ${LIBNBT_SOURCES})
target_include_directories(water_structure_nbt PRIVATE thirdparty/libnbt)
target_link_libraries(water_structure_nbt PUBLIC ZLIB::ZLIB)

add_library(water_structure_bwo STATIC ${BWO_SOURCES})
target_include_directories(water_structure_bwo PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/BedrockWorldOperator/include>
    $<INSTALL_INTERFACE:include>)
target_include_directories(water_structure_bwo PRIVATE
    thirdparty/LevelDB/include thirdparty/LevelDB)
target_link_libraries(water_structure_bwo PUBLIC water_structure_leveldb)

add_library(water_structure STATIC src/c_api.cpp ${WATER_STRUCTURE_SOURCES})
set_target_properties(water_structure PROPERTIES OUTPUT_NAME water_structure_static)
target_include_directories(water_structure PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_include_directories(water_structure PRIVATE
    thirdparty/BedrockWorldOperator/include
    thirdparty/LevelDB/include thirdparty/LevelDB
    thirdparty/libnbt)
target_link_libraries(water_structure PUBLIC
    water_structure_bwo water_structure_nbt
    nlohmann_json::nlohmann_json msgpack-cxx Brotli::brotlidec Brotli::brotlienc ZLIB::ZLIB)
if(WIN32)
    target_link_libraries(water_structure PUBLIC bcrypt)
endif()

if(WATER_STRUCTURE_BUILD_SHARED)
    add_library(water_structure_shared SHARED src/c_api.cpp ${WATER_STRUCTURE_SOURCES})
    target_compile_definitions(water_structure_shared PRIVATE WATER_STRUCTURE_BUILD_SHARED)
    target_include_directories(water_structure_shared PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_include_directories(water_structure_shared PRIVATE
        thirdparty/BedrockWorldOperator/include
        thirdparty/LevelDB/include thirdparty/LevelDB
        thirdparty/libnbt)
    target_link_libraries(water_structure_shared PUBLIC
        water_structure_bwo water_structure_nbt
        nlohmann_json::nlohmann_json msgpack-cxx
        Brotli::brotlidec Brotli::brotlienc ZLIB::ZLIB)
    if(WIN32)
        target_link_libraries(water_structure_shared PUBLIC bcrypt)
    endif()
    set_target_properties(water_structure_shared PROPERTIES OUTPUT_NAME water_structure)
endif()

if(WATER_STRUCTURE_BUILD_TOOLS)
    add_executable(water_structure_cli src/cli/main.cpp)
    target_link_libraries(water_structure_cli PRIVATE water_structure)
    target_include_directories(water_structure_cli PRIVATE include)
    add_executable(water_structure_tests tests/core_tests.cpp)
    target_link_libraries(water_structure_tests PRIVATE water_structure)
    target_include_directories(water_structure_tests PRIVATE include)
    add_executable(cpp_manifest tools/cpp_manifest/main.cpp)
    target_link_libraries(cpp_manifest PRIVATE water_structure nlohmann_json::nlohmann_json)
    target_include_directories(cpp_manifest PRIVATE include)
    add_executable(world_compare tools/world_compare/main.cpp)
    target_link_libraries(world_compare PRIVATE water_structure)
    target_include_directories(world_compare PRIVATE include)
    add_executable(water_structure_bench benchmarks/main.cpp)
    target_link_libraries(water_structure_bench PRIVATE water_structure)
    target_include_directories(water_structure_bench PRIVATE include)
    if(WIN32)
        target_link_libraries(water_structure_bench PRIVATE psapi)
    endif()
endif()

include(GNUInstallDirs)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY thirdparty/BedrockWorldOperator/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY assets/ DESTINATION ${CMAKE_INSTALL_DATADIR}/water_structure/assets)
install(TARGETS water_structure water_structure_leveldb water_structure_nbt water_structure_bwo
    EXPORT WaterStructureTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(WATER_STRUCTURE_BUILD_SHARED)
    install(TARGETS water_structure_shared
        EXPORT WaterStructureTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(EXPORT WaterStructureTargets
    FILE WaterStructureTargets.cmake
    NAMESPACE WaterStructure::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WaterStructure)

include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/WaterStructureConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/WaterStructureConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WaterStructure)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/WaterStructureConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/WaterStructureConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/WaterStructureConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/WaterStructure)
