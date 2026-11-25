import sys
from unittest.mock import patch

import pytest

from mooncake.mooncake_ssd_create_and_register import parse_arguments
from mooncake import mooncake_ssd_create_and_register as create
from mooncake.mooncake_ssd_tool_common import SPDKTgtCreator


def test_parse_arguments_accepts_ssh_port():
    with patch.object(
        sys,
        "argv",
        [
            "mooncake_ssd_create_and_register",
            "--skip-register",
            "--spdk_target_info",
            "ip:127.0.0.1 path:/home/spdk",
            "--port",
            "2222",
        ],
    ):
        args = parse_arguments()

    assert args.port == 2222
    assert args.master_server_address is None


@pytest.mark.parametrize("phase_args", [[], ["--skip-create"]])
def test_registration_requires_master_address(phase_args):
    with patch.object(
        sys,
        "argv",
        [
            "mooncake_ssd_create_and_register",
            "--spdk_target_info",
            "ip:127.0.0.1 path:/home/spdk",
            *phase_args,
        ],
    ):
        with pytest.raises(SystemExit) as error:
            parse_arguments()
    assert error.value.code == 2


def test_ssh_connect_uses_requested_port():
    creator = SPDKTgtCreator(["ip:127.0.0.1 path:/home/spdk"], port=2222)

    with patch(
        "mooncake.mooncake_ssd_tool_common.require_paramiko"
    ) as require_paramiko:
        ssh_client = require_paramiko.return_value.SSHClient
        creator._ssh_connect("127.0.0.1")

    ssh_client.return_value.connect.assert_called_once_with(
        "127.0.0.1",
        port=2222,
        username="root",
        password=None,
    )


def test_create_only_does_not_contact_master():
    with (
        patch.object(
            sys,
            "argv",
            [
                "mooncake_ssd_create_and_register",
                "--spdk_target_info",
                "ip:127.0.0.1 path:/home/spdk",
                "--skip-register",
            ],
        ),
        patch.object(create, "SPDKTgtCreator") as creator,
        patch.object(create, "MooncakeNoFRegister") as register,
    ):
        create.main()

    creator.return_value.deploy_all_targets.assert_called_once_with()
    register.assert_not_called()
