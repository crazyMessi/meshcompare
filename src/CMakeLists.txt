set(MESHCOMPARE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(MESHCOMPARE_TEST_ROOT "${PROJECT_SOURCE_DIR}/tests")

add_library(meshcompare-core STATIC
	core/analysis_color_map.cpp
	core/analysis_color_map.h
	core/camera_pose_uuid.cpp
	core/camera_pose_uuid.h
	core/meshcompare_types.cpp
	core/meshcompare_types.h
	core/mesh_resource_provider.h
	core/reference_resolver.cpp
	core/reference_resolver.h
	core/renderer_adapter.h
	core/workspace_state.cpp
	core/workspace_state.h)
target_link_libraries(meshcompare-core PUBLIC Qt5::Widgets)
target_include_directories(meshcompare-core PUBLIC ${MESHCOMPARE_SOURCE_ROOT})

add_library(meshcompare-camera-pose-store STATIC
	services/camera_pose_store.cpp
	services/camera_pose_store.h
	services/camera_pose_store_file_ops.cpp
	services/camera_pose_store_file_ops.h)
target_include_directories(meshcompare-camera-pose-store PUBLIC ${MESHCOMPARE_SOURCE_ROOT})
target_link_libraries(meshcompare-camera-pose-store
	PUBLIC meshcompare-core
	PRIVATE Qt5::Core Qt5::Xml)

add_library(meshcompare-analysis STATIC
	services/surface_comparison.cpp
	services/surface_comparison.h)
target_include_directories(meshcompare-analysis PUBLIC ${MESHCOMPARE_SOURCE_ROOT})
target_link_libraries(meshcompare-analysis
	PUBLIC meshcompare-core Qt5::Gui
	PRIVATE meshlab-common)

add_library(meshcompare-color-service STATIC
	services/mesh_color_service.cpp
	services/mesh_color_service.h)
target_include_directories(meshcompare-color-service PUBLIC ${MESHCOMPARE_SOURCE_ROOT})
target_link_libraries(meshcompare-color-service
	PUBLIC meshcompare-core meshcompare-analysis)

add_library(meshcompare-startup-command-line STATIC
	app/startup_command_line.cpp
	app/startup_command_line.h)
target_include_directories(meshcompare-startup-command-line
	PUBLIC ${MESHCOMPARE_SOURCE_ROOT})
target_link_libraries(meshcompare-startup-command-line PUBLIC Qt5::Core)

add_library(meshcompare-meshlab-infrastructure STATIC
	services/mesh_import_service.cpp
	services/mesh_import_service.h
	infrastructure/meshlab/meshlab_mesh_repository.cpp
	infrastructure/meshlab/meshlab_mesh_repository.h
	infrastructure/meshlab/meshlab_mesh_loader.cpp
	infrastructure/meshlab/meshlab_mesh_loader.h)
target_link_libraries(meshcompare-meshlab-infrastructure
	PUBLIC meshcompare-core
	PRIVATE meshlab-common)
target_include_directories(meshcompare-meshlab-infrastructure
	PUBLIC ${MESHCOMPARE_SOURCE_ROOT})

add_library(meshcompare-ui STATIC
	app/application_startup.cpp
	app/application_startup.h
	app/camera_commands.h
	app/camera_panel.cpp
	app/camera_panel.h
	app/tag_editor.cpp
	app/tag_editor.h
	app/camera_pose_paths.cpp
	app/camera_pose_paths.h
	app/coloring_commands.h
	app/coloring_panel.cpp
	app/coloring_panel.h
	app/diagnostics_menu.cpp
	app/diagnostics_menu.h
	app/standalone_main_window.cpp
	app/standalone_main_window.h
	app/app_theme.cpp
	app/app_theme.h
	app/workspace_controller.cpp
	app/workspace_controller.h
	services/diagnostics_log.cpp
	services/diagnostics_log.h)
target_link_libraries(meshcompare-ui
	PUBLIC
		meshcompare-core
		meshcompare-camera-pose-store
		meshcompare-color-service
		Qt5::Widgets
	PRIVATE meshcompare-meshlab-infrastructure)

add_library(meshcompare-meshlab-viewport STATIC
	renderer/meshlab/glarea_setting.cpp
	renderer/meshlab/glarea_setting.h
	renderer/meshlab/mesh_lab_viewport.cpp
	renderer/meshlab/mesh_lab_viewport.h
	renderer/meshlab/viewport_dependencies.h
	${MESHCOMPARE_VCG_DIR}/wrap/gui/trackball.cpp
	${MESHCOMPARE_VCG_DIR}/wrap/gui/trackmode.cpp)
target_link_libraries(meshcompare-meshlab-viewport
	PUBLIC meshcompare-core meshlab-common Qt5::OpenGL Qt5::Widgets
	PRIVATE OpenGL::GLU Qt5::Xml)
target_include_directories(meshcompare-meshlab-viewport
	PUBLIC ${MESHCOMPARE_SOURCE_ROOT})

add_library(meshcompare-meshlab-renderer STATIC
	renderer/meshlab/render_scene_context.cpp
	renderer/meshlab/render_scene_context.h
	renderer/meshlab/mesh_lab_renderer_adapter.cpp
	renderer/meshlab/mesh_lab_renderer_adapter.h
	renderer/meshlab/viewport_grid.cpp
	renderer/meshlab/viewport_grid.h
	renderer/meshlab/viewport_factory.h)
target_link_libraries(meshcompare-meshlab-renderer
	PUBLIC meshcompare-core meshcompare-meshlab-viewport
	PRIVATE meshlab-common Qt5::OpenGL OpenGL::GL)
target_include_directories(meshcompare-meshlab-renderer
	PUBLIC ${MESHCOMPARE_SOURCE_ROOT})

add_library(meshcompare-grid-render STATIC
	app/grid_renderer.cpp
	app/grid_renderer.h)
target_link_libraries(meshcompare-grid-render
	PUBLIC
		meshcompare-core
		meshcompare-meshlab-infrastructure
		meshcompare-meshlab-renderer
		meshcompare-color-service
		Qt5::Widgets
		Qt5::Xml)
target_include_directories(meshcompare-grid-render
	PUBLIC ${MESHCOMPARE_SOURCE_ROOT})

add_executable(meshcompare MACOSX_BUNDLE main.cpp)
target_compile_definitions(meshcompare
	PRIVATE MESHCOMPARE_VERSION="${PROJECT_VERSION}")
target_link_libraries(meshcompare PRIVATE
	meshcompare-ui
	meshcompare-core
	meshcompare-analysis
	meshcompare-startup-command-line
	meshcompare-grid-render
	meshcompare-meshlab-infrastructure
	meshcompare-meshlab-renderer
	meshlab-common)
set_target_properties(meshcompare PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${MESHCOMPARE_DISTRIB_DIR}"
	MACOSX_BUNDLE TRUE
	MACOSX_BUNDLE_BUNDLE_NAME "Mesh Compare"
	MACOSX_BUNDLE_GUI_IDENTIFIER "org.vcg.meshcompare"
	MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
	MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
	MACOSX_BUNDLE_INFO_STRING "Mesh Compare ${PROJECT_VERSION}")
if(APPLE)
	set(meshcompare_icon "${PROJECT_SOURCE_DIR}/resources/meshcompare.icns")
	set_source_files_properties(
		${meshcompare_icon}
		PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
	target_sources(meshcompare PRIVATE ${meshcompare_icon})
	set_target_properties(meshcompare PROPERTIES
		MACOSX_BUNDLE_ICON_FILE "meshcompare.icns"
		MACOSX_BUNDLE_INFO_PLIST
			"${PROJECT_SOURCE_DIR}/cmake/MeshCompareInfo.plist.in"
		BUILD_WITH_INSTALL_RPATH TRUE
		INSTALL_RPATH "@executable_path/../Frameworks")
	install(
		TARGETS meshcompare
		BUNDLE DESTINATION .
		COMPONENT MeshCompare)
else()
	install(
		TARGETS meshcompare
		RUNTIME DESTINATION bin
		COMPONENT MeshCompare)
endif()
if(APPLE)
	meshcompare_configure_macos_bundle(meshcompare)
endif()

function(add_meshcompare_test name source)
	add_executable(${name} "${MESHCOMPARE_TEST_ROOT}/${source}")
	target_link_libraries(${name} PRIVATE meshcompare-core Qt5::Test)
	add_test(NAME ${name} COMMAND ${name})
endfunction()

if(BUILD_TESTING)
	add_meshcompare_test(meshcompare-diagnostics-menu-test diagnostics_menu_test.cpp)
	target_link_libraries(meshcompare-diagnostics-menu-test PRIVATE meshcompare-ui)
	add_meshcompare_test(meshcompare-camera-panel-test camera_panel_test.cpp)
	target_link_libraries(meshcompare-camera-panel-test PRIVATE meshcompare-ui)
	add_meshcompare_test(meshcompare-tag-editor-test tag_editor_test.cpp)
	target_link_libraries(meshcompare-tag-editor-test PRIVATE meshcompare-ui)
	add_meshcompare_test(meshcompare-camera-workflow-test camera_workflow_test.cpp)
	target_link_libraries(meshcompare-camera-workflow-test PRIVATE
		meshcompare-ui meshcompare-camera-pose-store
		meshcompare-meshlab-infrastructure meshlab-common)
	add_meshcompare_test(meshcompare-camera-pose-paths-test camera_pose_paths_test.cpp)
	target_link_libraries(meshcompare-camera-pose-paths-test PRIVATE meshcompare-ui)
	add_meshcompare_test(meshcompare-startup-command-line-test startup_command_line_test.cpp)
	target_link_libraries(
		meshcompare-startup-command-line-test
		PRIVATE meshcompare-startup-command-line)
	add_meshcompare_test(meshcompare-grid-renderer-test grid_renderer_test.cpp)
	target_link_libraries(
		meshcompare-grid-renderer-test
		PRIVATE meshcompare-grid-render)
	add_meshcompare_test(meshcompare-camera-pose-store-test camera_pose_store_test.cpp)
	target_link_libraries(meshcompare-camera-pose-store-test PRIVATE meshcompare-camera-pose-store)
	add_meshcompare_test(meshcompare-renderer-adapter-contract-test renderer_adapter_contract_test.cpp)
	add_meshcompare_test(meshcompare-workspace-state-test workspace_state_test.cpp)
	add_meshcompare_test(meshcompare-surface-comparison-test surface_comparison_test.cpp)
	target_link_libraries(meshcompare-surface-comparison-test
		PRIVATE meshcompare-analysis meshlab-common)
	add_meshcompare_test(meshcompare-mesh-color-service-test mesh_color_service_test.cpp)
	target_link_libraries(meshcompare-mesh-color-service-test PRIVATE meshcompare-color-service)
	add_meshcompare_test(meshcompare-standalone-main-window-test standalone_main_window_test.cpp)
	target_link_libraries(meshcompare-standalone-main-window-test PRIVATE meshcompare-ui)
	add_meshcompare_test(meshcompare-coloring-panel-test coloring_panel_test.cpp)
	target_link_libraries(meshcompare-coloring-panel-test PRIVATE meshcompare-ui)
	add_meshcompare_test(meshcompare-workspace-controller-test workspace_controller_test.cpp)
	target_link_libraries(meshcompare-workspace-controller-test
		PRIVATE meshcompare-ui meshcompare-meshlab-infrastructure meshlab-common)
	add_meshcompare_test(meshcompare-workspace-lifecycle-test workspace_lifecycle_test.cpp)
	target_link_libraries(meshcompare-workspace-lifecycle-test
		PRIVATE meshcompare-ui meshcompare-meshlab-infrastructure meshlab-common)
	add_meshcompare_test(meshcompare-mesh-import-service-test mesh_import_service_test.cpp)
	target_link_libraries(meshcompare-mesh-import-service-test
		PRIVATE meshcompare-meshlab-infrastructure meshlab-common)
	add_dependencies(meshcompare-mesh-import-service-test io_base)
	add_meshcompare_test(
		meshcompare-mesh-lab-viewport-dependencies-test
		mesh_lab_viewport_dependencies_test.cpp)
	target_link_libraries(meshcompare-mesh-lab-viewport-dependencies-test
		PRIVATE meshcompare-meshlab-viewport)
	add_meshcompare_test(meshcompare-render-regression-test render_regression_test.cpp)
	target_link_libraries(meshcompare-render-regression-test
		PRIVATE meshcompare-meshlab-viewport)
	add_meshcompare_test(
		meshcompare-mesh-lab-renderer-adapter-test
		mesh_lab_renderer_adapter_test.cpp)
	target_link_libraries(meshcompare-mesh-lab-renderer-adapter-test
		PRIVATE meshcompare-meshlab-renderer)
	add_meshcompare_test(
		meshcompare-mesh-lab-renderer-adapter-offscreen-test
		mesh_lab_renderer_adapter_offscreen_test.cpp)
	target_link_libraries(meshcompare-mesh-lab-renderer-adapter-offscreen-test
		PRIVATE meshcompare-meshlab-renderer)
	add_meshcompare_test(meshcompare-viewport-grid-test viewport_grid_test.cpp)
	target_link_libraries(meshcompare-viewport-grid-test
		PRIVATE meshcompare-meshlab-renderer)
	add_meshcompare_test(meshcompare-renderer-smoke-test renderer_smoke_test.cpp)
	target_link_libraries(meshcompare-renderer-smoke-test PRIVATE
		meshcompare-meshlab-infrastructure
		meshcompare-meshlab-renderer
		meshlab-common)
	add_dependencies(meshcompare-renderer-smoke-test io_base)
	add_meshcompare_test(
		meshcompare-analysis-repaint-repro-test
		analysis_repaint_repro_test.cpp)
	target_link_libraries(meshcompare-analysis-repaint-repro-test PRIVATE
		meshcompare-ui
		meshcompare-analysis
		meshcompare-meshlab-infrastructure
		meshcompare-meshlab-renderer
		meshlab-common)
	add_dependencies(meshcompare-analysis-repaint-repro-test io_base)
	set_tests_properties(
		meshcompare-mesh-lab-renderer-adapter-offscreen-test
		PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
