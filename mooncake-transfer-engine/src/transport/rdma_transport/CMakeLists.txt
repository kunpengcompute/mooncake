file(GLOB RDMA_SOURCES "*.cpp")

add_library(rdma_transport OBJECT ${RDMA_SOURCES})
target_link_libraries(
  rdma_transport
  PUBLIC gflags::gflags glog::glog
  PRIVATE JsonCpp::JsonCpp pthread)

if(USE_CUDA)
  target_compile_definitions(rdma_transport PRIVATE _GNU_SOURCE)
  target_link_libraries(rdma_transport PUBLIC ${CMAKE_DL_LIBS})
endif()

if(USE_MLX5DV)
  target_compile_definitions(rdma_transport PRIVATE USE_MLX5DV)
endif()

if(USE_ASCEND_NDR OR USE_ASCEND_STAGING)
  target_link_libraries(rdma_transport PUBLIC ascendcl)
endif()
if(USE_ASCEND_NDR)
  find_library(
    ASCEND_HAL_LIBRARY
    NAMES ascend_hal
    HINTS "$ENV{ASCEND_DRIVER_PATH}/lib64/driver"
          "/usr/local/Ascend/driver/lib64/driver" "${ASCEND_LIB_DIR}" REQUIRED)
  target_link_libraries(rdma_transport PUBLIC ${ASCEND_HAL_LIBRARY})
endif()
