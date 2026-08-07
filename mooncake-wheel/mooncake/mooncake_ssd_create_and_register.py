#!/usr/bin/env python3
# Create SPDK NVMe-oF target namespaces and register them to Mooncake master.

import argparse
import logging
import sys

from mooncake.mooncake_ssd_tool_common import MooncakeNoFRegister, SPDKTgtCreator


def _run_phase(name: str, dry_run: bool, action) -> bool:
    logging.info("=== %s ===", name)
    if dry_run:
        logging.info("Dry-run: %s skipped", name)
        return True

    if not action():
        logging.error("%s failed", name)
        return False
    return True


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Create SPDK NVMe-oF target namespaces and register them to Mooncake master",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=r"""
Workflow:
  1. Create or reuse SPDK target resources: nvmf_tgt, transport, subsystem, bdev, namespace, listener.
  2. Discover active namespace(s) from the target.
  3. Register discovered namespace(s) to Mooncake master.

Examples:
  # Create target resources and register all discovered namespaces.
  python3 -m mooncake.mooncake_ssd_create_and_register \
    --master_server_address 192.168.65.13:50051 \
    --spdk_target_info "ip:192.168.65.57 path:/home/spdk pci:0000:01:00.0,0000:02:00.0"

  # Register namespaces only when target resources already exist.
  python3 -m mooncake.mooncake_ssd_create_and_register \
    --master_server_address 192.168.65.13:50051 \
    --spdk_target_info "ip:192.168.65.57 path:/home/spdk" \
    --skip-create

  # Create target resources only, without registering to master.
  python3 -m mooncake.mooncake_ssd_create_and_register \
    --master_server_address 192.168.65.13:50051 \
    --spdk_target_info "ip:192.168.65.57 path:/home/spdk pci:0000:01:00.0" \
    --skip-register

Notes:
  - --spdk_target_info can be specified multiple times for multiple targets.
  - If pci is omitted during create, the tool auto-discovers available NVMe PCI devices.
"""
    )
    parser.add_argument(
        "--master_server_address",
        type=str,
        required=True,
        help="Mooncake master server address, for example 192.168.65.81:50051",
    )
    parser.add_argument(
        "--spdk_target_info",
        action="append",
        required=True,
        help=(
            "SPDK target information, for example "
            "\"ip:192.168.65.56 path:/home/spdk pci:0000:01:00.0,0000:02:00.0\". "
            "The same target list is used for create and register phases."
        ),
    )
    parser.add_argument("--skip-create", action="store_true", help="Skip SPDK target create phase")
    parser.add_argument("--skip-register", action="store_true", help="Skip Mooncake SSD register phase")
    parser.add_argument("--dry-run", action="store_true", help="Print commands without executing them")

    parser.add_argument("--core-mask", type=str, default="0xff", help="CPU core mask for nvmf_tgt")
    parser.add_argument("--transport-type", type=str, default="RDMA", help="NVMe-oF transport type")
    parser.add_argument("--max-queue-depth", type=int, default=128)
    parser.add_argument("--max-io-qpairs-per-ctrlr", type=int, default=127)
    parser.add_argument("--max-io-size", type=int, default=4096)
    parser.add_argument("--in-capsule-data-size", type=int, default=131072)
    parser.add_argument("--io-unit-size", type=int, default=131072)
    parser.add_argument("--max-aq-depth", type=int, default=128)
    parser.add_argument("--num-shared-buffers", type=int, default=4096)
    parser.add_argument("--buf-cache-size", type=int, default=32)

    parser.add_argument("--username", type=str, default="root", help="SSH username for target nodes")
    parser.add_argument("--port", type=int, default=22, help="SSH port for register/discovery phase")
    parser.add_argument("--password", type=str, help="SSH password for target nodes")
    parser.add_argument("--key-file", type=str, help="SSH private key file path")
    parser.add_argument(
        "-D",
        "--define",
        action="append",
        default=[],
        help="Override register configuration fields globally, for example -Dtrsvcid=4420",
    )
    return parser.parse_args()


def _transport_options(args):
    return {
        "trtype": args.transport_type,
        "max_queue_depth": args.max_queue_depth,
        "max_io_qpairs_per_ctrlr": args.max_io_qpairs_per_ctrlr,
        "max_io_size": args.max_io_size,
        "in_capsule_data_size": args.in_capsule_data_size,
        "io_unit_size": args.io_unit_size,
        "max_aq_depth": args.max_aq_depth,
        "num_shared_buffers": args.num_shared_buffers,
        "buf_cache_size": args.buf_cache_size,
    }


def _register_cli_config(args):
    cli_config = {
        "master_server_address": args.master_server_address,
        "username": args.username,
        "port": args.port,
    }
    if args.password:
        cli_config["password"] = args.password
    if args.key_file:
        cli_config["key_file"] = args.key_file
    for item in args.define:
        if "=" in item:
            key, value = item.split("=", 1)
            cli_config[key] = value
        else:
            logging.warning("Ignoring invalid CLI config: %s", item)
    return cli_config


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s - %(levelname)s - %(message)s")
    args = parse_arguments()

    if args.skip_create and args.skip_register:
        logging.error("Both --skip-create and --skip-register are set; nothing to do")
        sys.exit(1)

    if not args.skip_create:
        if not _run_phase(
            "Create SPDK target",
            args.dry_run,
            lambda: SPDKTgtCreator(
                args.spdk_target_info,
                _transport_options(args),
                args.core_mask,
                args.username,
                args.password,
                args.key_file,
            ).deploy_all_targets(),
        ):
            sys.exit(1)

    if not args.skip_register:
        if not _run_phase(
            "Register SSD namespaces",
            args.dry_run,
            lambda: MooncakeNoFRegister(
                _register_cli_config(args),
                args.spdk_target_info,
            ).start_ssd_service(),
        ):
            sys.exit(1)

    logging.info("SSD create/register workflow completed")


if __name__ == "__main__":
    main()
