#!/usr/bin/env python3
# Shared implementation for Mooncake NoF SSD management CLI tools.

from __future__ import annotations

import json
import logging
import re
import shlex
import time
from typing import Any, Dict, List, Optional

class SPDKTgtCreator:
    """
    Remotely creates SPDK targets on multiple nodes via SSH.
    """

    DEFAULT_TRANSPORT_OPTIONS = {
        'trtype': 'RDMA',
        'max_queue_depth': 128,
        'max_io_qpairs_per_ctrlr': 127,
        'max_io_size': 4096,
        'in_capsule_data_size': 131072,
        'io_unit_size': 131072,
        'max_aq_depth': 128,
        'num_shared_buffers': 4096,
        'buf_cache_size': 32,
    }

    TRANSPORT_RPC_FLAGS = {
        'trtype': '-t',
        'max_queue_depth': '-q',
        'max_io_qpairs_per_ctrlr': '-m',
        'max_io_size': '-c',
        'in_capsule_data_size': '-i',
        'io_unit_size': '-u',
        'max_aq_depth': '-a',
        'num_shared_buffers': '-n',
        'buf_cache_size': '-b',
    }

    def __init__(
        self,
        spdk_targets: List[str],
        transport_options: Dict[str, Any] = None,
        core_mask: str = '0xff',
        username: str = 'root',
        password: str = None,
        key_file: str = None,
    ):
        self.spdk_targets = spdk_targets
        self.core_mask = core_mask
        self.username = username
        self.password = password
        self.key_file = key_file
        self.transport_options = dict(self.DEFAULT_TRANSPORT_OPTIONS)
        if transport_options:
            self.transport_options.update(transport_options)
        self._setup_logging()
        self.target_configs = self._parse_spdk_targets()

    def _setup_logging(self):
        logging.basicConfig(
            level=logging.INFO,
            format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
        )
        self.logger = logging.getLogger(self.__class__.__name__)

    def _parse_spdk_targets(self) -> List[Dict[str, Any]]:
        """
        Parse SPDK target information from command line arguments.
        Format: "ip:<ip> path:<spdk_path> [pci:<pci1>,<pci2> ...]"
        """
        target_configs = []

        for target_info in self.spdk_targets:
            target = {
                'ip': None,
                'path': None,
                'pci_devices': []
            }

            # Split the target info by spaces
            parts = target_info.split()

            # Simple state machine to parse the target info
            state = None  # Can be 'ip', 'path', or 'pci'

            for part in parts:
                if ':' in part:
                    # This is a key-value pair
                    key, value = part.split(':', 1)
                    key = key.strip()
                    value = value.strip()

                    if key == 'ip':
                        target['ip'] = value
                        state = 'ip'
                    elif key == 'path':
                        target['path'] = value
                        state = 'path'
                    elif key == 'pci':
                        # Parse PCI devices separated by commas
                        if value:
                            # Split by commas and strip whitespace
                            pci_list = self._split_pci_devices(value)
                            target['pci_devices'].extend(pci_list)
                        state = 'pci'
                    elif state == 'pci':
                        pci_list = self._split_pci_devices(part)
                        target['pci_devices'].extend(pci_list)
                else:
                    # This is a continuation of the current state
                    if state == 'path':
                        # Path might contain spaces (unlikely but possible)
                        target['path'] += ' ' + part
                    elif state == 'pci':
                        pci_list = self._split_pci_devices(part)
                        target['pci_devices'].extend(pci_list)

            # Validate required fields
            if not target['ip']:
                raise ValueError("Each spdk_target_info must contain 'ip' field")
            if not target['path']:
                raise ValueError("Each spdk_target_info must contain 'path' field")
            target['pci_devices'] = self._dedupe_pci_devices(target['pci_devices'])

            target_configs.append(target)
            pci_info = target['pci_devices'] if target['pci_devices'] else 'auto-discover'
            self.logger.info(f"Parsed target: IP={target['ip']}, Path={target['path']}, PCI devices={pci_info}")

        return target_configs

    def _split_pci_devices(self, value: str) -> List[str]:
        """
        Extract PCI addresses even when users type non-ASCII separators.
        """
        pci_pattern = re.compile(
            r'(?:[0-9a-fA-F]{4}:)?[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]'
        )
        matches = pci_pattern.findall(value)
        if matches:
            return matches

        normalized = re.sub(r'[^0-9a-fA-F:.]+', ',', value)
        return [dev.strip() for dev in normalized.split(',') if dev.strip()]

    def _dedupe_pci_devices(self, pci_devices: List[str]) -> List[str]:
        """
        Preserve input order while dropping duplicate PCI addresses.
        """
        seen = set()
        deduped = []
        for pci in pci_devices:
            normalized = pci.lower()
            if normalized in seen:
                self.logger.info(f"Skipping duplicate PCI device in input: {pci}")
                continue
            seen.add(normalized)
            deduped.append(pci)
        return deduped

    def _ssh_connect(self, ip: str, username: str = 'root', password: str = None, key_file: str = None) -> paramiko.SSHClient:
        """
        Establish an SSH connection to the target host.
        """
        import paramiko

        ssh = paramiko.SSHClient()
        ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())

        try:
            if key_file:
                self.logger.info(f"Connecting to {ip} using key file {key_file}")
                ssh.connect(ip, username=username, key_filename=key_file)
            else:
                self.logger.info(f"Connecting to {ip} using password authentication")
                ssh.connect(ip, username=username, password=password)
            return ssh
        except Exception as e:
            self.logger.error(f"Failed to connect to {ip}: {e}")
            raise

    def _execute_command(self, ssh: paramiko.SSHClient, command: str, working_dir: str = None, sudo: bool = False, log_errors: bool = True, timeout: Optional[int] = 30) -> tuple:
        """
        Execute a command on the remote host via SSH.
        """
        if working_dir:
            command = f"cd {working_dir} && {command}"

        if sudo:
            command = f"sudo {command}"

        self.logger.debug(f"Executing command: {command}")

        stdin, stdout, stderr = ssh.exec_command(command, timeout=timeout)
        exit_status = stdout.channel.recv_exit_status()
        output = stdout.read().decode('utf-8')
        error = stderr.read().decode('utf-8')

        if output:
            self.logger.debug(f"Command output: {output}")
        if error:
            self.logger.debug(f"Command error: {error}")
        if exit_status != 0:
            if log_errors:
                self.logger.error(f"Command failed with exit code {exit_status}: {command}")
                if output:
                    self.logger.error(f"Command output: {output}")
                self.logger.error(f"Error output: {error}")
            raise RuntimeError(f"Command execution failed: {error or output}")

        return output, error

    def _rpc_script(self, spdk_path: str) -> str:
        return f"{spdk_path}/scripts/rpc.py"

    def _rpc_json(self, ssh: paramiko.SSHClient, spdk_path: str, command: str) -> Any:
        output, _ = self._execute_command(ssh, f"{self._rpc_script(spdk_path)} {command}")
        if not output.strip():
            return None
        return json.loads(output)

    def _is_spdk_tgt_running(self, ssh: paramiko.SSHClient) -> bool:
        try:
            self._execute_command(ssh, "pgrep -x nvmf_tgt", log_errors=False)
            return True
        except RuntimeError:
            return False

    def _discover_nvme_pci_devices(self, ssh: paramiko.SSHClient) -> List[str]:
        """
        Discover SPDK-ready or unmounted NVMe controller PCI addresses on the target host.
        """
        self.logger.info("No PCI devices specified, discovering SPDK-ready or unmounted NVMe PCI devices")
        output, _ = self._execute_command(
            ssh,
            r"""for dev in /sys/bus/pci/devices/*; do
  class=$(cat "$dev/class" 2>/dev/null || true)
  case "$class" in
    0x0108*) ;;
    *) continue ;;
  esac
  pci=$(basename "$dev")
  driver=$(basename "$(readlink "$dev/driver" 2>/dev/null)" 2>/dev/null || true)
  case "$driver" in
    vfio-pci|uio_pci_generic|igb_uio)
      echo "USE $pci $driver"
      continue
      ;;
  esac
  has_block=0
  mounted=0
  for block in /sys/block/nvme*n*; do
    [ -e "$block" ] || continue
    real_device=$(readlink -f "$block/device" 2>/dev/null || true)
    case "$real_device" in
      *"/$pci"/*|*"/$pci") ;;
      *) continue ;;
    esac
    has_block=1
    disk=$(basename "$block")
    if lsblk -nr -o MOUNTPOINT "/dev/$disk" 2>/dev/null | grep -q '[^[:space:]]'; then
        mounted=1
    fi
  done
  if [ "$has_block" -eq 0 ]; then
    echo "SKIP $pci no_block_device"
  elif [ "$mounted" -eq 0 ]; then
    echo "USE $pci"
  else
    echo "SKIP $pci mounted"
  fi
done"""
        )

        pci_devices = []
        pci_pattern = re.compile(
            r'^(?:[0-9a-fA-F]{4}:)?[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$'
        )
        for line in output.splitlines():
            fields = line.split()
            if len(fields) < 2:
                continue
            action, pci = fields[0], fields[1]
            if action == 'USE' and pci_pattern.match(pci):
                pci_devices.append(pci)
            elif action == 'SKIP' and pci_pattern.match(pci):
                reason = ' '.join(fields[2:]) or 'not eligible'
                self.logger.warning(f"Skipping NVMe PCI device {pci}: {reason}")

        if not pci_devices:
            raise RuntimeError("No SPDK-ready or unmounted NVMe PCI devices found on the target host")

        self.logger.info(f"Selected NVMe PCI devices for SPDK setup: {', '.join(pci_devices)}")
        return pci_devices

    def _filter_spdk_ready_pci_devices(self, ssh: paramiko.SSHClient, pci_devices: List[str], strict: bool) -> List[str]:
        """
        Keep PCI devices that are bound to an SPDK-compatible userspace driver.
        """
        if not pci_devices:
            return []

        pci_args = ' '.join(shlex.quote(pci) for pci in pci_devices)
        output, _ = self._execute_command(
            ssh,
            f"""for pci in {pci_args}; do
  driver=$(basename "$(readlink "/sys/bus/pci/devices/$pci/driver" 2>/dev/null)" 2>/dev/null || true)
  case "$driver" in
    vfio-pci|uio_pci_generic|igb_uio) echo "READY $pci $driver" ;;
    "") echo "NOT_READY $pci no_driver" ;;
    *) echo "NOT_READY $pci $driver" ;;
  esac
done"""
        )

        ready_devices = []
        not_ready_devices = []
        for line in output.splitlines():
            fields = line.split()
            if len(fields) < 3:
                continue
            state, pci, driver = fields[0], fields[1], fields[2]
            if state == 'READY':
                ready_devices.append(pci)
            elif state == 'NOT_READY':
                not_ready_devices.append((pci, driver))

        if not_ready_devices:
            details = ', '.join(f"{pci} ({driver})" for pci, driver in not_ready_devices)
            if strict:
                raise RuntimeError(
                    "Some PCI devices are not available to SPDK after setup.sh: "
                    f"{details}. Check whether they are mounted or still bound to the kernel driver."
                )
            self.logger.warning(f"Skipping PCI devices not available to SPDK after setup.sh: {details}")

        if not ready_devices:
            raise RuntimeError("No PCI devices are available to SPDK after setup.sh")

        self.logger.info(f"PCI devices available to SPDK: {', '.join(ready_devices)}")
        return ready_devices

    def _start_spdk_tgt(self, ssh: paramiko.SSHClient, spdk_path: str) -> None:
        """
        Start the SPDK NVMF target service in the background.
        """
        if self._is_spdk_tgt_running(ssh):
            self.logger.info("SPDK tgt service is already running")
            return

        # Start tgt in the background using absolute path
        self.logger.info(f"Starting SPDK tgt service with core mask {self.core_mask}")
        tgt_binary = f"{spdk_path}/build/bin/nvmf_tgt"
        log_file = f"{spdk_path}/tgt.log"
        self._execute_command(
            ssh,
            f"nohup {tgt_binary} -m {shlex.quote(self.core_mask)} > {log_file} 2>&1 &",
            timeout=None
        )
        time.sleep(3)  # Give it time to start

    def _setup_spdk(self, ssh: paramiko.SSHClient, spdk_path: str, pci_devices: List[str]) -> None:
        """
        Setup SPDK with the specified PCI devices.
        """
        self.logger.info(f"Setting up SPDK with PCI devices: {', '.join(pci_devices)}")
        pci_allowed = ' '.join(pci_devices)
        setup_script = f"{spdk_path}/scripts/setup.sh"
        self._execute_command(ssh, f"PCI_ALLOWED='{pci_allowed}' {setup_script}", sudo=True)

    def _format_transport_options(self) -> str:
        """
        Format transport options for the SPDK nvmf_create_transport RPC.
        """
        formatted_options = []
        for option, flag in self.TRANSPORT_RPC_FLAGS.items():
            value = self.transport_options[option]
            formatted_options.extend([flag, shlex.quote(str(value))])
        return ' '.join(formatted_options)

    def _create_transport(
        self,
        ssh: paramiko.SSHClient,
        spdk_path: str,
        log_errors: bool = True,
    ) -> None:
        """
        Create NVMe-oF transport for SPDK.
        """
        self.logger.info(f"Creating {self.transport_options['trtype']} transport")
        rpc_script = f"{spdk_path}/scripts/rpc.py"
        transport_options = self._format_transport_options()
        self._execute_command(
            ssh,
            f"{rpc_script} nvmf_create_transport {transport_options}",
            log_errors=log_errors,
        )

    def _ensure_transport(self, ssh: paramiko.SSHClient, spdk_path: str) -> None:
        """
        Create the NVMe-oF transport if it does not already exist.
        """
        try:
            self._create_transport(ssh, spdk_path, log_errors=False)
        except RuntimeError as e:
            message = str(e).lower()
            if "exist" not in message and "already" not in message:
                self.logger.error(f"Failed to create {self.transport_options['trtype']} transport: {e}")
                raise
            self.logger.info(f"{self.transport_options['trtype']} transport already exists, reusing it")

    def _get_bdevs(self, ssh: paramiko.SSHClient, spdk_path: str) -> List[Dict[str, Any]]:
        bdevs = self._rpc_json(ssh, spdk_path, "bdev_get_bdevs")
        return bdevs or []

    def _find_bdev_for_pci(self, bdevs: List[Dict[str, Any]], pci: str) -> Optional[str]:
        pci_lower = pci.lower()
        pci_without_domain = pci_lower[-7:]
        for bdev in bdevs:
            serialized = json.dumps(bdev, sort_keys=True).lower()
            if pci_lower in serialized or pci_without_domain in serialized:
                return bdev.get("name")
        return None

    def _next_nvme_controller_name(self, bdevs: List[Dict[str, Any]]) -> str:
        max_index = -1
        for bdev in bdevs:
            name = bdev.get("name", "")
            match = re.match(r"^Nvme(\d+)n\d+$", name)
            if match:
                max_index = max(max_index, int(match.group(1)))
        return f"Nvme{max_index + 1}"

    def _create_bdevs(self, ssh: paramiko.SSHClient, spdk_path: str, pci_devices: List[str]) -> List[str]:
        """
        Create block devices for the specified PCI devices.
        """
        bdevs = []
        rpc_script = f"{spdk_path}/scripts/rpc.py"
        existing_bdevs = self._get_bdevs(ssh, spdk_path)
        for pci in pci_devices:
            existing_bdev = self._find_bdev_for_pci(existing_bdevs, pci)
            if existing_bdev:
                self.logger.info(f"PCI {pci} is already attached as bdev {existing_bdev}")
                bdevs.append(existing_bdev)
                continue

            bdev_name = self._next_nvme_controller_name(existing_bdevs)
            self.logger.info(f"Creating bdev {bdev_name} for PCI {pci}")
            self._execute_command(ssh, f"{rpc_script} bdev_nvme_attach_controller -b {bdev_name} -t PCIe -a {pci}")
            attached_bdev = f"{bdev_name}n1"
            bdevs.append(attached_bdev)
            existing_bdevs.append({"name": attached_bdev, "pci_address": pci})
            self.logger.info(f"Attached PCI {pci} as bdev {attached_bdev}")
        return bdevs

    def _create_subsystem(self, ssh: paramiko.SSHClient, spdk_path: str) -> str:
        """
        Create an NVMF subsystem.
        """
        subsystem_nqn = "nqn.2016-06.io.spdk:cnode1"
        self.logger.info(f"Creating subsystem {subsystem_nqn}")
        rpc_script = f"{spdk_path}/scripts/rpc.py"
        self._execute_command(ssh, f"{rpc_script} nvmf_create_subsystem {subsystem_nqn} -a -s SPDK00000000000001 -m 12")
        return subsystem_nqn

    def _get_subsystems(self, ssh: paramiko.SSHClient, spdk_path: str) -> List[Dict[str, Any]]:
        subsystems = self._rpc_json(ssh, spdk_path, "nvmf_get_subsystems")
        return subsystems or []

    def _find_subsystem(self, ssh: paramiko.SSHClient, spdk_path: str, subsystem_nqn: str) -> Optional[Dict[str, Any]]:
        for subsystem in self._get_subsystems(ssh, spdk_path):
            if subsystem.get("nqn") == subsystem_nqn:
                return subsystem
        return None

    def _ensure_subsystem(self, ssh: paramiko.SSHClient, spdk_path: str) -> str:
        subsystem_nqn = "nqn.2016-06.io.spdk:cnode1"
        if self._find_subsystem(ssh, spdk_path, subsystem_nqn):
            self.logger.info(f"Subsystem {subsystem_nqn} already exists, reusing it")
            return subsystem_nqn
        return self._create_subsystem(ssh, spdk_path)

    def _add_namespaces(self, ssh: paramiko.SSHClient, spdk_path: str, subsystem_nqn: str, bdevs: List[str]) -> None:
        """
        Add namespaces to the subsystem.
        """
        rpc_script = f"{spdk_path}/scripts/rpc.py"
        for bdev in bdevs:
            subsystem = self._find_subsystem(ssh, spdk_path, subsystem_nqn)
            namespaces = subsystem.get("namespaces", []) if subsystem else []
            if any(namespace.get("bdev_name") == bdev for namespace in namespaces):
                self.logger.info(f"Namespace {bdev} already exists in subsystem {subsystem_nqn}")
                continue
            self.logger.info(f"Adding namespace {bdev} to subsystem {subsystem_nqn}")
            self._execute_command(ssh, f"{rpc_script} nvmf_subsystem_add_ns {subsystem_nqn} {bdev}")

    def _add_listener(self, ssh: paramiko.SSHClient, spdk_path: str, subsystem_nqn: str, ip: str) -> None:
        """
        Add a listener to the subsystem.
        """
        trtype = self.transport_options['trtype']
        subsystem = self._find_subsystem(ssh, spdk_path, subsystem_nqn)
        listen_addresses = subsystem.get("listen_addresses", []) if subsystem else []
        for address in listen_addresses:
            if (str(address.get("trtype", "")).lower() == str(trtype).lower()
                    and str(address.get("traddr", "")) == str(ip)
                    and str(address.get("trsvcid", "")) == "4420"):
                self.logger.info(f"{trtype} listener on {ip}:4420 already exists")
                return

        self.logger.info(f"Adding {trtype} listener on {ip}:4420")
        rpc_script = f"{spdk_path}/scripts/rpc.py"
        try:
            self._execute_command(ssh, f"{rpc_script} nvmf_subsystem_add_listener {subsystem_nqn} -t {shlex.quote(str(trtype))} -a {ip} -s 4420")
        except RuntimeError as e:
            message = str(e).lower()
            if "exist" not in message and "already" not in message:
                raise
            self.logger.info(f"{trtype} listener on {ip}:4420 already exists")

    def deploy_target(self, target_config: Dict[str, Any]) -> bool:
        """
        Deploy SPDK target on a single node.
        """
        ip = target_config['ip']
        spdk_path = target_config['path']
        pci_devices = target_config['pci_devices']

        self.logger.info(f"Deploying SPDK target on {ip}")

        try:
            # Establish SSH connection
            ssh = self._ssh_connect(ip, self.username, self.password, self.key_file)

            try:
                auto_discovered = not pci_devices
                if not pci_devices:
                    pci_devices = self._discover_nvme_pci_devices(ssh)
                else:
                    self.logger.info(f"Using explicitly specified PCI devices for {ip}: {', '.join(pci_devices)}")

                # Setup SPDK
                self._setup_spdk(ssh, spdk_path, pci_devices)
                pci_devices = self._filter_spdk_ready_pci_devices(
                    ssh,
                    pci_devices,
                    strict=not auto_discovered
                )
                self.logger.info(f"Target {ip} will expose PCI devices: {', '.join(pci_devices)}")

                target_already_running = self._is_spdk_tgt_running(ssh)
                if target_already_running:
                    self.logger.info("SPDK tgt service is running, hot-adding PCI devices without restart")
                else:
                    self._start_spdk_tgt(ssh, spdk_path)

                # Create transport
                self._ensure_transport(ssh, spdk_path)

                # Create bdevs
                bdevs = self._create_bdevs(ssh, spdk_path, pci_devices)

                # Create subsystem
                subsystem_nqn = self._ensure_subsystem(ssh, spdk_path)

                # Add namespaces
                self._add_namespaces(ssh, spdk_path, subsystem_nqn, bdevs)

                # Add listener
                self._add_listener(ssh, spdk_path, subsystem_nqn, ip)

                self.logger.info(f"Successfully deployed SPDK target on {ip}")
                return True
            finally:
                ssh.close()
        except Exception as e:
            self.logger.error(f"Failed to deploy SPDK target on {ip}: {e}")
            return False

    def deploy_all_targets(self) -> bool:
        """
        Deploy SPDK targets on all specified nodes.
        """
        success_count = 0
        total_count = len(self.target_configs)

        for i, target_config in enumerate(self.target_configs):
            self.logger.info(f"=== Deploying target {i+1}/{total_count} ===")
            if self.deploy_target(target_config):
                success_count += 1

        self.logger.info("=== Deployment Summary ===")
        self.logger.info(f"Total targets: {total_count}")
        self.logger.info(f"Successfully deployed: {success_count}")
        self.logger.info(f"Failed: {total_count - success_count}")

        return success_count == total_count



class MooncakeNoFRegister:
    """
    Registers SSDs from remote SPDK targets to Mooncake master server.
    """

    def __init__(self, cli_config: dict = None, spdk_targets: List[str] = None):
        self.register = None
        self.config_list: List[Dict[str, Any]] = []
        self.cli_config = cli_config or {}
        self.spdk_targets = spdk_targets or []
        self._setup_logging()

        try:
            # Only support getting SSD info from remote SPDK targets
            if self.spdk_targets:
                # Get SSD info from remote SPDK targets
                master_server_address = self.cli_config.get('master_server_address')
                if not master_server_address:
                    raise ValueError("master_server_address is required when using spdk_target_info")
                self.config_list = self._get_remote_ssd_info(master_server_address)
            else:
                raise ValueError("spdk_target_info is required")

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

            # Remove duplicate SSDs based on their unique identifiers
            self._remove_duplicate_ssds()

            logging.info("Loaded %d SSD configuration(s)", len(self.config_list))
        except Exception as e:
            logging.error("Configuration load failed: %s", e)
            raise



    def _remove_duplicate_ssds(self):
        """
        Remove duplicate SSD configurations from the config list
        """
        unique_configs = []
        seen_keys = set()

        for config in self.config_list:
            # Generate unique key directly without calling _get_ssd_unique_key
            key = (config['nqn'], config['nsid'], config['traddr'], config['trsvcid'])
            if key not in seen_keys:
                seen_keys.add(key)
                unique_configs.append(config)

        if len(unique_configs) < len(self.config_list):
            logging.info(f"Removed {len(self.config_list) - len(unique_configs)} duplicate SSD configuration(s)")

        self.config_list = unique_configs



    def _parse_spdk_target_info(self, target_info: str) -> Dict[str, str]:
        """
        Parse spdk target info string like "ip:192.168.65.56 path:/home"
        """
        result = {}
        parts = re.findall(r'(\w+):([^\s]+)', target_info)
        for key, value in parts:
            result[key] = value
        return result

    def _execute_ssh_command(self, ip: str, command: str, path: str) -> str:
        """
        Execute command on remote server via SSH
        """
        import paramiko

        ssh = paramiko.SSHClient()
        ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        try:
            ssh.connect(
                ip,
                port=int(self.cli_config.get('port', 22)),
                username=self.cli_config.get('username', 'root'),
                password=self.cli_config.get('password'),
                key_filename=self.cli_config.get('key_file'),
                timeout=10
            )

            # Try multiple possible paths to find the RPC script
            possible_paths = [
                path,  # Direct path provided by user
                f"{path}/spdk"  # Common case: spdk is a subdirectory
            ]

            for test_path in possible_paths:
                full_command = f"cd {shlex.quote(test_path)} && test -f scripts/rpc.py"
                stdin, stdout, stderr = ssh.exec_command(full_command, timeout=5)
                if stdout.channel.recv_exit_status() == 0:
                    # Found the script, execute the actual command
                    full_command = f"cd {shlex.quote(test_path)} && {command}"
                    stdin, stdout, stderr = ssh.exec_command(full_command, timeout=30)
                    exit_status = stdout.channel.recv_exit_status()
                    output = stdout.read().decode('utf-8')
                    error = stderr.read().decode('utf-8')
                    if exit_status != 0:
                        logging.error(f"SSH command error on {ip}: {error}")
                        raise RuntimeError(f"SSH command failed: {error or output}")
                    return output

            # If we get here, none of the paths worked
            raise RuntimeError(f"Could not find scripts/rpc.py in any of the possible paths: {possible_paths}")
        finally:
            ssh.close()

    def _get_remote_ssd_info(self, master_server_address: str) -> List[Dict[str, Any]]:
        """
        Get SSD info from remote SPDK targets
        """
        ssd_configs = []

        for target_info in self.spdk_targets:
            target = self._parse_spdk_target_info(target_info)
            ip = target.get('ip')
            path = target.get('path')

            if not ip or not path:
                logging.error(f"Invalid target info: {target_info}")
                continue

            logging.info(f"Getting SSD info from target: {ip} (path: {path})")

            try:
                # Get subsystems info
                subsystems_cmd = "./scripts/rpc.py nvmf_get_subsystems"
                subsystems_output = self._execute_ssh_command(ip, subsystems_cmd, path)
                subsystems = json.loads(subsystems_output)

                # Process each subsystem
                for subsystem in subsystems:
                    if subsystem.get('subtype') != 'NVMe':
                        continue

                    nqn = subsystem.get('nqn')
                    listen_addresses = subsystem.get('listen_addresses', [])

                    if not nqn or not listen_addresses:
                        continue

                    # Get transport info from first listen address
                    traddr = listen_addresses[0].get('traddr')
                    trsvcid = listen_addresses[0].get('trsvcid')

                    if not traddr or not trsvcid:
                        continue

                    # Process each namespace
                    namespaces = subsystem.get('namespaces', [])
                    for namespace in namespaces:
                        nsid = namespace.get('nsid')
                        bdev_name = namespace.get('bdev_name')

                        if not nsid or not bdev_name:
                            continue

                        # Get bdev info to calculate size
                        bdev_cmd = f"./scripts/rpc.py bdev_get_bdevs -b {shlex.quote(bdev_name)}"
                        bdev_output = self._execute_ssh_command(ip, bdev_cmd, path)
                        bdevs = json.loads(bdev_output)

                        if not bdevs:
                            continue

                        bdev = bdevs[0]
                        block_size = bdev.get('block_size', 512)
                        num_blocks = bdev.get('num_blocks', 0)
                        size = block_size * num_blocks

                        # Create SSD config
                        ssd_config = {
                            'nqn': nqn,
                            'nsid': nsid,
                            'traddr': traddr,
                            'trsvcid': int(trsvcid),  # Ensure trsvcid is integer
                            'base': 0,
                            'size': size,
                            'master_server_address': master_server_address,
                            'metadata_server': ''
                        }

                        ssd_configs.append(ssd_config)
                        logging.info(f"Found SSD: nqn={nqn}, nsid={nsid}, traddr={traddr}, size={size}")

            except Exception as e:
                logging.error(f"Failed to get SSD info from {ip}: {e}")
                continue

        if not ssd_configs:
            raise RuntimeError("No SSD information found from remote targets")

        return ssd_configs

    def _setup_logging(self):
        logging.basicConfig(
            level=logging.INFO,
            format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
        )

    def start_ssd_service(self):
        success_count = 0
        skipped_count = 0
        failed_count = 0
        total = len(self.config_list)

        for i, cfg in enumerate(self.config_list):
            try:
                logging.info("Registering SSD %d/%d: nqn=%s, traddr=%s", i + 1, total, cfg.get("nqn"), cfg.get("traddr"))

                # Create register instance and register SSD
                from mooncake.store import MooncakeDistributedNoFRegister

                self.register = MooncakeDistributedNoFRegister()
                ret = self.register.real_register(
                    cfg["nqn"],
                    cfg["nsid"],
                    cfg["traddr"],
                    cfg["trsvcid"],
                    cfg["base"],
                    cfg["size"],
                    cfg["master_server_address"]
                )

                if ret != 0:
                    raise RuntimeError(f"Registration failed with code {ret}")

                logging.info("Register SSD %d/%d succeeded", i + 1, total)
                success_count += 1

            except Exception as e:
                # Check if the error is due to the segment already existing on the server
                if "SEGMENT_ALREADY_EXISTS" in str(e) or "segment already exists" in str(e):
                    logging.info("SSD %d/%d (nqn=%s, traddr=%s) already registered on server, skipping",
                                i + 1, total, cfg.get("nqn"), cfg.get("traddr"))
                    skipped_count += 1
                else:
                    logging.error("Failed to register SSD %d/%d: %s", i + 1, total, e)
                    failed_count += 1

        # Summary
        logging.info("SSD registration summary:")
        logging.info("- Total SSDs: %d", total)
        logging.info("- Successfully registered: %d", success_count)
        logging.info("- Already registered (skipped): %d", skipped_count)
        logging.info("- Failed: %d", failed_count)

        # Return success if all SSDs were either registered or already existed
        return failed_count == 0



class MooncakeNoFUnregister:
    """
    Unregisters SSDs from Mooncake master server by endpoint information.
    """

    def __init__(self, cli_config: dict = None, spdk_targets: List[str] = None):
        self.register = None
        self.config_list: List[Dict[str, Any]] = []
        self.cli_config = cli_config or {}
        self.spdk_targets = spdk_targets or []
        self._setup_logging()

        try:
            # Only support getting SSD info from command line
            if self.spdk_targets:
                # Get SSD info from command line parameters
                master_server_address = self.cli_config.get('master_server_address')
                if not master_server_address:
                    raise ValueError("master_server_address is required when using spdk_target_info")
                self.config_list = self._parse_spdk_targets(master_server_address)
            else:
                raise ValueError("spdk_target_info is required")

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

            # Remove duplicate SSDs based on their unique identifiers
            self._remove_duplicate_ssds()

            logging.info("Loaded %d SSD configuration(s) to unregister", len(self.config_list))
        except Exception as e:
            logging.error("Configuration load failed: %s", e)
            raise

    def _remove_duplicate_ssds(self):
        """
        Remove duplicate SSD configurations from the config list
        """
        unique_configs = []
        seen_keys = set()

        for config in self.config_list:
            # Generate unique key directly
            key = (config['nqn'], config['nsid'], config['traddr'], config['trsvcid'])
            if key not in seen_keys:
                seen_keys.add(key)
                unique_configs.append(config)

        if len(unique_configs) < len(self.config_list):
            logging.info(f"Removed {len(self.config_list) - len(unique_configs)} duplicate SSD configuration(s)")

        self.config_list = unique_configs

    def _execute_ssh_command(self, ip: str, command: str, path: str) -> str:
        """
        Execute command on remote server via SSH
        """
        import paramiko

        ssh = paramiko.SSHClient()
        ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        try:
            ssh.connect(
                ip,
                port=int(self.cli_config.get('port', 22)),
                username=self.cli_config.get('username', 'root'),
                password=self.cli_config.get('password'),
                key_filename=self.cli_config.get('key_file'),
                timeout=10
            )

            # Try multiple possible paths to find the RPC script
            possible_paths = [
                path,  # Direct path provided by user
                f"{path}/spdk"  # Common case: spdk is a subdirectory
            ]

            for test_path in possible_paths:
                full_command = f"cd {shlex.quote(test_path)} && test -f scripts/rpc.py"
                stdin, stdout, stderr = ssh.exec_command(full_command, timeout=5)
                if stdout.channel.recv_exit_status() == 0:
                    # Found the script, execute the actual command
                    full_command = f"cd {shlex.quote(test_path)} && {command}"
                    stdin, stdout, stderr = ssh.exec_command(full_command, timeout=30)
                    exit_status = stdout.channel.recv_exit_status()
                    output = stdout.read().decode('utf-8')
                    error = stderr.read().decode('utf-8')
                    if exit_status != 0:
                        logging.error(f"SSH command error on {ip}: {error}")
                        raise RuntimeError(f"SSH command failed: {error or output}")
                    return output

            # If we get here, none of the paths worked
            raise RuntimeError(f"Could not find scripts/rpc.py in any of the possible paths: {possible_paths}")
        finally:
            ssh.close()

    def _parse_spdk_targets(self, master_server_address: str) -> List[Dict[str, Any]]:
        """
        Parse spdk target info strings like "ip:192.168.65.56" or "ip:192.168.65.56 ns:2"
        or "ip:192.168.65.56 path:/home/spdk" to get actual namespaces from SPDK target
        """
        ssd_configs = []

        for target_info in self.spdk_targets:
            # Parse target info
            target = {}
            parts = target_info.split()
            for part in parts:
                if ':' in part:
                    key, value = part.split(':', 1)
                    target[key.strip()] = value.strip()

            # Validate required fields
            if 'ip' not in target:
                raise ValueError("spdk_target_info must contain 'ip' field")

            ip = target['ip']
            specified_ns = int(target['ns']) if 'ns' in target else None
            path = target.get('path')

            # We need to know the NQN to build the te_endpoint
            # For now, we'll use a default NQN pattern (this should be improved)
            default_nqn = "nqn.2016-06.io.spdk:cnode1"
            nqn = target.get('nqn', default_nqn)

            # Default transport parameters
            trsvcid = int(target.get('port', '4420'))
            trtype = target.get('trtype', 'RDMA')

            # Create SSD config for each namespace (or specified ns only)
            if specified_ns is not None:
                # Unregister specific namespace
                ssd_config = {
                    'nqn': nqn,
                    'nsid': specified_ns,
                    'traddr': ip,
                    'trsvcid': trsvcid,
                    'base': 0,
                    'size': 0,  # Size is not needed for unregister
                    'master_server_address': master_server_address,
                    'metadata_server': ''
                }
                ssd_configs.append(ssd_config)
                logging.info(f"Will unregister SSD: nqn={nqn}, nsid={specified_ns}, traddr={ip}")
            elif path is not None:
                # Get actual namespaces from SPDK target
                logging.info(f"Getting namespace info from target: {ip} (path: {path})")
                try:
                    # Get subsystems info
                    subsystems_cmd = "./scripts/rpc.py nvmf_get_subsystems"
                    subsystems_output = self._execute_ssh_command(ip, subsystems_cmd, path)
                    subsystems = json.loads(subsystems_output)

                    # Process each subsystem
                    for subsystem in subsystems:
                        if subsystem.get('subtype') != 'NVMe':
                            continue

                        subsystem_nqn = subsystem.get('nqn')
                        listen_addresses = subsystem.get('listen_addresses', [])

                        if not subsystem_nqn or not listen_addresses:
                            continue

                        # Get transport info from first listen address
                        traddr = listen_addresses[0].get('traddr')
                        target_trsvcid = listen_addresses[0].get('trsvcid')

                        if not traddr or not target_trsvcid:
                            continue

                        # Use the nqn from the subsystem if not specified
                        current_nqn = nqn if nqn != default_nqn else subsystem_nqn
                        current_trsvcid = int(target_trsvcid) if target_trsvcid else trsvcid

                        # Process each namespace
                        namespaces = subsystem.get('namespaces', [])
                        for namespace in namespaces:
                            nsid = namespace.get('nsid')

                            if not nsid:
                                continue

                            # Create SSD config
                            ssd_config = {
                                'nqn': current_nqn,
                                'nsid': nsid,
                                'traddr': traddr,
                                'trsvcid': current_trsvcid,
                                'base': 0,
                                'size': 0,  # Size is not needed for unregister
                                'master_server_address': master_server_address,
                                'metadata_server': ''
                            }
                            ssd_configs.append(ssd_config)
                            logging.info(f"Will unregister SSD: nqn={current_nqn}, nsid={nsid}, traddr={traddr}")

                except Exception as e:
                    logging.error(f"Failed to get namespace info from {ip}: {e}")
                    logging.warning("Falling back to unregistering default namespace (nsid=1)")
                    # Fall back to unregistering default namespace
                    ssd_config = {
                        'nqn': nqn,
                        'nsid': 1,
                        'traddr': ip,
                        'trsvcid': trsvcid,
                        'base': 0,
                        'size': 0,
                        'master_server_address': master_server_address,
                        'metadata_server': ''
                    }
                    ssd_configs.append(ssd_config)
                    logging.info(f"Will unregister SSD (fallback): nqn={nqn}, nsid=1, traddr={ip}")
            else:
                # No path provided, unregister default namespace only
                logging.warning("No 'path' provided in spdk_target_info, cannot query actual namespaces from SPDK target")
                logging.warning("Will unregister default namespace (nsid=1) only")
                ssd_config = {
                    'nqn': nqn,
                    'nsid': 1,
                    'traddr': ip,
                    'trsvcid': trsvcid,
                    'base': 0,
                    'size': 0,
                    'master_server_address': master_server_address,
                    'metadata_server': ''
                }
                ssd_configs.append(ssd_config)
                logging.info(f"Will unregister SSD: nqn={nqn}, nsid=1, traddr={ip}")

        if not ssd_configs:
            raise RuntimeError("No SSD configuration generated from target info")

        return ssd_configs

    def _setup_logging(self):
        logging.basicConfig(
            level=logging.INFO,
            format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
        )

    def start_ssd_unregister_service(self):
        success_count = 0
        skipped_count = 0
        failed_count = 0
        total = len(self.config_list)

        for i, cfg in enumerate(self.config_list):
            try:
                logging.info("Unregistering SSD %d/%d: nqn=%s, traddr=%s, nsid=%d", i + 1, total, cfg.get("nqn"), cfg.get("traddr"), cfg.get("nsid"))

                # Create register instance and unregister SSD
                from mooncake.store import MooncakeDistributedNoFRegister

                self.register = MooncakeDistributedNoFRegister()
                ret = self.register.real_unregister_by_endpoint(
                    cfg["nqn"],
                    cfg["nsid"],
                    cfg["traddr"],
                    cfg["trsvcid"],
                    cfg["master_server_address"]
                )

                if ret != 0:
                    raise RuntimeError(f"Unregistration failed with code {ret}")

                logging.info("Unregister SSD %d/%d succeeded", i + 1, total)
                success_count += 1

            except Exception as e:
                if "SEGMENT_NOT_FOUND" in str(e) or "segment not found" in str(e).lower():
                    logging.info("SSD %d/%d (nqn=%s, traddr=%s, nsid=%d) is already unregistered, skipping",
                                 i + 1, total, cfg.get("nqn"), cfg.get("traddr"), cfg.get("nsid"))
                    skipped_count += 1
                else:
                    logging.error("Failed to unregister SSD %d/%d: %s", i + 1, total, e)
                    failed_count += 1

        # Summary
        logging.info("SSD unregistration summary:")
        logging.info("- Total SSDs: %d", total)
        logging.info("- Successfully unregistered: %d", success_count)
        logging.info("- Already unregistered (skipped): %d", skipped_count)
        logging.info("- Failed: %d", failed_count)

        # Return success if all SSDs were either unregistered or already absent.
        return failed_count == 0


