#----------------------------------------------------------------
# Generated CMake target import file.
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "IrisFileExtension" for configuration ""
set_property(TARGET IrisFileExtension APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(IrisFileExtension PROPERTIES
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libIrisFileExtension.so"
  IMPORTED_SONAME_NOCONFIG "libIrisFileExtension.so"
  )

list(APPEND _cmake_import_check_targets IrisFileExtension )
list(APPEND _cmake_import_check_files_for_IrisFileExtension "${_IMPORT_PREFIX}/lib/libIrisFileExtension.so" )

# Import target "IrisFileExtensionStatic" for configuration ""
set_property(TARGET IrisFileExtensionStatic APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(IrisFileExtensionStatic PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_NOCONFIG "CXX"
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libIrisFileExtensionStatic.a"
  )

list(APPEND _cmake_import_check_targets IrisFileExtensionStatic )
list(APPEND _cmake_import_check_files_for_IrisFileExtensionStatic "${_IMPORT_PREFIX}/lib/libIrisFileExtensionStatic.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
