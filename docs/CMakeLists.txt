###########################################################################
# Copyright (C) 2015-2025 IoT.bzh Company
###########################################################################

find_program(SCDOC scdoc)
find_program(GZIP gzip)
set(MANUALS "")
function(add_scdoc_manual section scd_file)
	if(SCDOC)
		set(INSCD  "${CMAKE_CURRENT_SOURCE_DIR}/${scd_file}.${section}.scd")
		if(GZIP)
			set(OUTMAN "${CMAKE_CURRENT_BINARY_DIR}/${scd_file}.${section}.gz")
			add_custom_command(
				OUTPUT ${OUTMAN}
				COMMAND echo "generation of ${scd_file}.${section}.gz"
				COMMAND ${SCDOC} < ${INSCD} | ${GZIP} > ${OUTMAN}
				DEPENDS ${INSCD})
		else(GZIP)
			set(OUTMAN "${CMAKE_CURRENT_BINARY_DIR}/${scd_file}.${section}")
			add_custom_command(
				OUTPUT ${OUTMAN}
				COMMAND echo "generation of ${scd_file}.${section}"
				COMMAND ${SCDOC} < ${INSCD} > ${OUTMAN}
				DEPENDS ${INSCD})
		endif(GZIP)
		install(FILES ${OUTMAN} DESTINATION ${CMAKE_INSTALL_FULL_MANDIR}/man${section})
		list(APPEND MANUALS ${OUTMAN})
		set(MANUALS ${MANUALS} PARENT_SCOPE)
	endif(SCDOC)
endfunction()
add_scdoc_manual(1 afb-client)
if(MANUALS)
	add_custom_target(manuals ALL DEPENDS ${MANUALS})
endif(MANUALS)

