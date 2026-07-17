if(NOT DEFINED BUNDLE_PATH)
	message(FATAL_ERROR "BUNDLE_PATH is required.")
endif()

find_program(FILE_EXECUTABLE file REQUIRED)
find_program(OTOOL_EXECUTABLE otool REQUIRED)
find_program(INSTALL_NAME_TOOL_EXECUTABLE install_name_tool REQUIRED)
file(GLOB_RECURSE bundle_files
	LIST_DIRECTORIES false
	"${BUNDLE_PATH}/Contents/*")
foreach(bundle_file IN LISTS bundle_files)
	execute_process(
		COMMAND "${FILE_EXECUTABLE}" "${bundle_file}"
		OUTPUT_VARIABLE file_description)
	if(NOT file_description MATCHES "Mach-O")
		continue()
	endif()
	execute_process(
		COMMAND "${OTOOL_EXECUTABLE}" -l "${bundle_file}"
		OUTPUT_VARIABLE load_commands
		RESULT_VARIABLE inspect_result)
	if(NOT inspect_result EQUAL 0)
		message(FATAL_ERROR "Could not inspect ${bundle_file}.")
	endif()
	string(REGEX MATCHALL
		"[ \t]*path /[^\\n]* \\(offset [0-9]+\\)"
		rpath_lines
		"${load_commands}")
	foreach(rpath_line IN LISTS rpath_lines)
		string(REGEX REPLACE
			"^[ \t]*path (.*) \\(offset [0-9]+\\)$"
			"\\1"
			rpath
			"${rpath_line}")
		execute_process(
			COMMAND "${INSTALL_NAME_TOOL_EXECUTABLE}"
				-delete_rpath "${rpath}" "${bundle_file}"
			RESULT_VARIABLE delete_result
			ERROR_VARIABLE delete_error)
		if(NOT delete_result EQUAL 0)
			message(FATAL_ERROR
				"Could not remove ${rpath} from ${bundle_file}: ${delete_error}")
		endif()
	endforeach()
endforeach()
