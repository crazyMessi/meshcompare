# Copyright 2019, 2020, Collabora, Ltd.
# Copyright 2019, 2020, Visual Computing Lab, ISTI - Italian National Research Council
# SPDX-License-Identifier: BSL-1.0

option(MESHLAB_ALLOW_BUNDLED_SOURCE_EASYEXIF "Allow use bundled source of EasyExif" ON)

if(MESHLAB_ALLOW_BUNDLED_SOURCE_EASYEXIF)
	set(EASYEXIF_DIR "${MESHLAB_EXTERNAL_DIR}/easyexif-1.0")
	set(EASYEXIF_CHECK "${EASYEXIF_DIR}/exif.h")

	if (NOT EXISTS ${EASYEXIF_CHECK})
		message(FATAL_ERROR
			"Vendored EasyExif source is missing: ${EASYEXIF_CHECK}")
	endif()

	if (EXISTS ${EASYEXIF_CHECK})
		message(STATUS "- EasyExif - using bundled source")
		add_library(external-easyexif STATIC ${EASYEXIF_DIR}/exif.h ${EASYEXIF_DIR}/exif.cpp)
		target_include_directories(external-easyexif PUBLIC ${EASYEXIF_DIR})
	endif()
else()
	message(
		FATAL_ERROR
			"EeasyExif is required - MESHLAB_ALLOW_BUNDLED_SOURCE_EASYEXIF must be ON.")
endif()
