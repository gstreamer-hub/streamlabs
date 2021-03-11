cmake_minimum_required(VERSION 3.8)
project(streamlabs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTLIBS REQUIRED
        gobject-2.0
        glib-2.0
        gstreamer-1.0)

set(CMAKE_C_STANDARD 99)

include_directories(
  ${GSTLIBS_INCLUDE_DIRS}
  ${GSTLIBS_gstreamer-1.0_INCLUDEDIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
)

set(SOURCE_FILES streamlabs.c)

link_directories(${GSTLIBS_LIBRARY_DIRS})

add_executable(streamlabs ${SOURCE_FILES})

target_link_libraries(streamlabs ${GSTLIBS_LIBRARIES})
