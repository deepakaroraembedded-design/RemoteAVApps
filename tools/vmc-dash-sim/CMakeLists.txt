# LL-DASH live server: ffmpeg DASH packager + minimal HTTP server.
option(VMC_DEBUG "DASH server instrumentation" OFF)
add_executable(vmc-dash-sim main.c)
target_compile_definitions(vmc-dash-sim PRIVATE _DEFAULT_SOURCE=1 _POSIX_C_SOURCE=200809L)
if(VMC_DEBUG)
    target_compile_definitions(vmc-dash-sim PRIVATE VMC_DEBUG=1)
endif()
target_link_libraries(vmc-dash-sim PRIVATE Threads::Threads)
