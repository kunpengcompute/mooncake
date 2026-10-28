import logging
import sys
from types import ModuleType
from unittest.mock import Mock, patch

from mooncake.mooncake_ssd_tool_common import MooncakeNoFRegister


def config(nsid):
    return dict(
        nqn="nqn.test",
        nsid=nsid,
        traddr="127.0.0.1",
        trsvcid=4420,
        base=0,
        size=4096,
        master_server_address="master:50051",
    )


def test_configuration_log_counts_deduplicated_namespaces(monkeypatch, caplog):
    monkeypatch.setattr(
        MooncakeNoFRegister,
        "_get_remote_ssd_info",
        lambda self, address: [config(1), config(1), config(2)],
    )
    with caplog.at_level(logging.INFO):
        register = MooncakeNoFRegister(
            {"master_server_address": "master:50051"}, ["ip:127.0.0.1 path:/spdk"]
        )
    assert len(register.config_list) == 2
    assert "Loaded 2 registerable namespace configuration(s)" in caplog.text


def test_registration_logs_namespace_identity_and_outcome_counts(caplog):
    native = ModuleType("mooncake.store")
    clients = [Mock(), Mock(), Mock()]
    clients[0].real_register.return_value = 0
    clients[1].real_register.side_effect = RuntimeError("SEGMENT_ALREADY_EXISTS")
    clients[2].real_register.return_value = -1
    native.MooncakeDistributedNoFRegister = Mock(side_effect=clients)
    register = MooncakeNoFRegister.__new__(MooncakeNoFRegister)
    register.config_list = [config(1), config(2), config(3)]
    with patch.dict(sys.modules, {"mooncake.store": native}), caplog.at_level(
        logging.INFO
    ):
        assert not register.start_ssd_service()
    assert (
        "Registering namespace 1/3: nqn=nqn.test, nsid=1, traddr=127.0.0.1"
        in caplog.text
    )
    assert (
        "Namespace 2/3 (nqn=nqn.test, nsid=2, traddr=127.0.0.1) already registered"
        in caplog.text
    )
    assert "Register namespace 1/3 succeeded" in caplog.text
    assert "Failed to register namespace 3/3" in caplog.text
    assert "Namespace registration summary:" in caplog.text
    assert "Registerable namespaces: 3" in caplog.text
    assert "Successfully registered namespaces: 1" in caplog.text
    assert "Already registered namespaces (skipped): 1" in caplog.text
    assert "Failed namespaces: 1" in caplog.text
    assert native.MooncakeDistributedNoFRegister.call_count == 3
