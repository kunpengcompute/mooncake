import json
import unittest
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

from mooncake import mooncake_ssd_unregister_and_remove as cli


def _args(**overrides):
    values = {
        "master_server_address": "master:50051",
        "spdk_target_info": ["ip:10.0.0.1 path:/spdk ns:1 nqn:nqn"],
        "target_only": False,
        "remove_target_namespace": False,
        "detach_bdev": False,
        "dry_run": False,
        "username": "root",
        "port": 22,
        "password": None,
        "key_file": None,
        "define": [],
    }
    values.update(overrides)
    return SimpleNamespace(**values)


class NamespaceRemoverTest(unittest.TestCase):
    def test_parse_helpers(self):
        self.assertEqual(cli._parse_target_info("ip:10.0.0.1 ns:2")["ns"], "2")
        self.assertEqual(cli._ctrlr_name_from_bdev("Nvme12n1"), "Nvme12")
        self.assertEqual(cli._ctrlr_name_from_bdev("Aio0"), "Aio0")

    def test_find_spdk_path_checks_both_locations(self):
        remover = cli.SPDKNamespaceRemover(_args())
        ssh = MagicMock()
        first = MagicMock()
        first.channel.recv_exit_status.return_value = 1
        second = MagicMock()
        second.channel.recv_exit_status.return_value = 0
        ssh.exec_command.side_effect = [(None, first, None), (None, second, None)]
        self.assertEqual(remover._find_spdk_path(ssh, "/root"), "/root/spdk")
        ssh.exec_command.side_effect = [(None, first, None), (None, first, None)]
        with self.assertRaisesRegex(RuntimeError, "Could not find"):
            remover._find_spdk_path(ssh, "/root")

    def test_exec_rpc_supports_dry_run_success_and_error(self):
        remover = cli.SPDKNamespaceRemover(_args(dry_run=True))
        self.assertEqual(remover._exec_rpc(None, "/spdk", "get"), "[]")
        remover.args.dry_run = False
        ssh = MagicMock()
        stdout = MagicMock()
        stdout.channel.recv_exit_status.return_value = 0
        stdout.read.return_value = b"ok"
        stderr = MagicMock()
        stderr.read.return_value = b""
        ssh.exec_command.return_value = (None, stdout, stderr)
        self.assertEqual(remover._exec_rpc(ssh, "/spdk", "get"), "ok")
        stdout.channel.recv_exit_status.return_value = 1
        stderr.read.return_value = b"bad"
        with self.assertRaisesRegex(RuntimeError, "bad"):
            remover._exec_rpc(ssh, "/spdk", "get")

    def test_ignore_absent_only_suppresses_expected_errors(self):
        remover = cli.SPDKNamespaceRemover(_args())
        with patch.object(remover, "_exec_rpc", side_effect=RuntimeError("not found")):
            self.assertEqual(remover._exec_rpc_ignore_absent(None, "/spdk", "cmd", ["not found"]), "")
        with patch.object(remover, "_exec_rpc", side_effect=RuntimeError("permission")):
            with self.assertRaisesRegex(RuntimeError, "permission"):
                remover._exec_rpc_ignore_absent(None, "/spdk", "cmd", ["not found"])

    def test_list_namespaces_filters_subsystem_and_nsid(self):
        remover = cli.SPDKNamespaceRemover(_args())
        subsystems = [
            {"subtype": "Discovery"},
            {"subtype": "NVMe", "nqn": "other", "namespaces": [{"nsid": 1}]},
            {"subtype": "NVMe", "nqn": "nqn", "namespaces": [
                {"nsid": 1, "bdev_name": "Nvme0n1"}, {"nsid": 2, "name": "Nvme1n1"}, {"nsid": 0}
            ]},
        ]
        with patch.object(remover, "_exec_rpc", return_value=json.dumps(subsystems)):
            result = remover._list_namespaces(None, "/spdk", {"nqn": "nqn", "ns": "2"})
        self.assertEqual(result, [{"nqn": "nqn", "nsid": 2, "bdev_name": "Nvme1n1"}])
        remover.args.dry_run = True
        self.assertEqual(remover._list_namespaces(None, "/spdk", {"ns": "3"})[0]["nsid"], 3)

    def test_remove_target_validates_dry_run_and_empty_match(self):
        remover = cli.SPDKNamespaceRemover(_args())
        self.assertFalse(remover.remove_for_target("ip:10.0.0.1"))
        remover.args.dry_run = True
        remover.args.detach_bdev = True
        self.assertTrue(remover.remove_for_target("ip:10.0.0.1 path:/spdk ns:1"))
        remover.args.dry_run = False
        ssh = MagicMock()
        with patch.object(remover, "_connect", return_value=ssh), patch.object(
            remover, "_find_spdk_path", return_value="/spdk"
        ), patch.object(remover, "_list_namespaces", return_value=[]):
            self.assertTrue(remover.remove_for_target("ip:10.0.0.1 path:/spdk"))
            ssh.close.assert_called_once()

    def test_remove_target_removes_namespace_and_detaches_bdev(self):
        remover = cli.SPDKNamespaceRemover(_args(detach_bdev=True, remove_target_namespace=True))
        ssh = MagicMock()
        namespaces = [{"nqn": "nqn", "nsid": 1, "bdev_name": "Nvme0n1"}]
        with patch.object(remover, "_connect", return_value=ssh), patch.object(
            remover, "_find_spdk_path", return_value="/spdk"
        ), patch.object(remover, "_list_namespaces", return_value=namespaces), patch.object(
            remover, "_exec_rpc"
        ) as rpc, patch.object(remover, "_exec_rpc_ignore_absent") as ignore:
            self.assertTrue(remover.remove_for_target("ip:10.0.0.1 path:/spdk"))
            self.assertIn("remove_ns", rpc.call_args.args[2])
            self.assertIn("detach_controller", ignore.call_args.args[2])

    def test_remove_target_returns_false_on_exception_and_remove_all_aggregates(self):
        remover = cli.SPDKNamespaceRemover(_args())
        ssh = MagicMock()
        with patch.object(remover, "_connect", return_value=ssh), patch.object(
            remover, "_find_spdk_path", side_effect=RuntimeError("bad")
        ):
            self.assertFalse(remover.remove_for_target("ip:10.0.0.1 path:/spdk"))
        remover.args.spdk_target_info = ["one", "two"]
        with patch.object(remover, "remove_for_target", side_effect=[False, True]):
            self.assertFalse(remover.remove_all())


class UnregisterCliTest(unittest.TestCase):
    def test_unregister_config_and_dry_run(self):
        args = _args(password="p", key_file="k", define=["trsvcid=4421", "bad"])
        config = cli._unregister_cli_config(args)
        self.assertEqual(config["password"], "p")
        self.assertEqual(config["key_file"], "k")
        self.assertEqual(config["trsvcid"], "4421")
        args.dry_run = True
        self.assertTrue(cli.run_unregister_phase(args))

    def test_argument_validation(self):
        with self.assertRaises(SystemExit):
            cli.parse_arguments(["--spdk_target_info", "ip:x path:/spdk"])
        with self.assertRaises(SystemExit):
            cli.parse_arguments(["--target-only", "--master_server_address", "m", "--spdk_target_info", "ip:x path:/spdk"])
        with self.assertRaises(SystemExit):
            cli.parse_arguments(["--target-only", "--spdk_target_info", "ip:x path:/spdk ns:0"])
        with self.assertRaises(SystemExit):
            cli.parse_arguments(["--master_server_address", "m", "--spdk_target_info", "ip:x path:/spdk", "--detach-bdev"])
        args = cli.parse_arguments(["--target-only", "--spdk_target_info", "ip:x path:/spdk"])
        self.assertTrue(args.remove_target_namespace)

    def test_main_runs_master_and_target_phases(self):
        remover = MagicMock()
        remover.remove_all.return_value = True
        with patch.object(cli, "parse_arguments", return_value=_args(remove_target_namespace=True)), patch.object(
            cli, "run_unregister_phase", return_value=True
        ) as unregister, patch.object(cli, "SPDKNamespaceRemover", return_value=remover):
            cli.main()
        unregister.assert_called_once()
        remover.remove_all.assert_called_once()

    def test_main_exits_on_phase_failures(self):
        with patch.object(cli, "parse_arguments", return_value=_args()), patch.object(
            cli, "run_unregister_phase", return_value=False
        ):
            with self.assertRaisesRegex(SystemExit, "1"):
                cli.main()
        with patch.object(cli, "parse_arguments", return_value=_args(target_only=True, remove_target_namespace=True)), patch.object(
            cli, "SPDKNamespaceRemover"
        ) as remover_type:
            remover_type.return_value.remove_all.return_value = False
            with self.assertRaisesRegex(SystemExit, "1"):
                cli.main()


if __name__ == "__main__":
    unittest.main()
