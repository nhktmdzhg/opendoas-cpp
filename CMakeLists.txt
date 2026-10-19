cmake_minimum_required(VERSION 3.22)
project(doas_common LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
enable_testing()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options (replace the ./configure flags)
# ---------------------------------------------------------------------------
set(DOAS_AUTH_PROVIDER "PAM" CACHE STRING "Authentication provider (PAM or SHADOW)")
set_property(CACHE DOAS_AUTH_PROVIDER PROPERTY STRINGS PAM SHADOW)
option(DOAS_PERSIST "Enable timestamp/persist support" OFF)
set(DOAS_PERSIST_DIR "/run/doas" CACHE PATH "Directory holding timestamp files")
set(DOAS_UID_MAX "65535" CACHE STRING "Maximum uid accepted in doas.conf")
set(DOAS_GID_MAX "65535" CACHE STRING "Maximum gid accepted in doas.conf")
set(DOAS_SYSCONFDIR "/etc" CACHE PATH "Directory holding doas.conf")
set(DOAS_PAM_DIR "${DOAS_SYSCONFDIR}/pam.d" CACHE PATH "Directory for the PAM service definition")
option(DOAS_INSTALL_SETUID "Install the multicall binary setuid root" ON)
set(
    DOAS_LICENSE_DIR "${CMAKE_INSTALL_DATADIR}/licenses/doas-common"
    CACHE STRING
        "Prefix relative directory for LICENSE, NOTICE and LICENSES/ (packagers rename it after the package)"
)
set(DOAS_SANITIZE "" CACHE STRING "Sanitizer list for dev builds (e.g. address,undefined)")

# ---------------------------------------------------------------------------
# Shared compile / link flags, applied by every target through
# doas_target_defaults().
# ---------------------------------------------------------------------------
function(doas_target_defaults target)
    target_compile_definitions(${target} PRIVATE _GNU_SOURCE _DEFAULT_SOURCE)
    target_compile_options(
        ${target}
        PRIVATE
            -Wall
            -Wextra
            -Werror
            -Wconversion
            -Wsign-conversion
            -Wshorten-64-to-32
            -fno-exceptions
            -fno-rtti
            -ffunction-sections
            -fdata-sections
            -fstack-protector-strong
    )
    target_link_options(
        ${target}
        PRIVATE
            -fuse-ld=lld
            -static-libstdc++
            -static-libgcc
            -Wl,--gc-sections
            -Wl,-z,relro
            -Wl,-z,now
            -Wl,-z,noexecstack
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_options(${target} PRIVATE -D_FORTIFY_SOURCE=3)
    endif()
    if(DOAS_SANITIZE)
        target_compile_options(${target} PRIVATE -fsanitize=${DOAS_SANITIZE} -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=${DOAS_SANITIZE})
    endif()
endfunction()

add_subdirectory(src)
add_subdirectory(man)

# License and attribution: the primary MIT license, the third-party notices and the
# full text of every license they reference.
install(FILES LICENSE NOTICE DESTINATION ${DOAS_LICENSE_DIR})
install(DIRECTORY LICENSES DESTINATION ${DOAS_LICENSE_DIR})

if(DOAS_AUTH_PROVIDER STREQUAL "PAM")
    install(
        FILES pam.d__doas__linux
        DESTINATION ${DOAS_PAM_DIR}
        RENAME doas
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
    )
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Developer helpers: clang-format / clang-tidy
# ---------------------------------------------------------------------------
find_program(DOAS_CLANG_FORMAT NAMES clang-format)
file(
    GLOB_RECURSE
    DOAS_SOURCES
    CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/src/*.cpp
    ${CMAKE_SOURCE_DIR}/src/*.hpp
    ${CMAKE_SOURCE_DIR}/tests/*.cpp
    ${CMAKE_SOURCE_DIR}/tests/*.hpp
)
if(DOAS_CLANG_FORMAT)
    add_custom_target(
        format
        COMMAND ${DOAS_CLANG_FORMAT} -i ${DOAS_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Formatting sources with clang-format"
        VERBATIM
    )
    add_custom_target(
        format-check
        COMMAND ${DOAS_CLANG_FORMAT} --dry-run --Werror ${DOAS_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking clang-format"
        VERBATIM
    )
    if(BUILD_TESTING)
        add_test(
            NAME format
            COMMAND ${DOAS_CLANG_FORMAT} --dry-run --Werror ${DOAS_SOURCES}
        )
        set_tests_properties(format PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
endif()

find_program(DOAS_CLANG_TIDY NAMES clang-tidy)
find_program(DOAS_RUN_CLANG_TIDY NAMES run-clang-tidy run-clang-tidy.py)
if(DOAS_CLANG_TIDY)
    include(ProcessorCount)
    set(DOAS_TIDY_JOBS "" CACHE STRING "Parallel clang-tidy jobs (empty = every processor)")
    if(DOAS_TIDY_JOBS STREQUAL "")
        ProcessorCount(DOAS_TIDY_JOBS)
        if(DOAS_TIDY_JOBS EQUAL 0)
            set(DOAS_TIDY_JOBS 1)
        endif()
    endif()

    if(DOAS_RUN_CLANG_TIDY)
        add_custom_target(
            tidy
            COMMAND
                ${DOAS_RUN_CLANG_TIDY} -p ${CMAKE_BINARY_DIR} -j ${DOAS_TIDY_JOBS} -quiet
                "${CMAKE_SOURCE_DIR}/(src|tests)/.*[.](cpp|hpp)$"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Running clang-tidy with ${DOAS_TIDY_JOBS} jobs"
            VERBATIM
        )
    else()
        add_custom_target(
            tidy
            COMMAND ${DOAS_CLANG_TIDY} -p ${CMAKE_BINARY_DIR} --quiet ${DOAS_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Running clang-tidy (run-clang-tidy not found: single threaded)"
            VERBATIM
        )
    endif()
endif()
