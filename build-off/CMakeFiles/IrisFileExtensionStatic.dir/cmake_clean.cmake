file(REMOVE_RECURSE
  "libIrisFileExtensionStatic.a"
  "libIrisFileExtensionStatic.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CXX)
  include(CMakeFiles/IrisFileExtensionStatic.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
