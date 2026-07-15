#!/usr/bin/env python3
# Unregister SSD namespaces from Mooncake master and optionally remove them from SPDK target.

import argparse
import json
import logging
import re
import shlex
import sys
from typing import Any, Dict, List

from mooncake.mooncake_ssd_tool_common import MooncakeNoFUnregister


def _parse_target_info(target_info: str) -> Dict[str, str]:
    return {key.strip(): value.strip() for key, value in re.findall(r"(\w+):([^\s]+)", target_info)}


def _ctrlr_name_from_bdev(bdev_name: str) -> str:
    match = re.match(r"^(.*)n\d+$", bdev_name)
    return match.group(1) if match else bdev_name


class SPDKNamespaceRemover:
    def __init__(self, args):
        self.args = args

    def _connect(self, ip: str) -> Any:
        import paramiko

        ssh = paramiko.SSHClient()
        ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        ssh.connect(
            ip,
            port=int(self.args.port),
            username=self.args.username,
            password=self.args.password,
            key_filename=self.args.key_file,
            timeout=10,
        )
        return ssh

    def _find_spdk_path(self, ssh: Any, path: str) -> str:
        possible_paths = [path, f"{path}/spdk"]
        for test_path in possible_paths:
            command = f"cd {shlex.quote(test_path)} && test -f scripts/rpc.py"
            stdin, stdout, stderr = ssh.exec_command(command, timeout=5)
            if stdout.channel.recv_exit_status() == 0:
                return test_path
        raise RuntimeError(f"Could not find scripts/rpc.py in: {possible_paths}")

    def _exec_rpc(self, ssh: Any, spdk_path: str, command: str) -> str:
        full_command = f"cd {shlex.quote(spdk_path)} && ./scripts/rpc.py {command}"
        logging.info("RPC: %s", full_command)
        if self.args.dry_run:
            return "[]"

        stdin, stdout, stderr = ssh.exec_command(full_command, timeout=30)
        exit_status = stdout.channel.recv_exit_status()
        output = stdout.read().decode("utf-8")
        error = stderr.read().decode("utf-8")
        if exit_status != 0:
            raise RuntimeError(error or output)
        return output

    def _exec_rpc_ignore_absent(self, ssh: Any, spdk_path: str, command: str, absent_keywords: List[str]) -> str:
        try:
            return self._exec_rpc(ssh, spdk_path, command)
        except RuntimeError as e:
            message = str(e).lower()
            if any(keyword in message for keyword in absent_keywords):
                logging.info("RPC target already absent, skipping: %s", command)
                return ""
            raise

    def _list_namespaces(
        self, ssh: Any, spdk_path: str, target: Dict[str, str]
    ) -> List[Dict[str, Any]]:
        output = self._exec_rpc(ssh, spdk_path, "nvmf_get_subsystems")
        if self.args.dry_run:
            nsid = int(target["ns"]) if "ns" in target else 0
            nqn = target.get("nqn", "nqn.2016-06.io.spdk:cnode1")
            return [{"nqn": nqn, "nsid": nsid, "bdev_name": ""}]

        subsystems = json.loads(output)
        requested_nqn = target.get("nqn")
        requested_ns = int(target["ns"]) if "ns" in target else None
        namespaces = []

        for subsystem in subsystems:
            if subsystem.get("subtype") != "NVMe":
                continue
            nqn = subsystem.get("nqn")
            if requested_nqn and nqn != requested_nqn:
                continue

            for namespace in subsystem.get("namespaces", []):
                nsid = namespace.get("nsid")
                if not nsid:
                    continue
                if requested_ns is not None and int(nsid) != requested_ns:
                    continue
                namespaces.append(
                    {
                        "nqn": nqn,
                        "nsid": int(nsid),
                        "bdev_name": namespace.get("bdev_name") or namespace.get("name") or "",
                    }
                )

        return namespaces

    def remove_for_target(self, target_info: str) -> bool:
        target = _parse_target_info(target_info)
        ip = target.get("ip")
        path = target.get("path")
        if not ip or not path:
            logging.error("Target removal requires ip and path in --spdk_target_info: %s", target_info)
            return False

        if self.args.dry_run:
            if "ns" in target:
                logging.info(
                    "Dry-run: would remove namespace from SPDK target: ip=%s nqn=%s nsid=%s",
                    ip,
                    target.get("nqn", "auto-discovered"),
                    target["ns"],
                )
            else:
                logging.info("Dry-run: would discover and remove matching namespace(s) from SPDK target: ip=%s", ip)
            if self.args.detach_bdev:
                logging.info("Dry-run: would also detach matching SPDK NVMe bdev controller(s)")
            return True

        ssh = self._connect(ip)
        try:
            spdk_path = self._find_spdk_path(ssh, path)
            namespaces = self._list_namespaces(ssh, spdk_path, target)
            if not namespaces:
                logging.warning("No namespace matched on target %s", ip)
                return True

            for namespace in namespaces:
                nqn = namespace["nqn"]
                nsid = namespace["nsid"]
                bdev_name = namespace.get("bdev_name") or ""
                logging.info("Removing SPDK namespace: ip=%s nqn=%s nsid=%s bdev=%s", ip, nqn, nsid, bdev_name)
                self._exec_rpc(ssh, spdk_path, f"nvmf_subsystem_remove_ns {shlex.quote(nqn)} {nsid}")

                if self.args.detach_bdev and bdev_name:
                    ctrlr_name = _ctrlr_name_from_bdev(bdev_name)
                    logging.info("Detaching SPDK NVMe bdev controller: ip=%s ctrlr=%s", ip, ctrlr_name)
                    self._exec_rpc_ignore_absent(
                        ssh,
                        spdk_path,
                        f"bdev_nvme_detach_controller {shlex.quote(ctrlr_name)}",
                        ["not found", "no such", "does not exist", "not exist"],
                    )

            return True
        except Exception as e:
            logging.error("Failed to remove namespace on target %s: %s", ip, e)
            return False
        finally:
            ssh.close()

    def remove_all(self) -> bool:
        success = True
        for target_info in self.args.spdk_target_info:
            success = self.remove_for_target(target_info) and success
        return success


def _unregister_cli_config(args):
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


def run_unregister_phase(args) -> bool:
    logging.info("=== Unregister SSD namespaces from Mooncake master ===")
    if args.dry_run:
        logging.info("Dry-run: would unregister SSD namespace(s) from Mooncake master")
        return True

    return MooncakeNoFUnregister(
        _unregister_cli_config(args),
        args.spdk_target_info,
    ).start_ssd_unregister_service()


def parse_arguments():
    parser = argparse.ArgumentParser(
        description="Unregister SSD namespaces from Mooncake master and optionally remove them from SPDK target",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=r"""
Workflow:
  1. Always unregister matching namespace(s) from Mooncake master first.
  2. If --remove-target-namespace is set, remove matching namespace(s) from SPDK target.
  3. If --detach-bdev is also set, detach the corresponding SPDK NVMe bdev controller.

Examples:
  # Unregister one namespace from master only.
  python3 -m mooncake.mooncake_ssd_unregister_and_remove \
    --master_server_address 192.168.65.13:50051 \
    --spdk_target_info "ip:192.168.65.57 path:/home/spdk ns:1 nqn:nqn.2016-06.io.spdk:cnode1"

  # Unregister all namespaces from master.
  python3 -m mooncake.mooncake_ssd_unregister_and_remove \
    --master_server_address 192.168.65.13:50051 \
    --spdk_target_info "ip:192.168.65.57 path:/home/spdk"

  # Unregister one namespace from master, then remove it from target.
  python3 -m mooncake.mooncake_ssd_unregister_and_remove \
    --master_server_address 192.168.65.13:50051 \
    --spdk_target_info "ip:192.168.65.57 path:/home/spdk ns:1 nqn:nqn.2016-06.io.spdk:cnode1" \
    --remove-target-namespace

  # Unregister all namespaces from master, then remove them from target.
  python3 -m mooncake.mooncake_ssd_unregister_and_remove \
    --master_server_address 192.168.65.13:50051 \
    --spdk_target_info "ip:192.168.65.57 path:/home/spdk" \
    --remove-target-namespace

  # Unregister one namespace, remove it from target, then detach its bdev controller.
  python3 -m mooncake.mooncake_ssd_unregister_and_remove \
    --master_server_address 192.168.65.13:50051 \
    --spdk_target_info "ip:192.168.65.57 path:/home/spdk ns:1 nqn:nqn.2016-06.io.spdk:cnode1" \
    --remove-target-namespace \
    --detach-bdev

  # Unregister all namespaces, remove them from target, then detach their bdev controllers.
  python3 -m mooncake.mooncake_ssd_unregister_and_remove \
    --master_server_address 192.168.65.13:50051 \
    --spdk_target_info "ip:192.168.65.57 path:/home/spdk" \
    --remove-target-namespace \
    --detach-bdev

Notes:
  - Omitting ns means "all namespaces discoverable from the target".
  - Discovering all namespaces requires a valid path to SPDK and SSH access to the target.
  - The tool does not support removing target namespaces without first unregistering from master.
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
            "SPDK target information. Use path to discover namespaces, for example "
            "\"ip:192.168.65.56 path:/home/spdk\"; add ns/nqn to restrict removal, "
            "for example \"ip:192.168.65.56 path:/home/spdk ns:2 nqn:nqn.2016-06.io.spdk:cnode1\"."
        ),
    )
    parser.add_argument(
        "--remove-target-namespace",
        action="store_true",
        help="Also remove matching namespace(s) from SPDK target by nvmf_subsystem_remove_ns",
    )
    parser.add_argument(
        "--detach-bdev",
        action="store_true",
        help="After removing namespace, also detach the underlying SPDK NVMe bdev controller",
    )
    parser.add_argument("--dry-run", action="store_true", help="Print commands without executing them")
    parser.add_argument("--username", type=str, default="root", help="SSH username for target nodes")
    parser.add_argument("--port", type=int, default=22, help="SSH port for target nodes")
    parser.add_argument("--password", type=str, help="SSH password for target nodes")
    parser.add_argument("--key-file", type=str, help="SSH private key file path")
    parser.add_argument(
        "-D",
        "--define",
        action="append",
        default=[],
        help="Override unregister configuration fields globally, for example -Dtrsvcid=4420",
    )
    return parser.parse_args()


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s - %(levelname)s - %(message)s")
    args = parse_arguments()

    if args.detach_bdev and not args.remove_target_namespace:
        logging.error("--detach-bdev requires --remove-target-namespace")
        sys.exit(1)

    if not run_unregister_phase(args):
        sys.exit(1)

    if args.remove_target_namespace:
        logging.info("=== Remove SSD namespace(s) from SPDK target ===")
        if not SPDKNamespaceRemover(args).remove_all():
            sys.exit(1)

    logging.info("SSD unregister/remove workflow completed")


if __name__ == "__main__":
    main()
