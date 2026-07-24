if(NOT DEFINED BUNDLE_PATH OR NOT DEFINED IO_PLUGIN_NAME
		OR NOT DEFINED REQUIRE_DEPLOYED_QT)
	message(FATAL_ERROR
		"BUNDLE_PATH, IO_PLUGIN_NAME, and REQUIRE_DEPLOYED_QT are required.")
endif()

set(required_paths
	"${BUNDLE_PATH}/Contents/Info.plist"
	"${BUNDLE_PATH}/Contents/MacOS/meshcompare"
	"${BUNDLE_PATH}/Contents/Resources/meshcompare.icns"
	"${BUNDLE_PATH}/Contents/Frameworks/libmeshlab-common.dylib"
	"${BUNDLE_PATH}/Contents/PlugIns/${IO_PLUGIN_NAME}")
if(REQUIRE_DEPLOYED_QT)
	list(APPEND required_paths
		"${BUNDLE_PATH}/Contents/Frameworks/QtCore.framework/Versions/5/QtCore"
		"${BUNDLE_PATH}/Contents/PlugIns/platforms/libqcocoa.dylib")
endif()
foreach(required_path IN LISTS required_paths)
	if(NOT EXISTS "${required_path}")
		message(FATAL_ERROR
			"Self-contained bundle is missing ${required_path}.")
	endif()
endforeach()

file(GLOB top_level_plugins
	LIST_DIRECTORIES false
	"${BUNDLE_PATH}/Contents/PlugIns/*")
foreach(plugin_path IN LISTS top_level_plugins)
	get_filename_component(plugin_name "${plugin_path}" NAME)
	if(NOT plugin_name STREQUAL IO_PLUGIN_NAME)
		message(FATAL_ERROR
			"Unexpected top-level MeshLab plugin in bundle: ${plugin_name}")
	endif()
endforeach()

find_program(FILE_EXECUTABLE file REQUIRED)
find_program(OTOOL_EXECUTABLE otool REQUIRED)
file(GLOB_RECURSE bundle_files
	LIST_DIRECTORIES false
	"${BUNDLE_PATH}/Contents/*")

# Reject the precise dyld failure mode that macdeployqt can otherwise leave
# behind: a framework references a bundle-local dylib through @loader_path or
# @executable_path, but the dylib was not copied into Contents/Frameworks.
set(bundle_executable_directory "${BUNDLE_PATH}/Contents/MacOS")
foreach(bundle_file IN LISTS bundle_files)
	execute_process(
		COMMAND "${FILE_EXECUTABLE}" "${bundle_file}"
		RESULT_VARIABLE file_result
		OUTPUT_VARIABLE file_description
		ERROR_VARIABLE file_error)
	if(NOT file_result EQUAL 0)
		message(FATAL_ERROR
			"Could not inspect ${bundle_file}: ${file_error}")
	endif()
	if(NOT file_description MATCHES "Mach-O")
		continue()
	endif()

	execute_process(
		COMMAND "${OTOOL_EXECUTABLE}" -L "${bundle_file}"
		RESULT_VARIABLE dependency_result
		OUTPUT_VARIABLE dependencies
		ERROR_VARIABLE dependency_error)
	if(NOT dependency_result EQUAL 0)
		message(FATAL_ERROR
			"Could not inspect dependencies for ${bundle_file}: ${dependency_error}")
	endif()
	get_filename_component(bundle_file_realpath "${bundle_file}" REALPATH)
	get_filename_component(bundle_file_directory "${bundle_file_realpath}" DIRECTORY)
	string(REPLACE "\n" ";" dependency_lines "${dependencies}")
	foreach(dependency_line IN LISTS dependency_lines)
		string(
			REGEX MATCH
			"^[ \t]*([^ \t]+) \\(compatibility version"
			dependency_match
			"${dependency_line}")
		if(NOT dependency_match)
			continue()
		endif()
		set(dependency_path "${CMAKE_MATCH_1}")
		if(dependency_path MATCHES "^@loader_path/")
			string(
				REPLACE "@loader_path" "${bundle_file_directory}"
				resolved_dependency_path "${dependency_path}")
		elseif(dependency_path MATCHES "^@executable_path/")
			string(
				REPLACE "@executable_path" "${bundle_executable_directory}"
				resolved_dependency_path "${dependency_path}")
		else()
			continue()
		endif()
		if(NOT EXISTS "${resolved_dependency_path}")
			message(FATAL_ERROR
				"Bundle dependency is missing: ${dependency_path} required by "
				"${bundle_file}; resolved to ${resolved_dependency_path}.")
		endif()
	endforeach()
endforeach()

foreach(bundle_file IN LISTS bundle_files)
	execute_process(
		COMMAND "${FILE_EXECUTABLE}" "${bundle_file}"
		RESULT_VARIABLE file_result
		OUTPUT_VARIABLE file_description
		ERROR_VARIABLE file_error)
	if(NOT file_result EQUAL 0)
		message(FATAL_ERROR
			"Could not inspect ${bundle_file}: ${file_error}")
	endif()
	if(NOT file_description MATCHES "Mach-O")
		continue()
	endif()

	execute_process(
		COMMAND "${OTOOL_EXECUTABLE}" -L "${bundle_file}"
		RESULT_VARIABLE dependency_result
		OUTPUT_VARIABLE dependencies
		ERROR_VARIABLE dependency_error)
	if(NOT dependency_result EQUAL 0)
		message(FATAL_ERROR
			"Could not inspect dependencies for ${bundle_file}: ${dependency_error}")
	endif()
	execute_process(
		COMMAND "${OTOOL_EXECUTABLE}" -D "${bundle_file}"
		OUTPUT_VARIABLE install_name)
	string(REGEX MATCHALL
		"(/opt/homebrew|/usr/local|/Users/)[^ \t\n]*"
		nonportable_paths
		"${dependencies}")
	foreach(nonportable_path IN LISTS nonportable_paths)
		string(FIND "${install_name}" "${nonportable_path}" is_own_install_name)
		if(is_own_install_name EQUAL -1)
			message(FATAL_ERROR
				"Non-portable dependency remains in ${bundle_file}: "
				"${nonportable_path}")
		endif()
	endforeach()

	execute_process(
		COMMAND "${OTOOL_EXECUTABLE}" -l "${bundle_file}"
		RESULT_VARIABLE load_command_result
		OUTPUT_VARIABLE load_commands
		ERROR_VARIABLE load_command_error)
	if(NOT load_command_result EQUAL 0)
		message(FATAL_ERROR
			"Could not inspect load commands for ${bundle_file}: ${load_command_error}")
	endif()
	string(REGEX MATCHALL
		"[ \t]*path /[^\\n]* \\(offset [0-9]+\\)"
		absolute_rpaths
		"${load_commands}")
	if(absolute_rpaths)
		message(FATAL_ERROR
			"Absolute RPATH remains in ${bundle_file}: ${absolute_rpaths}")
	endif()
endforeach()
