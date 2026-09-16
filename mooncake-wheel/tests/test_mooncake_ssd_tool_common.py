import json
import sys
import types
import unittest
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

from mooncake.mooncake_ssd_tool_common import (
    MooncakeNoFRegister,
    MooncakeNoFUnregister,
    SPDKTgtCreator,
)


class _Stream:
    def __init__(self, data="", status=0):
        self._data = data.encode()
        self.channel = SimpleNamespace(recv_exit_status=lambda: status)

    def read(self):
        return self._data


class _SSH:
    def __init__(self, responses=None):
        self.responses = list(responses or [])
        self.commands = []
        self.closed = False

    def exec_command(self, command, timeout=None):
        self.commands.append((command, timeout))
        status, output, error = self.responses.pop(0)
        return None, _Stream(output, status), _Stream(error)

    def close(self):
        self.closed = True


def _creator(targets=None):
    return SPDKTgtCreator(targets or ["ip:10.0.0.1 path:/opt/spdk"])


def _fake_store(register_type):
    module = types.ModuleType("mooncake.store")
    module.MooncakeDistributedNoFRegister = register_type
    return patch.dict(sys.modules, {"mooncake.store": module})


class SPDKTgtCreatorTest(unittest.TestCase):
    def test_parse_target_and_deduplicate_pci_devices(self):
        creator = _creator([
            "ip:10.0.0.1 path:/opt/spdk pci:0000:01:00.0,0000:01:00.0 02:00.0"
        ])
        self.assertEqual(creator.target_configs[0]["pci_devices"], ["0000:01:00.0", "02:00.0"])

    def test_parse_target_rejects_missing_required_fields(self):
        with self.assertRaisesRegex(ValueError, "ip"):
            _creator(["path:/opt/spdk"])
        with self.assertRaisesRegex(ValueError, "path"):
            _creator(["ip:10.0.0.1"])

    def test_split_pci_accepts_non_ascii_separator_and_fallback(self):
        creator = _creator()
        self.assertEqual(
            creator._split_pci_devices("0000:01:00.0;0000:02:00.1"),
            ["0000:01:00.0", "0000:02:00.1"],
        )
        self.assertEqual(creator._split_pci_devices("abc/def"), ["abc", "def"])

    def test_execute_command_builds_command_and_returns_output(self):
        ssh = _SSH([(0, "ok", "warning")])
        output, error = _creator()._execute_command(
            ssh, "rpc", working_dir="/opt/spdk", sudo=True
        )
        self.assertEqual((output, error), ("ok", "warning"))
        self.assertEqual(ssh.commands[0][0], "sudo cd /opt/spdk && rpc")

    def test_execute_command_raises_on_failure(self):
        with self.assertRaisesRegex(RuntimeError, "denied"):
            _creator()._execute_command(_SSH([(1, "", "denied")]), "rpc")

    def test_rpc_json_handles_empty_and_json_output(self):
        creator = _creator()
        with patch.object(creator, "_execute_command", return_value=("", "")):
            self.assertIsNone(creator._rpc_json(None, "/spdk", "get"))
        with patch.object(creator, "_execute_command", return_value=('[{"x": 1}]', "")):
            self.assertEqual(creator._rpc_json(None, "/spdk", "get"), [{"x": 1}])

    def test_target_running_detection(self):
        creator = _creator()
        with patch.object(creator, "_execute_command"):
            self.assertTrue(creator._is_spdk_tgt_running(None))
        with patch.object(creator, "_execute_command", side_effect=RuntimeError("missing")):
            self.assertFalse(creator._is_spdk_tgt_running(None))

    def test_discover_devices_filters_invalid_and_skipped_entries(self):
        creator = _creator()
        output = "\n".join([
            "USE 0000:01:00.0 vfio-pci",
            "SKIP 0000:02:00.0 mounted",
            "USE invalid value",
            "noise",
        ])
        with patch.object(creator, "_execute_command", return_value=(output, "")):
            self.assertEqual(creator._discover_nvme_pci_devices(None), ["0000:01:00.0"])
        with patch.object(creator, "_execute_command", return_value=("SKIP 0000:02:00.0 mounted", "")):
            with self.assertRaisesRegex(RuntimeError, "No SPDK-ready"):
                creator._discover_nvme_pci_devices(None)

    def test_filter_ready_devices_supports_strict_and_best_effort(self):
        creator = _creator()
        output = "READY 0000:01:00.0 vfio-pci\nNOT_READY 0000:02:00.0 nvme"
        with patch.object(creator, "_execute_command", return_value=(output, "")):
            self.assertEqual(
                creator._filter_spdk_ready_pci_devices(None, ["0000:01:00.0", "0000:02:00.0"], False),
                ["0000:01:00.0"],
            )
            with self.assertRaisesRegex(RuntimeError, "not available"):
                creator._filter_spdk_ready_pci_devices(None, ["0000:01:00.0", "0000:02:00.0"], True)
        self.assertEqual(creator._filter_spdk_ready_pci_devices(None, [], False), [])
        with patch.object(creator, "_execute_command", return_value=("NOT_READY 0000:02:00.0 nvme", "")):
            with self.assertRaisesRegex(RuntimeError, "No PCI devices"):
                creator._filter_spdk_ready_pci_devices(None, ["0000:02:00.0"], False)

    def test_start_target_reuses_running_process_or_starts_new_one(self):
        creator = _creator()
        with patch.object(creator, "_is_spdk_tgt_running", return_value=True), patch.object(
            creator, "_execute_command"
        ) as execute:
            creator._start_spdk_tgt(None, "/spdk")
            execute.assert_not_called()
        with patch.object(creator, "_is_spdk_tgt_running", return_value=False), patch.object(
            creator, "_execute_command"
        ) as execute, patch("mooncake.mooncake_ssd_tool_common.time.sleep"):
            creator._start_spdk_tgt(None, "/spdk")
            self.assertIn("nvmf_tgt", execute.call_args.args[1])

    def test_transport_creation_is_idempotent(self):
        creator = _creator()
        self.assertIn("-t RDMA", creator._format_transport_options())
        with patch.object(creator, "_create_transport"):
            creator._ensure_transport(None, "/spdk")
        with patch.object(creator, "_create_transport", side_effect=RuntimeError("already exists")):
            creator._ensure_transport(None, "/spdk")
        with patch.object(creator, "_create_transport", side_effect=RuntimeError("permission denied")):
            with self.assertRaisesRegex(RuntimeError, "permission"):
                creator._ensure_transport(None, "/spdk")

    def test_bdev_helpers_reuse_and_create_devices(self):
        creator = _creator()
        existing = [{"name": "Nvme3n1", "driver_specific": {"pci_address": "0000:01:00.0"}}]
        self.assertEqual(creator._find_bdev_for_pci(existing, "0000:01:00.0"), "Nvme3n1")
        self.assertIsNone(creator._find_bdev_for_pci(existing, "0000:02:00.0"))
        self.assertEqual(creator._next_nvme_controller_name(existing), "Nvme4")
        with patch.object(creator, "_get_bdevs", return_value=existing.copy()), patch.object(
            creator, "_execute_command"
        ) as execute:
            result = creator._create_bdevs(None, "/spdk", ["0000:01:00.0", "0000:02:00.0"])
            self.assertEqual(result, ["Nvme3n1", "Nvme4n1"])
            execute.assert_called_once()

    def test_subsystem_namespace_and_listener_are_idempotent(self):
        creator = _creator()
        subsystem = {
            "nqn": "nqn.2016-06.io.spdk:cnode1",
            "namespaces": [{"bdev_name": "Nvme0n1"}],
            "listen_addresses": [{"trtype": "RDMA", "traddr": "10.0.0.1", "trsvcid": "4420"}],
        }
        with patch.object(creator, "_get_subsystems", return_value=[subsystem]):
            self.assertEqual(creator._ensure_subsystem(None, "/spdk"), subsystem["nqn"])
        with patch.object(creator, "_find_subsystem", return_value=subsystem), patch.object(
            creator, "_execute_command"
        ) as execute:
            creator._add_namespaces(None, "/spdk", subsystem["nqn"], ["Nvme0n1", "Nvme1n1"])
            execute.assert_called_once()
            creator._add_listener(None, "/spdk", subsystem["nqn"], "10.0.0.1")
            execute.assert_called_once()

    def test_listener_tolerates_already_exists_only(self):
        creator = _creator()
        with patch.object(creator, "_find_subsystem", return_value={}), patch.object(
            creator, "_execute_command", side_effect=RuntimeError("already exists")
        ):
            creator._add_listener(None, "/spdk", "nqn", "10.0.0.1")
        with patch.object(creator, "_find_subsystem", return_value={}), patch.object(
            creator, "_execute_command", side_effect=RuntimeError("bad request")
        ):
            with self.assertRaisesRegex(RuntimeError, "bad request"):
                creator._add_listener(None, "/spdk", "nqn", "10.0.0.1")

    def test_deploy_target_runs_workflow_and_closes_ssh(self):
        creator = _creator()
        ssh = MagicMock()
        with patch.object(creator, "_ssh_connect", return_value=ssh), patch.object(
            creator, "_setup_spdk"
        ), patch.object(creator, "_filter_spdk_ready_pci_devices", return_value=["0000:01:00.0"]), patch.object(
            creator, "_is_spdk_tgt_running", return_value=True
        ), patch.object(creator, "_ensure_transport"), patch.object(
            creator, "_create_bdevs", return_value=["Nvme0n1"]
        ), patch.object(creator, "_ensure_subsystem", return_value="nqn"), patch.object(
            creator, "_add_namespaces"
        ), patch.object(creator, "_add_listener"):
            self.assertTrue(creator.deploy_target({"ip": "10.0.0.1", "path": "/spdk", "pci_devices": ["0000:01:00.0"]}))
            ssh.close.assert_called_once()

    def test_deploy_target_returns_false_and_deploy_all_aggregates(self):
        creator = _creator(["ip:10.0.0.1 path:/spdk", "ip:10.0.0.2 path:/spdk"])
        with patch.object(creator, "_ssh_connect", side_effect=RuntimeError("offline")):
            self.assertFalse(creator.deploy_target(creator.target_configs[0]))
        with patch.object(creator, "deploy_target", side_effect=[True, False]):
            self.assertFalse(creator.deploy_all_targets())


class MooncakeNoFRegisterTest(unittest.TestCase):
    def _new(self):
        obj = MooncakeNoFRegister.__new__(MooncakeNoFRegister)
        obj.cli_config = {}
        obj.spdk_targets = []
        obj.config_list = []
        obj.register = None
        return obj

    def test_constructor_validates_input_and_applies_overrides(self):
        with self.assertRaisesRegex(ValueError, "spdk_target_info"):
            MooncakeNoFRegister({}, [])
        configs = [{"nqn": "nqn", "nsid": 1, "traddr": "ip", "trsvcid": 4420, "size": 1}]
        with patch.object(MooncakeNoFRegister, "_get_remote_ssd_info", return_value=configs):
            obj = MooncakeNoFRegister({"master_server_address": "master", "nsid": "2"}, ["target"])
            self.assertEqual(obj.config_list[0]["nsid"], 2)

    def test_parse_and_match_pci(self):
        obj = self._new()
        target = obj._parse_spdk_target_info("ip:10.0.0.1 path:/spdk pci:0000:01:00.0")
        selected = obj._parse_selected_pci_devices(target)
        self.assertEqual(selected, ["0000:01:00.0"])
        self.assertTrue(obj._bdev_matches_selected_pci({"pci": "01:00.0"}, selected))
        self.assertFalse(obj._bdev_matches_selected_pci({"pci": "02:00.0"}, selected))
        self.assertTrue(obj._bdev_matches_selected_pci({}, []))
        with self.assertRaisesRegex(ValueError, "No valid PCI"):
            obj._parse_selected_pci_devices({"pci": "invalid"})

    def test_remove_duplicate_configs(self):
        obj = self._new()
        cfg = {"nqn": "nqn", "nsid": 1, "traddr": "ip", "trsvcid": 4420}
        obj.config_list = [cfg, dict(cfg)]
        obj._remove_duplicate_ssds()
        self.assertEqual(len(obj.config_list), 1)

    def test_get_remote_ssd_info_builds_geometry_and_filters_pci(self):
        obj = self._new()
        obj.spdk_targets = ["ip:10.0.0.1 path:/spdk pci:0000:01:00.0"]
        subsystem = [{
            "subtype": "NVMe",
            "nqn": "nqn",
            "listen_addresses": [{"traddr": "10.0.0.1", "trsvcid": "4420"}],
            "namespaces": [{"nsid": 1, "bdev_name": "Nvme0n1"}, {"nsid": 2, "bdev_name": "Nvme1n1"}],
        }]
        bdev_good = [{"name": "Nvme0n1", "block_size": 4096, "num_blocks": 10, "pci": "0000:01:00.0"}]
        bdev_other = [{"name": "Nvme1n1", "block_size": 4096, "num_blocks": 20, "pci": "0000:02:00.0"}]
        with patch.object(obj, "_execute_ssh_command", side_effect=[json.dumps(subsystem), json.dumps(bdev_good), json.dumps(bdev_other)]):
            configs = obj._get_remote_ssd_info("master")
        self.assertEqual(len(configs), 1)
        self.assertEqual(configs[0]["size"], 40960)

    def test_get_remote_ssd_info_rejects_empty_discovery(self):
        obj = self._new()
        obj.spdk_targets = ["bad", "ip:10.0.0.1 path:/spdk"]
        with patch.object(obj, "_execute_ssh_command", side_effect=RuntimeError("offline")):
            with self.assertRaisesRegex(RuntimeError, "No SSD information"):
                obj._get_remote_ssd_info("master")

    def test_start_service_counts_success_existing_and_failure(self):
        obj = self._new()
        obj.config_list = [
            {"nqn": "nqn", "nsid": n, "traddr": "ip", "trsvcid": 4420, "base": 0, "size": 1, "master_server_address": "master"}
            for n in (1, 2, 3)
        ]
        register = MagicMock()
        register.real_register.side_effect = [0, RuntimeError("SEGMENT_ALREADY_EXISTS"), -1]
        with _fake_store(lambda: register):
            self.assertFalse(obj.start_ssd_service())
        self.assertEqual(register.real_register.call_count, 3)


class MooncakeNoFUnregisterTest(unittest.TestCase):
    def _new(self):
        obj = MooncakeNoFUnregister.__new__(MooncakeNoFUnregister)
        obj.cli_config = {}
        obj.spdk_targets = []
        obj.config_list = []
        obj.register = None
        return obj

    def test_parse_specific_default_and_discovered_namespaces(self):
        obj = self._new()
        obj.spdk_targets = ["ip:10.0.0.1 ns:2 nqn:custom port:4421"]
        self.assertEqual(obj._parse_spdk_targets("master")[0]["nsid"], 2)
        obj.spdk_targets = ["ip:10.0.0.1"]
        self.assertEqual(obj._parse_spdk_targets("master")[0]["nsid"], 1)
        obj.spdk_targets = ["ip:10.0.0.1 path:/spdk"]
        subsystems = [{
            "subtype": "NVMe", "nqn": "discovered",
            "listen_addresses": [{"traddr": "10.0.0.2", "trsvcid": "4420"}],
            "namespaces": [{"nsid": 3}],
        }]
        with patch.object(obj, "_execute_ssh_command", return_value=json.dumps(subsystems)):
            self.assertEqual(obj._parse_spdk_targets("master")[0]["nsid"], 3)

    def test_discovery_failure_does_not_fallback(self):
        obj = self._new()
        obj.spdk_targets = ["ip:10.0.0.1 path:/spdk"]
        with patch.object(obj, "_execute_ssh_command", side_effect=RuntimeError("offline")):
            with self.assertRaisesRegex(RuntimeError, "aborted"):
                obj._parse_spdk_targets("master")

    def test_start_unregister_counts_absent_and_failure(self):
        obj = self._new()
        obj.config_list = [
            {"nqn": "nqn", "nsid": n, "traddr": "ip", "trsvcid": 4420, "master_server_address": "master"}
            for n in (1, 2, 3)
        ]
        register = MagicMock()
        register.real_unregister_by_endpoint.side_effect = [0, RuntimeError("segment not found"), -1]
        with _fake_store(lambda: register):
            self.assertFalse(obj.start_ssd_unregister_service())
        self.assertEqual(register.real_unregister_by_endpoint.call_count, 3)


if __name__ == "__main__":
    unittest.main()
