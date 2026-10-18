# The sudo applet: sudo command line translation plus the in-process hand-off to
# the doas applet.
add_library(sudo_app STATIC sudo.cpp)
target_link_libraries(sudo_app PUBLIC sys_core)
doas_target_defaults(sudo_app)

add_library(sudo_main STATIC main.cpp)
target_link_libraries(sudo_main PUBLIC sudo_app doas_core sys_core)
doas_target_defaults(sudo_main)
