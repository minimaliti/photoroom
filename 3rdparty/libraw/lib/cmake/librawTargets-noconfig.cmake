#----------------------------------------------------------------
# Generated CMake target import file.
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "libraw::raw" for configuration ""
set_property(TARGET libraw::raw APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(libraw::raw PROPERTIES
  IMPORTED_IMPLIB_NOCONFIG "${_IMPORT_PREFIX}/lib/libraw.dll.a"
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/bin/libraw.dll"
  )

list(APPEND _cmake_import_check_targets libraw::raw )
list(APPEND _cmake_import_check_files_for_libraw::raw "${_IMPORT_PREFIX}/lib/libraw.dll.a" "${_IMPORT_PREFIX}/bin/libraw.dll" )

# Import target "libraw::raw_r" for configuration ""
set_property(TARGET libraw::raw_r APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(libraw::raw_r PROPERTIES
  IMPORTED_IMPLIB_NOCONFIG "${_IMPORT_PREFIX}/lib/libraw_r.dll.a"
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/bin/libraw_r.dll"
  )

list(APPEND _cmake_import_check_targets libraw::raw_r )
list(APPEND _cmake_import_check_files_for_libraw::raw_r "${_IMPORT_PREFIX}/lib/libraw_r.dll.a" "${_IMPORT_PREFIX}/bin/libraw_r.dll" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
