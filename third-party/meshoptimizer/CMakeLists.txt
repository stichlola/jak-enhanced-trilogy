add_library(meshoptimizer allocator.cpp indexgenerator.cpp simplifier.cpp)
target_include_directories(meshoptimizer PUBLIC .)
