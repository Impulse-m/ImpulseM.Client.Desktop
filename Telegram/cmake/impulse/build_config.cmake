# ImpulseM build variables. Each one defaults to the environment variable of the
# same name, so CI passes them from the organisation variables and a local build
# may export them or pass -D. The values are compiled only into the generated
# build_config.cpp of the Telegram and Updater targets: no target-wide definitions.

set(IMPULSEM_RPC_URL "$ENV{IMPULSEM_RPC_URL}" CACHE STRING "Base URL of the ImpulseM transport host, e.g. https://rpc.example.org.")
set(IMPULSEM_SITE_URL "$ENV{IMPULSEM_SITE_URL}" CACHE STRING "Base URL of the project website without a trailing slash, e.g. https://example.org.")
set(IMPULSEM_LINK_HOST "$ENV{IMPULSEM_LINK_HOST}" CACHE STRING "Host of public share links without a scheme, e.g. o.example.org.")
set(IMPULSEM_WEB_URL "$ENV{IMPULSEM_WEB_URL}" CACHE STRING "URL of the web client, e.g. https://web.example.org/k.")

# CI builds run with CI=true (GitHub Actions sets it) and must have a transport
# host. A local build only warns: the app then stays in its not-connected state.
if (IMPULSEM_RPC_URL STREQUAL "")
    if (DEFINED ENV{CI} AND NOT "$ENV{CI}" STREQUAL "")
        message(FATAL_ERROR
        " \n"
        " PROVIDE: -D IMPULSEM_RPC_URL=[URL]\n"
        " \n"
        " > CI builds of ImpulseM need the base URL of the transport host.\n"
        " > Set the organisation variable IMPULSEM_RPC_URL and pass it to CMake.\n"
        " ")
    else()
        message(WARNING
        " IMPULSEM_RPC_URL is empty: the client will not connect to a transport host.")
    endif()
endif()

if (IMPULSEM_SITE_URL STREQUAL "")
    message(STATUS "IMPULSEM_SITE_URL is empty: website links are hidden.")
endif()

set(impulse_build_config_src "${CMAKE_CURRENT_BINARY_DIR}/impulse_build_config.cpp")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/SourceFiles/mtproto/impulse/core/build_config.cpp.in"
    "${impulse_build_config_src}"
    @ONLY)
