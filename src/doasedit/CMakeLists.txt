# The doasedit applet.
add_library(doasedit_app STATIC cli.cpp doasedit.cpp path.cpp)
target_link_libraries(doasedit_app PUBLIC sys_core)
doas_target_defaults(doasedit_app)

# The bridge to the in-process doas applet lives in its own library so that tests
# can link the scripted replacement instead (tests/fake_bridge.cpp).
add_library(doasedit_bridge STATIC bridge.cpp)
target_link_libraries(doasedit_bridge PUBLIC doasedit_app doas_core sys_core)
doas_target_defaults(doasedit_bridge)
