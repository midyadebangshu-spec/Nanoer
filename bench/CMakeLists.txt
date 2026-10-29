# Benchmarks (docs/M6_PLAN.md §6): programs here, scripts beside them.
add_executable(nanoer-scale scale.cpp)
target_link_libraries(nanoer-scale PRIVATE nanoer_core nanoer_warnings)
