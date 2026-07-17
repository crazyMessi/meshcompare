function(meshcompare_configure_macos_bundle target_name)
	if(NOT APPLE)
		return()
	endif()

	find_program(CODESIGN_EXECUTABLE codesign REQUIRED)
	if(MESHCOMPARE_DEPLOY_QT)
		find_program(
			MACDEPLOYQT_EXECUTABLE
			macdeployqt
			HINTS
				"/opt/homebrew/opt/qt@5/bin"
				"/usr/local/opt/qt@5/bin"
			REQUIRED)
	endif()

	get_target_property(meshcompare_glew_target_type external-glew TYPE)
	set(bundle_dependencies io_base meshlab-common)
	set(bundle_commands
		COMMAND "${CMAKE_COMMAND}" -E make_directory
			"$<TARGET_BUNDLE_DIR:${target_name}>/Contents/PlugIns"
		COMMAND "${CMAKE_COMMAND}" -E make_directory
			"$<TARGET_BUNDLE_DIR:${target_name}>/Contents/Frameworks"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"$<TARGET_FILE:io_base>"
			"$<TARGET_BUNDLE_DIR:${target_name}>/Contents/PlugIns/$<TARGET_FILE_NAME:io_base>"
		COMMAND "${CMAKE_COMMAND}"
			"-DBINARY_PATH=$<TARGET_BUNDLE_DIR:${target_name}>/Contents/PlugIns/$<TARGET_FILE_NAME:io_base>"
			"-DRELATIVE_RPATH=@loader_path/../Frameworks"
			-P "${PROJECT_SOURCE_DIR}/cmake/sanitize_macos_rpaths.cmake"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"$<TARGET_FILE:meshlab-common>"
			"$<TARGET_BUNDLE_DIR:${target_name}>/Contents/Frameworks/$<TARGET_FILE_NAME:meshlab-common>"
		COMMAND "${CMAKE_COMMAND}"
			"-DBINARY_PATH=$<TARGET_BUNDLE_DIR:${target_name}>/Contents/Frameworks/$<TARGET_FILE_NAME:meshlab-common>"
			"-DRELATIVE_RPATH=@loader_path"
			-P "${PROJECT_SOURCE_DIR}/cmake/sanitize_macos_rpaths.cmake"
		COMMAND "${CMAKE_COMMAND}"
			"-DBINARY_PATH=$<TARGET_FILE:${target_name}>"
			"-DRELATIVE_RPATH=@executable_path/../Frameworks"
			-P "${PROJECT_SOURCE_DIR}/cmake/sanitize_macos_rpaths.cmake")

	if(NOT meshcompare_glew_target_type STREQUAL "INTERFACE_LIBRARY")
		list(APPEND bundle_dependencies external-glew)
		list(APPEND bundle_commands
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"$<TARGET_FILE:external-glew>"
				"$<TARGET_BUNDLE_DIR:${target_name}>/Contents/Frameworks/$<TARGET_FILE_NAME:external-glew>"
			COMMAND "${CMAKE_COMMAND}"
				"-DBINARY_PATH=$<TARGET_BUNDLE_DIR:${target_name}>/Contents/Frameworks/$<TARGET_FILE_NAME:external-glew>"
				"-DRELATIVE_RPATH=@loader_path"
				-P "${PROJECT_SOURCE_DIR}/cmake/sanitize_macos_rpaths.cmake")
	endif()

	if(MESHCOMPARE_DEPLOY_QT)
		list(APPEND bundle_commands
			COMMAND "${MACDEPLOYQT_EXECUTABLE}"
				"$<TARGET_BUNDLE_DIR:${target_name}>"
				-always-overwrite
				-no-strip
				"-executable=$<TARGET_BUNDLE_DIR:${target_name}>/Contents/PlugIns/$<TARGET_FILE_NAME:io_base>"
			COMMAND "${CMAKE_COMMAND}"
				"-DBUNDLE_PATH=$<TARGET_BUNDLE_DIR:${target_name}>"
				-P "${PROJECT_SOURCE_DIR}/cmake/sanitize_macos_bundle.cmake"
			COMMAND "${CMAKE_COMMAND}"
				"-DBUNDLE_PATH=$<TARGET_BUNDLE_DIR:${target_name}>"
				"-DIO_PLUGIN_NAME=$<TARGET_FILE_NAME:io_base>"
				-DREQUIRE_DEPLOYED_QT=ON
				-P "${PROJECT_SOURCE_DIR}/cmake/verify_macos_bundle.cmake")
	else()
		list(APPEND bundle_commands
			COMMAND "${CMAKE_COMMAND}"
				"-DBUNDLE_PATH=$<TARGET_BUNDLE_DIR:${target_name}>"
				"-DIO_PLUGIN_NAME=$<TARGET_FILE_NAME:io_base>"
				-DREQUIRE_DEPLOYED_QT=OFF
				-P "${PROJECT_SOURCE_DIR}/cmake/verify_macos_bundle.cmake")
	endif()

	if(MESHCOMPARE_ADHOC_SIGN)
		list(APPEND bundle_commands
			COMMAND "${CODESIGN_EXECUTABLE}"
				--force --deep --sign -
				"$<TARGET_BUNDLE_DIR:${target_name}>"
			COMMAND "${CODESIGN_EXECUTABLE}"
				--verify --deep --strict
				"$<TARGET_BUNDLE_DIR:${target_name}>")
	endif()

	add_dependencies(${target_name} ${bundle_dependencies})
	add_custom_command(
		TARGET ${target_name}
		POST_BUILD
		${bundle_commands}
		COMMENT "Preparing the self-contained Mesh Compare application"
		COMMAND_EXPAND_LISTS
		VERBATIM)
endfunction()
