#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "LiveKit::livekit" for configuration "Release"
set_property(TARGET LiveKit::livekit APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(LiveKit::livekit PROPERTIES
  IMPORTED_LINK_DEPENDENT_LIBRARIES_RELEASE "livekit_ffi"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/liblivekit.dylib"
  IMPORTED_SONAME_RELEASE "@rpath/liblivekit.dylib"
  )

list(APPEND _cmake_import_check_targets LiveKit::livekit )
list(APPEND _cmake_import_check_files_for_LiveKit::livekit "${_IMPORT_PREFIX}/lib/liblivekit.dylib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
