if(NOT DEFINED BINARY_PATH OR NOT DEFINED RELATIVE_RPATH)
	message(FATAL_ERROR "BINARY_PATH and RELATIVE_RPATH are required.")
endif()

if(NOT EXISTS "${BINARY_PATH}")
	message(FATAL_ERROR "Cannot sanitize missing binary: ${BINARY_PATH}")
endif()

find_program(OTOOL_EXECUTABLE otool)
find_program(INSTALL_NAME_TOOL_EXECUTABLE install_name_tool)
if(NOT OTOOL_EXECUTABLE OR NOT INSTALL_NAME_TOOL_EXECUTABLE)
	message(FATAL_ERROR "otool and install_name_tool are required to sanitize macOS RPATHs.")
endif()

function(read_rpaths output_variable)
	execute_process(
		COMMAND "${OTOOL_EXECUTABLE}" -l "${BINARY_PATH}"
		RESULT_VARIABLE otool_result
		OUTPUT_VARIABLE load_commands
		ERROR_VARIABLE otool_error)
	if(NOT otool_result EQUAL 0)
		message(FATAL_ERROR
			"Could not inspect RPATHs in ${BINARY_PATH}: ${otool_error}")
	endif()

	string(REGEX MATCHALL
		"[ \t]*path [^\n]* \\(offset [0-9]+\\)"
		rpath_lines
		"${load_commands}")
	set(rpaths)
	foreach(rpath_line IN LISTS rpath_lines)
		string(REGEX REPLACE
			"^[ \t]*path (.*) \\(offset [0-9]+\\)$"
			"\\1"
			rpath
			"${rpath_line}")
		list(APPEND rpaths "${rpath}")
	endforeach()
	set(${output_variable} "${rpaths}" PARENT_SCOPE)
endfunction()

read_rpaths(existing_rpaths)
foreach(rpath IN LISTS existing_rpaths)
	if(IS_ABSOLUTE "${rpath}")
		execute_process(
			COMMAND "${INSTALL_NAME_TOOL_EXECUTABLE}"
				-delete_rpath "${rpath}" "${BINARY_PATH}"
			RESULT_VARIABLE delete_result
			ERROR_VARIABLE delete_error)
		if(NOT delete_result EQUAL 0)
			message(FATAL_ERROR
				"Could not remove absolute RPATH ${rpath} from ${BINARY_PATH}: ${delete_error}")
		endif()
	endif()
endforeach()

read_rpaths(sanitized_rpaths)
if(NOT RELATIVE_RPATH IN_LIST sanitized_rpaths)
	execute_process(
		COMMAND "${INSTALL_NAME_TOOL_EXECUTABLE}"
			-add_rpath "${RELATIVE_RPATH}" "${BINARY_PATH}"
		RESULT_VARIABLE add_result
		ERROR_VARIABLE add_error)
	if(NOT add_result EQUAL 0)
		message(FATAL_ERROR
			"Could not add RPATH ${RELATIVE_RPATH} to ${BINARY_PATH}: ${add_error}")
	endif()
endif()

read_rpaths(final_rpaths)
foreach(rpath IN LISTS final_rpaths)
	if(IS_ABSOLUTE "${rpath}")
		message(FATAL_ERROR
			"Absolute RPATH remains in ${BINARY_PATH}: ${rpath}")
	endif()
endforeach()
