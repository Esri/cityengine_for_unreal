cmake_minimum_required(VERSION 3.27)

set(CMAKE_CONFIGURATION_TYPES "Release;RelWithDebInfo" CACHE STRING "Supported configurations")
set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build configuration")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
project(UnrealGeometryEncoder LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
	message(FATAL_ERROR "Use an out-of-source build: cmake -S . -B Build")
endif()
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
NOT MSVC_CXX_ARCHITECTURE_ID STREQUAL "x64")
	message(FATAL_ERROR "UnrealGeometryEncoder requires Windows x64 and MSVC. Use an x64 Native Tools command prompt.")
endif()
foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES CMAKE_BUILD_TYPE)
	if(NOT config MATCHES "^(Release|RelWithDebInfo)$")
		message(FATAL_ERROR "Unsupported configuration '${config}': the PRT SDK requires Release or RelWithDebInfo with /MD.")
	endif()
endforeach()

get_filename_component(PLUGIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(PRT_METADATA_FILE "${PLUGIN_DIR}/Source/ThirdParty/PRT/PRT.version.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PRT_METADATA_FILE}")
file(READ "${PRT_METADATA_FILE}" PRT_METADATA)
foreach(field IN ITEMS major minor build toolchain sha256)
	string(JSON prt_${field} ERROR_VARIABLE error GET "${PRT_METADATA}" "${field}")
	if(error)
		message(FATAL_ERROR "Invalid ${PRT_METADATA_FILE}: ${error}")
	endif()
endforeach()
foreach(field IN ITEMS major minor build)
	if(NOT prt_${field} MATCHES "^[0-9]+$" OR prt_${field} GREATER 2147483647)
		message(FATAL_ERROR "Invalid ${PRT_METADATA_FILE}: ${field} must be a nonnegative integer.")
	endif()
endforeach()
if(prt_major EQUAL 0 OR prt_build EQUAL 0)
	message(FATAL_ERROR "Invalid ${PRT_METADATA_FILE}: major and build must be positive.")
endif()
string(LENGTH "${prt_sha256}" hash_length)
if(NOT hash_length EQUAL 64 OR NOT prt_sha256 MATCHES "^[0-9a-fA-F]+$")
	message(FATAL_ERROR "Invalid ${PRT_METADATA_FILE}: sha256 must contain 64 hexadecimal digits.")
endif()
if(NOT prt_toolchain MATCHES "^win[0-9]+-vc14([0-9][0-9])-x86_64-rel-opt$")
	message(FATAL_ERROR "Invalid ${PRT_METADATA_FILE}: expected a Windows MSVC 14.xx x64 release toolchain.")
endif()
set(PRT_MSVC_MINOR "${CMAKE_MATCH_1}")
math(EXPR PRT_MSVC_VERSION "1900 + ${PRT_MSVC_MINOR}")
if(NOT MSVC_VERSION EQUAL PRT_MSVC_VERSION)
	message(FATAL_ERROR "PRT ${prt_toolchain} requires MSVC 14.${PRT_MSVC_MINOR}. Run vcvars64.bat -vcvars_ver=14.${PRT_MSVC_MINOR} and configure a fresh build directory.")
endif()

set(PRT_EXPECTED_VERSION "${prt_major}.${prt_minor}.${prt_build}")
set(PRT_ARCHIVE "esri_ce_sdk-${PRT_EXPECTED_VERSION}-${prt_toolchain}.zip")
include(FetchContent)
FetchContent_Declare(prt_sdk
	URL "https://github.com/Esri/cityengine-sdk/releases/download/${PRT_EXPECTED_VERSION}/${PRT_ARCHIVE}"
	URL_HASH "SHA256=${prt_sha256}"
	TLS_VERIFY TRUE
)
FetchContent_MakeAvailable(prt_sdk)

set(prt_DIR "${prt_sdk_SOURCE_DIR}/cmake" CACHE PATH "CMake-managed PRT SDK package" FORCE)
# The SDK's find_library calls otherwise retain paths from an earlier package.
get_cmake_property(cached_variables CACHE_VARIABLES)
foreach(variable IN LISTS cached_variables)
	if(variable MATCHES "^PRT_.*_LIBRARY$")
		unset(${variable} CACHE)
	endif()
endforeach()
find_package(prt CONFIG REQUIRED PATHS "${prt_DIR}" NO_DEFAULT_PATH)
if(NOT "${PRT_VERSION_MAJOR}.${PRT_VERSION_MINOR}.${PRT_VERSION_MICRO}" STREQUAL PRT_EXPECTED_VERSION)
	message(FATAL_ERROR "Downloaded PRT package does not match ${PRT_EXPECTED_VERSION}.")
endif()
foreach(required_file IN ITEMS "${PRT_INCLUDE_PATH}/prt/API.h" "${PRT_INCLUDE_PATH}/prtx/Encoder.h"
		"${PRT_CORE_LIBRARY}" "${prt_sdk_SOURCE_DIR}/bin/com.esri.prt.core.dll")
	if(NOT EXISTS "${required_file}")
		message(FATAL_ERROR "Incomplete PRT SDK: missing ${required_file}")
	endif()
endforeach()
find_program(POWERSHELL_EXECUTABLE NAMES powershell REQUIRED)
string(REPLACE "'" "''" PRT_CORE_DLL "${PRT_CORE_SHARED_LIBRARY}")
execute_process(
	COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive -Command
		"[System.Diagnostics.FileVersionInfo]::GetVersionInfo('${PRT_CORE_DLL}').FileVersion"
	OUTPUT_VARIABLE PRT_DLL_VERSION
	OUTPUT_STRIP_TRAILING_WHITESPACE
	COMMAND_ERROR_IS_FATAL ANY
)
if(NOT PRT_DLL_VERSION MATCHES "^${prt_major}\\.${prt_minor}\\.[0-9]+ Build ${prt_build}$")
	message(FATAL_ERROR "PRT core DLL version '${PRT_DLL_VERSION}' does not match ${PRT_EXPECTED_VERSION}.")
endif()
message(STATUS "Using prebuilt PRT ${PRT_EXPECTED_VERSION}: ${prt_sdk_SOURCE_DIR}")

add_library(UnrealGeometryEncoder SHARED
	Private/Codec/CodecMain.cpp
	Private/Codec/Encoder/UnrealGeometryEncoder.cpp
)
set_target_properties(UnrealGeometryEncoder PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED YES
	CXX_EXTENSIONS NO
	MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase
)
target_include_directories(UnrealGeometryEncoder PRIVATE Public Private "${PRT_INCLUDE_PATH}")
target_compile_definitions(UnrealGeometryEncoder PRIVATE
	PRT_VERSION_MAJOR=${prt_major}
	PRT_VERSION_MINOR=${prt_minor}
	_ITERATOR_DEBUG_LEVEL=0
)
target_compile_options(UnrealGeometryEncoder PRIVATE /bigobj /GR /EHsc)
target_link_libraries(UnrealGeometryEncoder PRIVATE "${PRT_CORE_LIBRARY}" delayimp)
target_link_options(UnrealGeometryEncoder PRIVATE /DEBUG /OPT:REF /OPT:ICF /DELAYLOAD:com.esri.prt.core.dll)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
	set(CMAKE_INSTALL_PREFIX "${PLUGIN_DIR}/Source/ThirdParty/UnrealGeometryEncoderLib"
		CACHE PATH "Encoder installation directory" FORCE)
endif()
install(TARGETS UnrealGeometryEncoder
	RUNTIME DESTINATION lib/Win64/Release
	ARCHIVE DESTINATION lib/Win64/Release
)
install(FILES "$<TARGET_PDB_FILE:UnrealGeometryEncoder>"
	"$<TARGET_LINKER_FILE_DIR:UnrealGeometryEncoder>/UnrealGeometryEncoder.exp"
	DESTINATION lib/Win64/Release
)
install(DIRECTORY Public/ DESTINATION include)
