# Find Python package
add_subdirectory(cachelib_memory_allocator)

set(MOONCAKE_STORE_SOURCES
    allocator.cpp
    master_service.cpp
    client_service.cpp
    client_metric.cpp
    types.cpp
    master_client.cpp
    utils.cpp
    master_metric_manager.cpp
    storage_backend.cpp
    thread_pool.cpp
    etcd_helper.cpp
    ha_helper.cpp
    segment.cpp
    transfer_task.cpp
    etcd_helper.cpp
    ha_helper.cpp
    rpc_service.cpp
    offset_allocator.cpp
    posix_file.cpp
    client_buffer.cpp
    real_client.cpp
    dummy_client.cpp
    http_metadata_server.cpp
    file_storage.cpp
    spdk_hello.cpp
    memory_alloc.cpp
    ssd_register_client.cpp
)

set(EXTRA_LIBS "")

if(USE_3FS)
  add_subdirectory(hf3fs)
  list(APPEND MOONCAKE_STORE_SOURCES  ${HF3FS_SOURCES})
  find_library(HF3FS_API_LIB hf3fs_api_shared PATHS /usr/lib NO_DEFAULT_PATH)
  if(NOT HF3FS_API_LIB)
    message(FATAL_ERROR "hf3fs_api_shared library not found in /usr/lib")
  endif()
  set(EXTRA_LIBS ${HF3FS_API_LIB})
endif()

add_subdirectory(spdk)
list(APPEND MOONCAKE_STORE_SOURCES  ${SPDK_WRAPPER_SOURCES})

# The cache_allocator library
include_directories(${Python3_INCLUDE_DIRS})
add_library(mooncake_store ${MOONCAKE_STORE_SOURCES})
# Note: transfer_engine is PRIVATE to avoid propagating its dependencies (e.g., vendor-specific hardware)
# to targets that don't need it (e.g., mooncake_master).
# Targets that need transfer_engine should link it explicitly.

set(SPDK_LIB_DIR /usr/local/lib)
set(DPDK_LIB_DIR /usr/local/lib)

set(SPDK_STATIC_LIBS
	-Wl,--whole-archive    # 确保libspdk_event.a 和libspdk_nvme.a全量链接，防止丢失预初始化内容
    ${SPDK_LIB_DIR}/libspdk_event.a
	${SPDK_LIB_DIR}/libspdk_rdma.a
    ${SPDK_LIB_DIR}/libspdk_nvme.a
	-Wl,--no-whole-archive  # 恢复按需链接
	${SPDK_LIB_DIR}/libspdk_vfio_user.a
	${SPDK_LIB_DIR}/libspdk_dma.a
	${SPDK_LIB_DIR}/libspdk_init.a
    ${SPDK_LIB_DIR}/libspdk_env_dpdk.a
    ${SPDK_LIB_DIR}/libspdk_log.a
    ${SPDK_LIB_DIR}/libspdk_util.a
    ${SPDK_LIB_DIR}/libspdk_blob.a
    ${SPDK_LIB_DIR}/libspdk_blobfs.a
    ${SPDK_LIB_DIR}/libspdk_bdev.a
    ${SPDK_LIB_DIR}/libspdk_conf.a
    ${SPDK_LIB_DIR}/libspdk_json.a
    ${SPDK_LIB_DIR}/libspdk_jsonrpc.a
    ${SPDK_LIB_DIR}/libspdk_sock.a
    ${SPDK_LIB_DIR}/libspdk_thread.a
    ${SPDK_LIB_DIR}/libspdk_trace.a
    ${SPDK_LIB_DIR}/libspdk_vmd.a
    ${SPDK_LIB_DIR}/libspdk_rpc.a

    ${DPDK_LIB_DIR}/librte_eal.a
    ${DPDK_LIB_DIR}/librte_mempool.a
    ${DPDK_LIB_DIR}/librte_ring.a
    ${DPDK_LIB_DIR}/librte_bus_pci.a
    ${DPDK_LIB_DIR}/librte_pci.a
    ${DPDK_LIB_DIR}/librte_timer.a
    ${DPDK_LIB_DIR}/librte_telemetry.a
    ${DPDK_LIB_DIR}/librte_kvargs.a
    ${DPDK_LIB_DIR}/librte_mbuf.a
	-Wl,--whole-archive # 确保librte_mempool_ring.a全量链接，防止丢失预初始化内容
    ${DPDK_LIB_DIR}/librte_mempool_ring.a
	-Wl,--no-whole-archive
    ${DPDK_LIB_DIR}/librte_power.a
    ${DPDK_LIB_DIR}/librte_ethdev.a
    ${DPDK_LIB_DIR}/librte_net.a
    ${DPDK_LIB_DIR}/librte_vhost.a
    ${DPDK_LIB_DIR}/librte_cryptodev.a
    ${DPDK_LIB_DIR}/librte_hash.a
    ${DPDK_LIB_DIR}/librte_rcu.a

    # 系统库
    pthread dl numa rt m uuid crypto aio z elf ibverbs rdmacm
)

target_link_libraries(mooncake_store
    PUBLIC
        cachelib_memory_allocator
        ${ETCD_WRAPPER_LIB}
        glog::glog
        gflags::gflags
        ${EXTRA_LIBS}
        ${SPDK_STATIC_LIBS}
    PRIVATE
        transfer_engine
)
if (STORE_USE_ETCD)
    add_dependencies(mooncake_store build_etcd_wrapper)
endif()

if (BUILD_SHARED_LIBS)
  install(TARGETS mooncake_store DESTINATION lib)
endif()

# Master binary
add_executable(mooncake_master master.cpp)

set(MASTER_EXTRA_INCS)
set(MASTER_EXTRA_LIBS)

if (STORE_USE_JEMALLOC)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(JEMALLOC REQUIRED jemalloc)
    list(APPEND MASTER_EXTRA_INCS ${JEMALLOC_INCLUDE_DIRS})
    list(APPEND MASTER_EXTRA_LIBS ${JEMALLOC_STATIC_LIBRARIES})
endif()

target_include_directories(mooncake_master PRIVATE ${MASTER_EXTRA_INCS})
target_link_libraries(mooncake_master PRIVATE
    mooncake_store
    cachelib_memory_allocator
    pthread
    ibverbs
    mooncake_common
    ${ETCD_WRAPPER_LIB}
    ${MASTER_EXTRA_LIBS}
)

if (STORE_USE_ETCD)
    add_dependencies(mooncake_master build_etcd_wrapper)
endif()

target_compile_options(mooncake_master PRIVATE -Os)
target_link_options(mooncake_master PRIVATE -Os -s)

# Client server binary
add_executable(mooncake_client real_client_main.cpp)
# Client needs transfer_engine for data transfer operations
target_link_libraries(mooncake_client PRIVATE mooncake_store transfer_engine)
target_compile_options(mooncake_client PRIVATE -Os)
target_link_options(mooncake_client PRIVATE -Os -s)

install(TARGETS mooncake_master mooncake_client DESTINATION bin)
