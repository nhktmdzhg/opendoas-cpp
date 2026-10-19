# The multicall binary: argv[0] decides which applet runs.
add_executable(doas-common main.cpp)
target_link_libraries(doas-common PRIVATE sys_core doas_core doasedit_app doasedit_elevated sudo_main)
doas_target_defaults(doas-common)

# Applet symlinks used by the dispatcher; they live next to the binary so that the
# relative target is a plain file name. The tests use the same paths.
foreach(applet IN ITEMS doas doasedit sudo)
    add_custom_command(
        TARGET doas-common
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink doas-common
                ${CMAKE_CURRENT_BINARY_DIR}/${applet}
        COMMENT "Creating the ${applet} applet symlink"
        VERBATIM
    )
endforeach()

if(DOAS_INSTALL_SETUID)
    set(DOAS_BINARY_PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE
        SETUID
    )
else()
    set(DOAS_BINARY_PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE
    )
endif()

install(
    TARGETS doas-common
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PERMISSIONS ${DOAS_BINARY_PERMISSIONS}
)
foreach(applet IN ITEMS doas doasedit sudo)
    install(
        CODE "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink doas-common \
              \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_BINDIR}/${applet}\")"
    )
endforeach()
