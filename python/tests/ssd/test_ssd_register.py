import json
import runpy
import sys
from types import ModuleType
from unittest.mock import Mock, call, patch


def _ssh_output(result=None):
    stdout = Mock()
    stdout.channel.recv_exit_status.return_value = 0
    stdout.read.return_value = json.dumps(result).encode()
    stderr = Mock()
    stderr.read.return_value = b""
    return None, stdout, stderr


def test_register_uses_per_target_rpc_sockets_on_same_host():
    ip = "192.168.65.56"
    path = "/home/spdk"
    master_address = "192.168.65.81:50051"
    nqn = "nqn.2016-06.io.spdk:cnode1"
    bdev_name = "Nvme0n1"
    args = [
        "mooncake_ssd_create_and_register",
        "--skip-create",
        "--master_server_address",
        master_address,
    ]
    ssh_calls = []
    ssh_responses = []
    register_calls = []

    for sock_field, rpc_args, port, num_blocks in (
        (" sock:/var/tmp/target1.sock", " -s /var/tmp/target1.sock", 4420, 1024),
        (" sock:target$2.sock", " -s 'target$2.sock'", 4421, 2048),
        ("", "", 4422, 4096),
    ):
        args.extend(["--spdk_target_info", f"ip:{ip} path:{path}{sock_field}"])
        rpc = f"cd {path} && ./scripts/rpc.py{rpc_args}"
        ssh_calls.extend(
            [
                call(f"cd {path} && test -f scripts/rpc.py", timeout=5),
                call(f"{rpc} nvmf_get_subsystems", timeout=30),
                call(f"cd {path} && test -f scripts/rpc.py", timeout=5),
                call(f"{rpc} bdev_get_bdevs -b {bdev_name}", timeout=30),
            ]
        )
        ssh_responses.extend(
            [
                _ssh_output(),
                _ssh_output(
                    [
                        {
                            "subtype": "NVMe",
                            "nqn": nqn,
                            "listen_addresses": [{"traddr": ip, "trsvcid": str(port)}],
                            "namespaces": [{"nsid": 1, "bdev_name": bdev_name}],
                        }
                    ]
                ),
                _ssh_output(),
                _ssh_output([{"block_size": 4096, "num_blocks": num_blocks}]),
            ]
        )
        register_calls.append(
            call(nqn, 1, ip, port, 0, 4096 * num_blocks, master_address, 4096)
        )

    args.extend(["--spdk_target_info", f"ip:{ip} path:{path} sock:"])

    store = ModuleType("mooncake.store")
    store.MooncakeDistributedNoFRegister = Mock()
    register = store.MooncakeDistributedNoFRegister.return_value
    register.real_register.return_value = 0
    with (
        patch.dict(sys.modules, {"mooncake.store": store}),
        patch.object(sys, "argv", args),
        patch("mooncake.mooncake_ssd_tool_common.require_paramiko") as require_paramiko,
    ):
        ssh = require_paramiko.return_value.SSHClient.return_value
        ssh.exec_command.side_effect = ssh_responses
        runpy.run_module(
            "mooncake.mooncake_ssd_create_and_register", run_name="__main__"
        )

    assert ssh.exec_command.call_args_list == ssh_calls
    assert register.real_register.call_args_list == register_calls
