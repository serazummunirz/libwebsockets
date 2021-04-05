project(lws-minimal-ws-server C)
cmake_minimum_required(VERSION 2.8.12)
find_package(libwebsockets CONFIG REQUIRED)
list(APPEND CMAKE_MODULE_PATH ${LWS_CMAKE_DIR})
include(CheckCSourceCompiles)
include(LwsCheckRequirements)

set(SAMP lws-minimal-ws-server-v4l2)
set(SRCS minimal-ws-server.c v4l2.c annex-b.c boxes.c transcode.c)

set(requirements 1)
require_lws_config(LWS_ROLE_WS 1 requirements)
require_lws_config(LWS_WITH_SERVER 1 requirements)

if (requirements AND ${CMAKE_SYSTEM_NAME} STREQUAL "Linux")

	find_package(PkgConfig REQUIRED)
	pkg_check_modules(PC_FFMPEG REQUIRED IMPORTED_TARGET
				libavcodec libavformat libavutil libswscale)

	find_path(AVFORMAT_INCLUDE_DIR libavformat/avformat.h
		  HINTS ${PC_FFMPEG_LIBAVFORMAT_INCLUDEDIR}
		  	${PC_FFMPEG_INCLUDE_DIRS})

	find_library(AVFORMAT_LIBRARY NAMES avformat
		  HINTS ${PC_FFMPEG_LIBAVFORMAT_LIBDIR}
		  	${PC_FFMPEG_LIBRARY_DIRS})
	 
	find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h
		  HINTS ${PC_FFMPEG_LIBAVCODEC_INCLUDEDIR}
		  	${PC_FFMPEG_INCLUDE_DIRS})
	find_library(AVCODEC_LIBRARY NAMES avcodec
		  HINTS ${PC_FFMPEG_LIBAVCODEC_LIBDIR}
		  	${PC_FFMPEG_LIBRARY_DIRS})
	 
	find_path(AVUTIL_INCLUDE_DIR libavutil/avutil.h
		  HINTS ${PC_FFMPEG_LIBAVUTIL_INCLUDEDIR}
		  	${PC_FFMPEG_INCLUDE_DIRS})
	find_library(AVUTIL_LIBRARY NAMES avutil
		  HINTS ${PC_FFMPEG_LIBAVUTIL_LIBDIR}
		  	${PC_FFMPEG_LIBRARY_DIRS})
	 
	find_path(SWSCALE_INCLUDE_DIR libswscale/swscale.h
		  HINTS ${PC_FFMPEG_LIBSWSCALE_INCLUDEDIR}
		  	${PC_FFMPEG_INCLUDE_DIRS})
	find_library(SWSCALE_LIBRARY NAMES swscale
		  HINTS ${PC_FFMPEG_LIBSWSCALE_LIBDIR}
		  	${PC_FFMPEG_LIBRARY_DIRS})

	add_executable(${SAMP} ${SRCS})
	target_include_directories(${SAMP} PUBLIC
				   ${AVFORMAT_INCLUDE_DIR}
				   ${AVCODEC_INCLUDE_DIR}
				   ${AVUTIL_INCLUDE_DIR}
				   ${SWSCALE_INCLUDE_DIR})

	# ffmpeg headers are not clean for these themselves
	target_compile_options(${SAMP} PUBLIC -Wno-conversion -Wno-sign-conversion)

	if (websockets_shared)
		target_link_libraries(${SAMP} websockets_shared
					${LIBWEBSOCKETS_DEP_LIBS}
					${AVFORMAT_LIBRARY}
					${AVCODEC_LIBRARY}
					${SWSCALE_LIBRARY}
					${AVUTIL_LIBRARY})
		add_dependencies(${SAMP} websockets_shared)
	else()
		target_link_libraries(${SAMP} websockets
					${LIBWEBSOCKETS_DEP_LIBS}
					${AVFORMAT_LIBRARY}
					${AVCODEC_LIBRARY}
					${SWSCALE_LIBRARY}
					${AVUTIL_LIBRARY})
	endif()
endif()
