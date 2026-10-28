import json
from unittest.mock import Mock

import pytest

from mooncake.mooncake_ssd_tool_common import MooncakeNoFRegister


@pytest.fixture
def register():
    # Exercise discovery without opening SSH or loading native bindings.
    return MooncakeNoFRegister.__new__(MooncakeNoFRegister)


def test_explicit_pci_selection_is_normalized_and_deduplicated(register):
    assert register._parse_selected_pci_devices(
        {"pci": "0000:AB:01.0,0000:ab:01.0,02:00.0"}
    ) == ["0000:ab:01.0", "02:00.0"]


def test_omitted_pci_keeps_unrestricted_discovery(register):
    assert register._parse_selected_pci_devices({}) == []
    assert register._bdev_matches_selected_pci({}, [])


@pytest.mark.parametrize(
    "selection",
    ["", "invalid", "0000:01:00.8", "0000:01:00.0,bad", "10000:01:00.0"],
)
def test_invalid_explicit_pci_is_rejected(register, selection):
    with pytest.raises(ValueError, match="Invalid PCI address"):
        register._parse_selected_pci_devices({"pci": selection})


@pytest.mark.parametrize(
    "reported,selected,expected",
    [
        ("0000:AB:01.0", "0000:ab:01.0", True),
        ("0001:01:00.0", "0000:01:00.0", False),
        ("01:00.0", "0000:01:00.0", True),
        ("0000:01:00.0", "01:00.0", True),
        ("0000:02:00.0", "0000:01:00.0", False),
        ("0000:01:00.1", "0000:01:00.0", False),
        ("10000:01:00.0", "0000:01:00.0", False),
    ],
)
def test_pci_filter_preserves_explicit_domains(register, reported, selected, expected):
    bdev = {"driver_specific": {"nvme": [{"trid": {"traddr": reported}}]}}
    assert register._bdev_matches_selected_pci(bdev, [selected]) is expected


def test_bdev_without_selected_pci_metadata_is_not_registered(register):
    assert not register._bdev_matches_selected_pci(
        {"name": "Nvme0n1", "block_size": 4096}, ["0000:01:00.0"]
    )


def prepare_discovery(register, target_info):
    register.spdk_targets = [target_info]
    subsystems = [
        {
            "subtype": "NVMe",
            "nqn": "nqn.test",
            "listen_addresses": [{"traddr": "127.0.0.1", "trsvcid": "4420"}],
            "namespaces": [
                {"nsid": 1, "bdev_name": "Nvme0n1"},
                {"nsid": 2, "bdev_name": "Nvme1n1"},
            ],
        }
    ]
    bdevs = {
        "Nvme0n1": {
            "pci_address": "0000:01:00.0",
            "block_size": 4096,
            "num_blocks": 16,
        },
        "Nvme1n1": {"pci_address": "0000:02:00.0", "block_size": 512, "num_blocks": 32},
    }

    def rpc(ip, command, path):
        if "nvmf_get_subsystems" in command:
            return json.dumps(subsystems)
        return json.dumps([bdevs[command.split()[-1]]])

    register._execute_ssh_command = Mock(side_effect=rpc)


def test_discovery_only_registers_namespaces_backed_by_selected_pci(register):
    prepare_discovery(register, "ip:127.0.0.1 path:/spdk pci:0000:02:00.0")
    configs = register._get_remote_ssd_info("master:50051")
    assert len(configs) == 1
    assert configs[0]["nsid"] == 2
    assert configs[0]["size"] == 512 * 32
    assert configs[0]["base"] == 0
    assert configs[0]["master_server_address"] == "master:50051"


def test_discovery_without_pci_keeps_all_namespaces(register):
    prepare_discovery(register, "ip:127.0.0.1 path:/spdk")
    assert [cfg["nsid"] for cfg in register._get_remote_ssd_info("master:50051")] == [
        1,
        2,
    ]


def test_unmatched_pci_does_not_fall_back_to_registering_all_namespaces(register):
    prepare_discovery(register, "ip:127.0.0.1 path:/spdk pci:0000:03:00.0")
    with pytest.raises(RuntimeError, match="No SSD information"):
        register._get_remote_ssd_info("master:50051")


def test_invalid_selection_fails_before_ssh(register):
    prepare_discovery(register, "ip:127.0.0.1 path:/spdk pci:bad")
    with pytest.raises(ValueError, match="Invalid PCI address"):
        register._get_remote_ssd_info("master:50051")
    register._execute_ssh_command.assert_not_called()
