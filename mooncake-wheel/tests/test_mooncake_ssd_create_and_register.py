import unittest
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

from mooncake import mooncake_ssd_create_and_register as cli


def _args(**overrides):
    values = {
        "master_server_address": "master:50051",
        "spdk_target_info": ["ip:10.0.0.1 path:/spdk"],
        "skip_create": False,
        "skip_register": False,
        "dry_run": False,
        "core_mask": "0xff",
        "transport_type": "RDMA",
        "max_queue_depth": 128,
        "max_io_qpairs_per_ctrlr": 127,
        "max_io_size": 4096,
        "in_capsule_data_size": 131072,
        "io_unit_size": 131072,
        "max_aq_depth": 128,
        "num_shared_buffers": 4096,
        "buf_cache_size": 32,
        "username": "root",
        "port": 22,
        "password": None,
        "key_file": None,
        "define": [],
    }
    values.update(overrides)
    return SimpleNamespace(**values)


class CreateAndRegisterCliTest(unittest.TestCase):
    def test_run_phase_handles_dry_run_success_and_failure(self):
        action = MagicMock(return_value=True)
        self.assertTrue(cli._run_phase("phase", True, action))
        action.assert_not_called()
        self.assertTrue(cli._run_phase("phase", False, action))
        self.assertFalse(cli._run_phase("phase", False, MagicMock(return_value=False)))

    def test_transport_options_maps_every_field(self):
        options = cli._transport_options(_args(transport_type="TCP", max_queue_depth=64))
        self.assertEqual(options["trtype"], "TCP")
        self.assertEqual(options["max_queue_depth"], 64)
        self.assertEqual(options["buf_cache_size"], 32)

    def test_register_config_applies_credentials_and_valid_defines(self):
        config = cli._register_cli_config(
            _args(password="secret", key_file="key", define=["trsvcid=4421", "invalid"])
        )
        self.assertEqual(config["password"], "secret")
        self.assertEqual(config["key_file"], "key")
        self.assertEqual(config["trsvcid"], "4421")
        self.assertNotIn("invalid", config)

    def test_main_rejects_skipping_both_phases(self):
        with patch.object(cli, "parse_arguments", return_value=_args(skip_create=True, skip_register=True)):
            with self.assertRaisesRegex(SystemExit, "1"):
                cli.main()

    def test_main_runs_create_and_register(self):
        creator = MagicMock()
        creator.deploy_all_targets.return_value = True
        register = MagicMock()
        register.start_ssd_service.return_value = True
        with patch.object(cli, "parse_arguments", return_value=_args()), patch.object(
            cli, "SPDKTgtCreator", return_value=creator
        ) as creator_type, patch.object(cli, "MooncakeNoFRegister", return_value=register) as register_type:
            cli.main()
        creator.deploy_all_targets.assert_called_once()
        register.start_ssd_service.assert_called_once()
        creator_type.assert_called_once()
        register_type.assert_called_once()

    def test_main_stops_after_create_failure(self):
        creator = MagicMock()
        creator.deploy_all_targets.return_value = False
        with patch.object(cli, "parse_arguments", return_value=_args()), patch.object(
            cli, "SPDKTgtCreator", return_value=creator
        ), patch.object(cli, "MooncakeNoFRegister") as register_type:
            with self.assertRaisesRegex(SystemExit, "1"):
                cli.main()
        register_type.assert_not_called()

    def test_main_supports_each_phase_and_dry_run(self):
        register = MagicMock()
        register.start_ssd_service.return_value = True
        with patch.object(cli, "parse_arguments", return_value=_args(skip_create=True)), patch.object(
            cli, "MooncakeNoFRegister", return_value=register
        ):
            cli.main()
        with patch.object(cli, "parse_arguments", return_value=_args(skip_register=True)), patch.object(
            cli, "SPDKTgtCreator"
        ) as creator_type:
            creator_type.return_value.deploy_all_targets.return_value = True
            cli.main()
        with patch.object(cli, "parse_arguments", return_value=_args(dry_run=True)), patch.object(
            cli, "SPDKTgtCreator"
        ) as creator_type, patch.object(cli, "MooncakeNoFRegister") as register_type:
            cli.main()
            creator_type.assert_not_called()
            register_type.assert_not_called()


if __name__ == "__main__":
    unittest.main()
