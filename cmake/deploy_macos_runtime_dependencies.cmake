if(NOT DEFINED BUNDLE_PATH OR NOT DEFINED LIBRARY_SEARCH_PATHS)
	message(FATAL_ERROR
		"BUNDLE_PATH and LIBRARY_SEARCH_PATHS are required.")
endif()

set(frameworks_directory "${BUNDLE_PATH}/Contents/Frameworks")
set(executable_directory "${BUNDLE_PATH}/Contents/MacOS")
if(NOT IS_DIRECTORY "${frameworks_directory}")
	message(FATAL_ERROR
		"Bundle frameworks directory does not exist: ${frameworks_directory}")
endif()

string(REPLACE "|" ";" library_search_paths "${LIBRARY_SEARCH_PATHS}")
find_program(FILE_EXECUTABLE file REQUIRED)
find_program(OTOOL_EXECUTABLE otool REQUIRED)
find_program(INSTALL_NAME_TOOL_EXECUTABLE install_name_tool REQUIRED)
find_program(CHMOD_EXECUTABLE chmod REQUIRED)

function(is_mach_o file_path result_variable)
	execute_process(
		COMMAND "${FILE_EXECUTABLE}" "${file_path}"
		RESULT_VARIABLE file_result
		OUTPUT_VARIABLE file_description
		ERROR_VARIABLE file_error)
	if(NOT file_result EQUAL 0)
		message(FATAL_ERROR
			"Could not inspect ${file_path}: ${file_error}")
	endif()
	if(file_description MATCHES "Mach-O")
		set(${result_variable} ON PARENT_SCOPE)
	else()
		set(${result_variable} OFF PARENT_SCOPE)
	endif()
endfunction()

function(copy_missing_relative_dependencies binary_path copied_variable)
	get_filename_component(binary_realpath "${binary_path}" REALPATH)
	get_filename_component(binary_directory "${binary_realpath}" DIRECTORY)
	execute_process(
		COMMAND "${OTOOL_EXECUTABLE}" -L "${binary_path}"
		RESULT_VARIABLE dependency_result
		OUTPUT_VARIABLE dependencies
		ERROR_VARIABLE dependency_error)
	if(NOT dependency_result EQUAL 0)
		message(FATAL_ERROR
			"Could not inspect dependencies for ${binary_path}: ${dependency_error}")
	endif()

	string(REPLACE "\n" ";" dependency_lines "${dependencies}")
	set(first_dependency ON)
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
		if(first_dependency)
			set(first_dependency OFF)
			continue()
		endif()
		set(rewrite_reference OFF)

		if(dependency_path MATCHES "^@loader_path/")
			string(
				REPLACE "@loader_path" "${binary_directory}"
				resolved_path "${dependency_path}")
		elseif(dependency_path MATCHES "^@executable_path/")
			string(
				REPLACE "@executable_path" "${executable_directory}"
				resolved_path "${dependency_path}")
		elseif(dependency_path MATCHES "^(/opt/homebrew|/usr/local)/")
			set(resolved_path "${dependency_path}")
			set(rewrite_reference ON)
		else()
			continue()
		endif()

		if(EXISTS "${resolved_path}" AND NOT rewrite_reference)
			continue()
		endif()

		get_filename_component(dependency_name "${dependency_path}" NAME)
		if(rewrite_reference AND EXISTS "${resolved_path}")
			set(source_library "${resolved_path}")
		else()
			unset(source_library CACHE)
			unset(source_library)
			find_file(
				source_library
				NAMES "${dependency_name}"
				PATHS ${library_search_paths}
				NO_DEFAULT_PATH)
		endif()
		if(NOT source_library)
			message(FATAL_ERROR
				"Bundle dependency is missing: ${dependency_path} required by "
				"${binary_path}; no ${dependency_name} was found in "
				"${library_search_paths}.")
		endif()

		execute_process(
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${source_library}"
				"${frameworks_directory}/${dependency_name}"
			RESULT_VARIABLE copy_result
			ERROR_VARIABLE copy_error)
		if(NOT copy_result EQUAL 0)
			message(FATAL_ERROR
				"Could not copy ${source_library} into the bundle: ${copy_error}")
		endif()
		if(rewrite_reference)
			file(
				RELATIVE_PATH framework_relative_path
				"${binary_directory}" "${frameworks_directory}")
			if(framework_relative_path STREQUAL ".")
				set(bundle_reference "@loader_path/${dependency_name}")
			else()
				set(bundle_reference
					"@loader_path/${framework_relative_path}/${dependency_name}")
			endif()
			execute_process(
				COMMAND "${CHMOD_EXECUTABLE}" u+w
					"${binary_realpath}" "${binary_directory}"
				RESULT_VARIABLE chmod_result
				ERROR_VARIABLE chmod_error)
			if(NOT chmod_result EQUAL 0)
				message(FATAL_ERROR
					"Could not make ${binary_path} writable: ${chmod_error}")
			endif()
			execute_process(
				COMMAND "${INSTALL_NAME_TOOL_EXECUTABLE}"
					-change "${dependency_path}" "${bundle_reference}"
					"${binary_realpath}"
				RESULT_VARIABLE rewrite_result
				ERROR_VARIABLE rewrite_error)
			if(NOT rewrite_result EQUAL 0)
				message(FATAL_ERROR
					"Could not rewrite ${dependency_path} in ${binary_path}: "
					"${rewrite_error}")
			endif()
		endif()
		set(${copied_variable} ON PARENT_SCOPE)
	endforeach()
endfunction()

# macdeployqt deploys the Qt frameworks but can leave their non-Qt Homebrew
# dependencies missing or referenced through an absolute install name. New
# copies are scanned in the next pass, which closes the dependency graph.
foreach(pass RANGE 1 32)
	set(copied_dependency OFF)
	set(processed_realpaths)
	file(GLOB_RECURSE bundle_files
		LIST_DIRECTORIES false
		"${BUNDLE_PATH}/Contents/*")
	foreach(bundle_file IN LISTS bundle_files)
		get_filename_component(bundle_realpath "${bundle_file}" REALPATH)
		if(bundle_realpath IN_LIST processed_realpaths)
			continue()
		endif()
		list(APPEND processed_realpaths "${bundle_realpath}")
		is_mach_o("${bundle_realpath}" is_mach_o_file)
		if(NOT is_mach_o_file)
			continue()
		endif()
		copy_missing_relative_dependencies(
			"${bundle_realpath}" copied_dependency)
	endforeach()
	if(NOT copied_dependency)
		return()
	endif()
endforeach()

message(FATAL_ERROR
	"Exceeded the bundle runtime-dependency copy pass limit.")
