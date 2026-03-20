# Add allocator benchmark executable
add_executable(allocator_bench allocator_bench.cpp)
target_link_libraries(allocator_bench PRIVATE cachelib_memory_allocator mooncake_store)

# Add master benchmark executable
# add_executable(master_bench master_bench.cpp)
# target_link_libraries(master_bench PRIVATE cachelib_memory_allocator mooncake_store)

# Add NoF worker pool benchmark executable
add_executable(nof_worker_pool_bench nof_worker_pool_bench.cpp)
target_link_libraries(nof_worker_pool_bench PRIVATE mooncake_store glog::glog gflags::gflags)
