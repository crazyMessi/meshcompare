cmake_minimum_required(VERSION 3.18)

project(meshcompare VERSION 0.1.0 LANGUAGES C CXX)

option(BUILD_TESTING "Build the Mesh Compare test suite." OFF)
include(CTest)

# Keep the customized viewport engine in-tree so a checkout never depends on
# the state, location, or availability of a sibling MeshLab repository.
set(
	MESHCOMPARE_MESHLAB_SOURCE_DIR
	"${CMAKE_CURRENT_SOURCE_DIR}/third_party/meshlab")

set(
	MESHCOMPARE_DISTRIB_DIR
	"${CMAKE_BINARY_DIR}/dist"
	CACHE PATH
	"Directory that receives the built Mesh Compare application.")
option(
	MESHCOMPARE_DEPLOY_QT
	"Bundle Qt frameworks and platform plugins into the macOS application."
	ON)
option(
	MESHCOMPARE_ADHOC_SIGN
	"Ad-hoc sign the completed local macOS application bundle."
	ON)

set(MESHCOMPARE_MESHLAB_SRC_DIR "${MESHCOMPARE_MESHLAB_SOURCE_DIR}/src")
set(MESHCOMPARE_VCG_DIR "${MESHCOMPARE_MESHLAB_SRC_DIR}/vcglib")
set(MESHCOMPARE_EXTERNAL_DIR "${MESHCOMPARE_MESHLAB_SRC_DIR}/external")
set(MESHCOMPARE_ENGINE_CONTEXT_HEADER
	"${MESHCOMPARE_MESHLAB_SRC_DIR}/common/ml_shared_data_context/ml_scene_gl_shared_data_context.h")
set(MESHCOMPARE_ENGINE_BUFFER_HEADER
	"${MESHCOMPARE_VCG_DIR}/wrap/gl/gl_mesh_attributes_multi_viewer_bo_manager.h")

foreach(required_path
		"${MESHCOMPARE_MESHLAB_SOURCE_DIR}/ML_VERSION"
		"${MESHCOMPARE_MESHLAB_SOURCE_DIR}/LICENSE.txt"
		"${MESHCOMPARE_MESHLAB_SRC_DIR}/cmake/meshlab_global_settings.cmake"
		"${MESHCOMPARE_MESHLAB_SRC_DIR}/cmake/meshlab_tools.cmake"
		"${MESHCOMPARE_MESHLAB_SRC_DIR}/common/CMakeLists.txt"
		"${MESHCOMPARE_MESHLAB_SRC_DIR}/meshlabplugins/io_base/CMakeLists.txt"
		"${MESHCOMPARE_VCG_DIR}/CMakeLists.txt"
		"${MESHCOMPARE_VCG_DIR}/LICENSE.txt"
		"${MESHCOMPARE_VCG_DIR}/eigenlib/Eigen/Eigen"
		"${MESHCOMPARE_VCG_DIR}/eigenlib/COPYING.MPL2"
		"${MESHCOMPARE_VCG_DIR}/wrap/openfbx/LICENSE"
		"${MESHCOMPARE_EXTERNAL_DIR}/glew-2.2.0/src/glew.c"
		"${MESHCOMPARE_EXTERNAL_DIR}/easyexif-1.0/exif.cpp"
		"${MESHCOMPARE_ENGINE_CONTEXT_HEADER}"
		"${MESHCOMPARE_ENGINE_BUFFER_HEADER}")
	if(NOT EXISTS "${required_path}")
		message(FATAL_ERROR
			"Vendored viewport engine dependency is incomplete: ${required_path}")
	endif()
endforeach()

# Mesh Compare relies on the fail-closed GL lifecycle added to the customized
# MeshLab viewport engine. Fail during configuration instead of silently
# building against an incomplete snapshot that can crash after recoloring.
file(READ "${MESHCOMPARE_ENGINE_CONTEXT_HEADER}" engine_context_header)
if(NOT engine_context_header MATCHES "bool tryInitializeGL\\(\\)")
	message(FATAL_ERROR
		"The vendored engine does not provide the required "
		"MLSceneGLSharedDataContext::tryInitializeGL engine contract.")
endif()
file(READ "${MESHCOMPARE_ENGINE_BUFFER_HEADER}" engine_buffer_header)
if(NOT engine_buffer_header MATCHES "areTriangleBuffersReady"
		OR NOT engine_buffer_header MATCHES "_memorytracked")
	message(FATAL_ERROR
		"The vendored engine does not contain the required "
		"fail-closed VCGLib buffer manager fix.")
endif()
unset(engine_context_header)
unset(engine_buffer_header)

set(MESHLAB_BUILD_MINI ON CACHE BOOL "" FORCE)
set(MESHLAB_BUILD_STRICT ON CACHE BOOL "" FORCE)
set(MESHLAB_BUILD_WITH_DOUBLE_SCALAR OFF CACHE BOOL "" FORCE)
set(MESHLAB_ENABLE_DEBUG_LOG_FILE OFF CACHE BOOL "" FORCE)
set(MESHLAB_IS_NIGHTLY_VERSION OFF CACHE BOOL "" FORCE)
set(MESHLAB_ALLOW_OPTIONAL_EXTERNAL_LIBRARIES OFF CACHE BOOL "" FORCE)
set(MESHLAB_ALLOW_SYSTEM_GLEW OFF CACHE BOOL "" FORCE)
set(MESHLAB_ALLOW_BUNDLED_SOURCE_GLEW ON CACHE BOOL "" FORCE)
set(MESHLAB_ALLOW_BUNDLED_SOURCE_EASYEXIF ON CACHE BOOL "" FORCE)
set(VCG_ALLOW_SYSTEM_EIGEN OFF CACHE BOOL "" FORCE)
set(VCG_ALLOW_BUNDLED_EIGEN ON CACHE BOOL "" FORCE)
set(VCG_USE_OPENMP OFF CACHE BOOL "" FORCE)
set(VCG_HEADER_ONLY ON CACHE BOOL "" FORCE)
set(VCG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

list(APPEND CMAKE_MODULE_PATH "${MESHCOMPARE_MESHLAB_SRC_DIR}/cmake")
include(
	"${MESHCOMPARE_MESHLAB_SRC_DIR}/cmake/meshlab_global_settings.cmake"
	NO_POLICY_SCOPE)
include("${MESHCOMPARE_MESHLAB_SRC_DIR}/cmake/meshlab_tools.cmake")

find_package(OpenGL REQUIRED)
find_package(
	Qt5 5.15
	COMPONENTS Core Gui Widgets OpenGL Xml Network
	REQUIRED)
if(BUILD_TESTING)
	find_package(Qt5 5.15 COMPONENTS Test REQUIRED)
endif()

set(MESHLAB_LIB_OUTPUT_DIR "${CMAKE_BINARY_DIR}/engine/lib")
# MeshLab's default plugin discovery walks parent directories from each
# executable and looks for a directory named "plugins". Keeping the build-tree
# plugin here lets tests use the same discovery path as the packaged app.
set(MESHLAB_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/plugins")
set(MESHLAB_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/engine/shaders")
set(MESHLAB_LIB_INSTALL_DIR "meshcompare.app/Contents/Frameworks")
set(MESHLAB_PLUGIN_INSTALL_DIR "meshcompare.app/Contents/PlugIns")
set(MESHLAB_SHADER_INSTALL_DIR "meshcompare.app/Contents/shaders")

set(VCGDIR "${MESHCOMPARE_VCG_DIR}")
add_subdirectory(
	"${MESHCOMPARE_VCG_DIR}"
	"${CMAKE_BINARY_DIR}/_deps/vcglib"
	EXCLUDE_FROM_ALL)

set(EXTERNAL_DIR "${MESHCOMPARE_EXTERNAL_DIR}")
add_subdirectory(
	"${EXTERNAL_DIR}"
	"${CMAKE_BINARY_DIR}/_deps/meshlab-external"
	EXCLUDE_FROM_ALL)
add_subdirectory(
	"${MESHCOMPARE_MESHLAB_SRC_DIR}/common"
	"${CMAKE_BINARY_DIR}/_deps/meshlab-common"
	EXCLUDE_FROM_ALL)
add_subdirectory(
	"${MESHCOMPARE_MESHLAB_SRC_DIR}/meshlabplugins/io_base"
	"${CMAKE_BINARY_DIR}/_deps/io-base"
	EXCLUDE_FROM_ALL)

if(APPLE)
	include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/BundleMacOS.cmake")
endif()

add_subdirectory(src)
