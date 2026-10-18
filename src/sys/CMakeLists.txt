# Thin C++ wrappers around libc: errors, io, numbers, strings, time, users,
# filesystem, processes, secrets and the terminal.
add_library(
    sys_core
    STATIC
    error.cpp
    fs.cpp
    io.cpp
    numbers.cpp
    process.cpp
    secret.cpp
    strings.cpp
    terminal.cpp
    users.cpp
)
target_include_directories(sys_core PUBLIC ${PROJECT_SOURCE_DIR}/src)
doas_target_defaults(sys_core)
