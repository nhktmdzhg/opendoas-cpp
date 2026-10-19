function(add_doas_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ${ARGN})
    doas_target_defaults(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Pure unit tests over the sys layer.
add_doas_test(t_numbers sys_core)
add_doas_test(t_strings sys_core)
add_doas_test(t_utf8 sys_core)
add_doas_test(t_error sys_core)

# doas core: matching and environment construction.
add_doas_test(t_match sys_core doas_core)
add_doas_test(t_env sys_core doas_core)
add_doas_test(t_exec sys_core doas_core)

# Applets with pure logic.
add_doas_test(t_sudo_table sys_core sudo_app)
add_doas_test(t_doasedit_cli sys_core doasedit_app)

# The doasedit policy test substitutes the scripted privileged operations for the
# real ones.
add_executable(t_doasedit_policy t_doasedit_policy.cpp fake_elevated.cpp)
target_link_libraries(t_doasedit_policy PRIVATE sys_core doasedit_app doas_core)
doas_target_defaults(t_doasedit_policy)
add_test(NAME t_doasedit_policy COMMAND t_doasedit_policy)

# The real privileged operations and the in-process config validation, exercised as
# far as possible without the setuid bit.
add_doas_test(t_elevated sys_core doas_core doasedit_elevated)

# Tests that drive the real multicall binary through its applet symlinks.
foreach(name IN ITEMS t_golden t_identity t_sudo_main t_dispatch)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE sys_core)
    doas_target_defaults(${name})
    add_test(NAME ${name} COMMAND ${name})
    add_dependencies(${name} doas-common)
endforeach()

set(DOAS_APPLET_PATH "${CMAKE_BINARY_DIR}/src/applets/doas")
target_compile_definitions(t_golden PRIVATE DOAS_APPLET_PATH="${DOAS_APPLET_PATH}")
target_compile_definitions(t_identity PRIVATE DOAS_APPLET_PATH="${DOAS_APPLET_PATH}")
target_compile_definitions(
    t_golden PRIVATE DOAS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_compile_definitions(
    t_sudo_main PRIVATE DOAS_SUDO_APPLET_PATH="${CMAKE_BINARY_DIR}/src/applets/sudo"
)
target_compile_definitions(
    t_dispatch PRIVATE DOAS_COMMON_PATH="${CMAKE_BINARY_DIR}/src/applets/doas-common"
)
