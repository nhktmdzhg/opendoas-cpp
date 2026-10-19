# The doasedit applet.
add_library(doasedit_app STATIC cli.cpp doasedit.cpp path.cpp)
target_link_libraries(doasedit_app PUBLIC sys_core)
doas_target_defaults(doasedit_app)

# The privileged file operations live in their own library so that tests can link
# the scripted replacement instead (tests/fake_elevated.cpp).
add_library(doasedit_elevated STATIC elevated.cpp)
target_link_libraries(doasedit_elevated PUBLIC doasedit_app doas_core sys_core)
doas_target_defaults(doasedit_elevated)
