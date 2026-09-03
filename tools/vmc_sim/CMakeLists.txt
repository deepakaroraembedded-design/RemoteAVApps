# C/CUDA MEC simulator: wire-identical replacement for tools/mec_sim.py.
add_executable(vmc-mec-sim main.c)
target_link_libraries(vmc-mec-sim PRIVATE vmc_thinclient)
target_link_libraries(vmc-mec-sim PRIVATE Threads::Threads)
target_compile_definitions(vmc-mec-sim PRIVATE _DEFAULT_SOURCE=1 _POSIX_C_SOURCE=200809L)
