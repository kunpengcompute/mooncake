#!/usr/bin/env python3
# python3 -m mooncake.mooncake_ssd_register --config=xxx.json

import argparse
import json
import logging
import time
from typing import List, Dict, Any

from mooncake.store import MooncakeDistributedSSDRegister
from mooncake.mooncake_config import MooncakeConfig


class MooncakeSSDRegister:
    """
    Configuration Example (JSON format):

    [
      {
        "nqn": "nqn.spdk:cnode",
        "nsid": 1,
        "traddr": "192.168.65.56",
        "trsvcid": 4420,
        "base":0,
        "size":8196,
        "master_server_address": "192.168.65.87:50051",
        "metadata_server": ""
      },
      {
        "nqn": "nqn.spdk:cnode1",
        "nsid": 1,
        "traddr": "192.168.65.57",
        "trsvcid": 4420,
        "base":0,
        "size":8196,
        "master_server_address": "192.168.65.87:50051",
        "metadata_server": ""
      },
      .......
    ]

    """

    def __init__(self, config_path: str = None, cli_config: dict = None):
        self.register = None
        self.config_list: List[Dict[str, Any]] = []
        self.cli_config = cli_config or {}
        self._setup_logging()

        try:
            if config_path:
                with open(config_path, 'r') as f:
                    data = json.load(f)
                if isinstance(data, list):
                    self.config_list = data
                else:
                    self.config_list = [data]
            else:
                # Fallback to env-based single config
                config_obj = MooncakeConfig.load_from_env()
                self.config_list = [config_obj.__dict__]

            # Apply CLI overrides to every config (if key exists)
            for config in self.config_list:
                for key, value in self.cli_config.items():
                    if key in config:
                        # Convert trsvcid/nsid to int if needed
                        if key in ("trsvcid", "nsid"):
                            try:
                                config[key] = int(value)
                            except ValueError:
                                logging.warning(f"Invalid integer for {key}: {value}")
                        else:
                            config[key] = value

            logging.info("Loaded %d SSD configuration(s)", len(self.config_list))
        except Exception as e:
            logging.error("Configuration load failed: %s", e)
            raise

    def _setup_logging(self):
        logging.basicConfig(
            level=logging.INFO,
            format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
        )

    def start_ssd_service(self):
        success_count = 0
        total = len(self.config_list)

        for i, cfg in enumerate(self.config_list):
            try:
                logging.info("Registering SSD %d/%d: nqn=%s, traddr=%s", i + 1, total, cfg.get("nqn"), cfg.get("traddr"))

                self.register = MooncakeDistributedSSDRegister()
                ret = self.register.real_register(
                    cfg["nqn"],
                    cfg["nsid"],
                    cfg["traddr"],
                    cfg["trsvcid"],
                    cfg["master_server_address"],
                    cfg["base"],
                    cfg["size"]
                )
                if ret != 0:
                    raise RuntimeError(f"Registration failed with code {ret}")

                logging.info("Register SSD %d/%d succeeded", i + 1, total)
                success_count += 1

            except Exception as e:
                logging.error("Failed to register SSD %d/%d: %s", i + 1, total, e)

        if success_count == total:
            logging.info("All %d SSD(s) registered successfully!", total)
            return True
        else:
            logging.error("%d/%d SSD(s) failed to register", total - success_count, total)
            return False


def parse_arguments():
    parser = argparse.ArgumentParser(description='Mooncake SSD Register with REST API')
    parser.add_argument('--config', type=str,
                        help='Path to Mooncake config file (supports JSON array or single object)',
                        required=False)
    parser.add_argument('-D', '--define', action='append',
                        help='Override configuration fields globally (e.g., -Dtrsvcid=4420)',
                        default=[])
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_arguments()

    cli_config = {}
    for item in args.define:
        if '=' in item:
            key, value = item.split('=', 1)
            cli_config[key] = value
        else:
            logging.warning(f"Ignoring invalid CLI config: {item}")

    register = MooncakeSSDRegister(args.config, cli_config)
    success = register.start_ssd_service()
    if not success:
        exit(1)
