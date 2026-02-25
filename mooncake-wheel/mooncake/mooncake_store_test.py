import logging
import uuid
import ctypes
import unittest
import argparse
import asyncio
import json
import time

logging.basicConfig(
    level=logging.INFO, format="%(asctime)s - %(levelname)s - %(message)s"
)
logger = logging.getLogger(__name__)

from mooncake.store import MooncakeDistributedStore, ReplicateConfig, get_alloc_func_addr

class MooncakeStoreTestService:

    def __init__(self, config_path: str = None, cli_config: dict = None):
        self.store = None

    def _setup_logging(self):
        logging.basicConfig(
            level=logging.INFO,
            format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
        )

    async def start_store_test_service(self):
        try:
            """Test the set API with a single key-value pair."""
            print("=" * 100)
            print("Testing single operation")

            buffer_size = 1024 * 1024 * 16  # 16MB
            self.store = MooncakeDistributedStore()

            ret_code = self.store.setup("localhost", "http://192.168.65.81:8080/metadata", 64 * 1024 * 1024, 0,
                   "rdma", "rocep65s0f0", "192.168.65.81:50051")
            if ret_code:
                logger.error(f"failed to setup mooncake store, error code: {ret_code}")

            logger.info("Connect to Mooncake store successfully.")
    
            test_data = b"Hello, put_from config world!"
            key = "test_put_from_config_key"
          
            func_addr = get_alloc_func_addr()
            HugepageAllocFunc = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_size_t)
            alloc_func = HugepageAllocFunc(func_addr)
            buffer = alloc_func(buffer_size)
            if not buffer:
                raise RuntimeError("SPDK memory allocation failed")
        
            result = self.store.register_buffer(buffer, buffer_size)
            if result:
                logger.error("put_buf register failed.")
            logger.info("put_buf register successfully.")
        
            # Copy test data to buffer
            ctypes.memmove(buffer, test_data, len(test_data))
        
            # Test with default config (backward compatibility)
            result = self.store.put_from(key=key, buffer_ptr=buffer, size=len(test_data))
            if result:
                logger.error("put_from buf failed.")
            logger.info(f"put_from {test_data} successfully.")


            recv_buffer = alloc_func(buffer_size)
            result = self.store.register_buffer(recv_buffer, buffer_size)
            if result:
                logger.error("get_buf register failed.")
            logger.info("get_buf register successfully.")

            # Verify data
            self.store.get_into(key=key, buffer_ptr=recv_buffer, size=len(test_data))
            recv_array = (ctypes.c_ubyte * len(test_data)).from_address(recv_buffer)
            retrieved_data = bytes(recv_array)
            assert retrieved_data == test_data, f"Data mismatch! Expected {test_data}, got {retrieved_data}"
            if retrieved_data != test_data:
                 logger.error(f"Data mismatch! Expected {test_data}, got {retrieved_data}")
            logger.info(f"get_into {retrieved_data} successfully.")

            # # Clean up
            # time.sleep(default_kv_lease_ttl / 1000)
            # self.store.unregister_buffer(buffer_ptr)
            # self.store.remove(key2)

            logger.info(f"✅ Single operation passed")
            return True
        except Exception as e:
            logging.error("Store startup failed: %s", e)
            return False

    async def stop(self):
        if self.store:
            self.store.close()
            logging.info("Mooncake service stopped")


async def main():
    service = MooncakeStoreTestService()
    try:
        if not await service.start_store_test_service():
            raise RuntimeError("Failed to start store test service")

        while True:
            await asyncio.sleep(1)

    except KeyboardInterrupt:
        await service.stop()
    except Exception as e:
        logging.error("Service error: %s", e)
        await service.stop()

if __name__ == "__main__":
    asyncio.run(main())
