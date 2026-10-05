# This file installs the executable and creates release archives.

include_guard(GLOBAL)

include(GNUInstallDirs)

install(TARGETS fram RUNTIME COMPONENT fram_runtime)
install(
  FILES "${PROJECT_SOURCE_DIR}/README.md"
        "${PROJECT_SOURCE_DIR}/LICENSE.txt"
        "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}"
  COMPONENT fram_runtime
)

string(TOLOWER "${CMAKE_SYSTEM_NAME}" fram_target_system)
set(FRAM_RELEASE_TARGET
    "${CMAKE_SYSTEM_PROCESSOR}-${fram_target_system}"
    CACHE STRING "Platform label in the release archive name"
)

set(CPACK_PACKAGE_NAME "fram")
set(CPACK_PACKAGE_VENDOR "Daniel Perez Alvarez")
set(CPACK_PACKAGE_CONTACT "${CPACK_PACKAGE_VENDOR} <daniel@unindented.org>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE.txt")
set(CPACK_RESOURCE_FILE_README "${PROJECT_SOURCE_DIR}/README.md")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "${CPACK_PACKAGE_NAME}")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${FRAM_RELEASE_TARGET}")
set(CPACK_PACKAGE_DIRECTORY "${PROJECT_BINARY_DIR}")
set(CPACK_STRIP_FILES TRUE)
set(CPACK_GENERATOR "TGZ")
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-source")

# CPack includes untracked files when it archives the source tree. Keep the default exclusions. Also
# exclude build products.
set(CPACK_SOURCE_IGNORE_FILES
    "/CVS/"
    "/\\.svn/"
    "/\\.bzr/"
    "/\\.hg/"
    "/\\.git/"
    "\\.swp$"
    "\\.#"
    "/#"
    "^${PROJECT_SOURCE_DIR}/build/"
    "^${PROJECT_SOURCE_DIR}/CMakeUserPresets\\.json$"
)
set(CPACK_VERBATIM_VARIABLES TRUE)

include(CPack)
