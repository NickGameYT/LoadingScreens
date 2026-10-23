cmake_minimum_required(VERSION 3.24)

project(LoadingScreens VERSION 1.3.11 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

include(FetchContent)

FetchContent_Declare(
	fmt
	GIT_REPOSITORY https://github.com/fmtlib/fmt.git
	GIT_TAG 10.2.1
	GIT_SHALLOW TRUE
)
set(FMT_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(fmt)
set(fmt_DIR "${fmt_BINARY_DIR}" CACHE PATH "" FORCE)

FetchContent_Declare(
	spdlog
	GIT_REPOSITORY https://github.com/gabime/spdlog.git
	GIT_TAG v1.14.1
	GIT_SHALLOW TRUE
)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)
set(spdlog_DIR "${spdlog_BINARY_DIR}" CACHE PATH "" FORCE)

FetchContent_Declare(
	rapidcsv
	GIT_REPOSITORY https://github.com/d99kris/rapidcsv.git
	GIT_TAG v8.84
	GIT_SHALLOW TRUE
)
FetchContent_GetProperties(rapidcsv)
if(NOT rapidcsv_POPULATED)
	FetchContent_Populate(rapidcsv)
endif()
set(RAPIDCSV_INCLUDE_DIRS "${rapidcsv_SOURCE_DIR}/src" CACHE PATH "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

set(CommonLibSSE_ROOT "" CACHE PATH "Path to a local CommonLibSSE-NG checkout")
if(CommonLibSSE_ROOT)
	add_subdirectory("${CommonLibSSE_ROOT}" "${CMAKE_BINARY_DIR}/CommonLibSSE-NG")
else()
	FetchContent_Declare(
		CommonLibSSE
		GIT_REPOSITORY https://github.com/CharmedBaryon/CommonLibSSE-NG.git
		GIT_TAG main
		GIT_SHALLOW TRUE
	)
	FetchContent_MakeAvailable(CommonLibSSE)
endif()

set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBP_JS OFF CACHE BOOL "" FORCE)
set(WEBP_NEAR_LOSSLESS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
	libwebp
	GIT_REPOSITORY https://github.com/webmproject/libwebp.git
	GIT_TAG v1.4.0
	GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libwebp)

add_library(${PROJECT_NAME} SHARED src/main.cpp src/media.cpp)

target_include_directories(${PROJECT_NAME} PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/extern"
	"${rapidcsv_SOURCE_DIR}/src"
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_compile_options(${PROJECT_NAME} PRIVATE /utf-8 /EHsc)

target_compile_definitions(
	${PROJECT_NAME}
	PRIVATE
		NOMINMAX
		WIN32_LEAN_AND_MEAN
		UNICODE
		_UNICODE
)

target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
		CommonLibSSE::CommonLibSSE
		d3d11
		dxgi
		d3dcompiler
		mfplat
		mfreadwrite
		mfuuid
		xaudio2
		ole32
		windowscodecs
		webp
		webpdemux
)

set(SKSE_PLUGIN_DIR "" CACHE PATH "Optional folder where LoadingScreens.dll is copied after build")
if(SKSE_PLUGIN_DIR)
	foreach(CONFIG Debug Release RelWithDebInfo MinSizeRel)
		string(TOUPPER "${CONFIG}" CONFIG_UPPER)
		set_target_properties(${PROJECT_NAME} PROPERTIES
			RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${SKSE_PLUGIN_DIR}"
			LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${SKSE_PLUGIN_DIR}"
		)
	endforeach()
endif()
