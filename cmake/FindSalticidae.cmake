# FindSalticidae.cmake
# Locate the Salticidae library and headers.
# Once done, defines:
#   SALTICIDAE_FOUND
#   SALTICIDAE_INCLUDE_DIRS
#   SALTICIDAE_LIBRARIES

find_path(SALTICIDAE_INCLUDE_DIR
  NAMES salticidae/conn.h
  PATHS
    /usr/local/include
    /usr/include
)

find_library(SALTICIDAE_LIBRARY
  NAMES salticidae
  PATHS
    /usr/local/lib
    /usr/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Salticidae
  REQUIRED_VARS SALTICIDAE_LIBRARY SALTICIDAE_INCLUDE_DIR
  FAIL_MESSAGE "Could not find Salticidae library"
)

if(SALTICIDAE_FOUND)
  set(SALTICIDAE_LIBRARIES ${SALTICIDAE_LIBRARY})
  set(SALTICIDAE_INCLUDE_DIRS ${SALTICIDAE_INCLUDE_DIR})
endif()

mark_as_advanced(SALTICIDAE_LIBRARY SALTICIDAE_INCLUDE_DIR)
