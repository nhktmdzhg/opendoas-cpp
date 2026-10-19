# The doas applet: doas.conf parser (bison), rule matching, environment
# construction, authentication, timestamps and the exec chain.
find_package(BISON 3.8 REQUIRED)
bison_target(
    doas_parser
    parse.y
    ${CMAKE_CURRENT_BINARY_DIR}/parse.cpp
    COMPILE_FLAGS -Wno-empty-rule
)
# The generated parser is C++ code; bison's stack bookkeeping is fine with the
# C++ skeleton, but clang warns about one bookkeeping counter it keeps.
set_source_files_properties(
    ${BISON_doas_parser_OUTPUTS}
    PROPERTIES
        LANGUAGE CXX
        COMPILE_OPTIONS "-Wno-unused-but-set-variable;-Wno-conversion;-Wno-sign-conversion;-Wno-shorten-64-to-32"
)

add_library(
    doas_core
    STATIC
    auth.cpp
    authorize.cpp
    config.cpp
    doas.cpp
    env.cpp
    exec.cpp
    timestamp.cpp
    ${BISON_doas_parser_OUTPUTS}
)
target_include_directories(doas_core PUBLIC ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(doas_core PUBLIC sys_core)
doas_target_defaults(doas_core)
target_compile_definitions(
    doas_core
    PRIVATE
        DOAS_CONF="${DOAS_SYSCONFDIR}/doas.conf"
        DOAS_UID_MAX=${DOAS_UID_MAX}
        DOAS_GID_MAX=${DOAS_GID_MAX}
)
if(DOAS_PERSIST)
    target_compile_definitions(doas_core PRIVATE DOAS_PERSIST DOAS_PERSIST_DIR="${DOAS_PERSIST_DIR}")
endif()

# Authentication provider (the equivalent of configure's authmethod()).
if(DOAS_AUTH_PROVIDER STREQUAL "PAM")
    find_path(DOAS_PAM_INCLUDE_DIR security/pam_appl.h)
    find_library(DOAS_PAM_LIBRARY pam)
    if(NOT DOAS_PAM_INCLUDE_DIR OR NOT DOAS_PAM_LIBRARY)
        message(FATAL_ERROR "PAM provider requested but <security/pam_appl.h> or libpam was not found")
    endif()
    target_sources(doas_core PRIVATE pam.cpp)
    target_compile_definitions(doas_core PRIVATE DOAS_HAVE_PAM)
    target_include_directories(doas_core PRIVATE ${DOAS_PAM_INCLUDE_DIR})
    target_link_libraries(doas_core PUBLIC ${DOAS_PAM_LIBRARY})
elseif(DOAS_AUTH_PROVIDER STREQUAL "SHADOW")
    find_path(DOAS_SHADOW_INCLUDE_DIR shadow.h)
    find_path(DOAS_CRYPT_INCLUDE_DIR crypt.h)
    find_library(DOAS_CRYPT_LIBRARY crypt)
    if(NOT DOAS_SHADOW_INCLUDE_DIR OR NOT DOAS_CRYPT_INCLUDE_DIR OR NOT DOAS_CRYPT_LIBRARY)
        message(FATAL_ERROR "SHADOW provider requested but <shadow.h>/<crypt.h> or libcrypt was not found")
    endif()
    target_sources(doas_core PRIVATE shadow.cpp)
    target_compile_definitions(doas_core PRIVATE DOAS_HAVE_SHADOW)
    target_link_libraries(doas_core PUBLIC ${DOAS_CRYPT_LIBRARY})
else()
    message(FATAL_ERROR "Unknown DOAS_AUTH_PROVIDER '${DOAS_AUTH_PROVIDER}'")
endif()
