from contextlib import ExitStack
import logging
from unittest.mock import Mock, patch

import pytest

from mooncake import mooncake_ssd_unregister_and_remove as remove
from mooncake.mooncake_ssd_tool_common import SPDKTgtCreator


TARGET = "ip:127.0.0.1 path:/spdk ns:1 nqn:nqn.test"


def parse(*extra):
    return remove.parse_arguments(["--spdk_target_info", TARGET, *extra])


def test_target_only_implies_namespace_removal_without_master():
    args = parse("--target-only", "--detach-bdev")
    assert args.target_only
    assert args.remove_target_namespace
    assert args.detach_bdev
    assert args.master_server_address is None


@pytest.mark.parametrize(
    "extra",
    [
        [],
        ["--target-only", "--master_server_address", "master:50051"],
        ["--target-only", "-Dtrsvcid=4420"],
        ["--master_server_address", "master:50051", "--detach-bdev"],
    ],
)
def test_conflicting_or_missing_mode_arguments_are_rejected(extra):
    with pytest.raises(SystemExit) as caught:
        parse(*extra)
    assert caught.value.code == 2


@pytest.mark.parametrize(
    "target",
    [
        "ip:127.0.0.1",
        "path:/spdk",
        "ip:127.0.0.1 path:/spdk ns:0",
        "ip:127.0.0.1 path:/spdk ns:-1",
        "ip:127.0.0.1 path:/spdk ns:invalid",
    ],
)
def test_target_only_rejects_incomplete_targets_and_invalid_namespace_ids(target):
    with pytest.raises(SystemExit):
        remove.parse_arguments(["--target-only", "--spdk_target_info", target])


def test_every_target_is_validated():
    with pytest.raises(SystemExit):
        remove.parse_arguments(
            [
                "--target-only",
                "--spdk_target_info",
                TARGET,
                "--spdk_target_info",
                "ip:bad",
            ]
        )


def test_target_only_never_runs_master_unregistration():
    args = parse("--target-only", "--detach-bdev")
    with (
        patch.object(remove, "parse_arguments", return_value=args),
        patch.object(remove, "run_unregister_phase") as unregister,
        patch.object(remove, "SPDKNamespaceRemover") as remover,
    ):
        remove.main()
        unregister.assert_not_called()
        remover.assert_called_once_with(args)
        remover.return_value.remove_all.assert_called_once_with()


def test_target_only_dry_run_needs_neither_ssh_nor_master():
    args = parse("--target-only", "--detach-bdev", "--dry-run")
    with (
        patch.object(remove, "parse_arguments", return_value=args),
        patch.object(remove, "run_unregister_phase") as unregister,
        patch.object(remove, "require_paramiko") as require,
    ):
        remove.main()
        unregister.assert_not_called()
        require.assert_not_called()


def test_target_cleanup_failure_is_reported():
    with (
        patch.object(remove, "parse_arguments", return_value=parse("--target-only")),
        patch.object(remove, "SPDKNamespaceRemover") as remover,
    ):
        remover.return_value.remove_all.return_value = False
        with pytest.raises(SystemExit) as caught:
            remove.main()
        assert caught.value.code == 1


def test_default_mode_still_unregisters_before_target_removal():
    args = parse("--master_server_address", "master:50051", "--remove-target-namespace")
    events = []
    with (
        patch.object(remove, "parse_arguments", return_value=args),
        patch.object(
            remove,
            "run_unregister_phase",
            side_effect=lambda _: events.append("master") or True,
        ),
        patch.object(
            remove.SPDKNamespaceRemover,
            "remove_all",
            side_effect=lambda: events.append("target") or True,
        ),
    ):
        remove.main()
    assert events == ["master", "target"]


def test_failed_master_unregistration_stops_target_removal():
    args = parse("--master_server_address", "master:50051", "--remove-target-namespace")
    with (
        patch.object(remove, "parse_arguments", return_value=args),
        patch.object(remove, "run_unregister_phase", return_value=False),
        patch.object(remove, "SPDKNamespaceRemover") as remover,
    ):
        with pytest.raises(SystemExit):
            remove.main()
        remover.assert_not_called()


def deploy_explicit_devices(driver_output):
    creator = SPDKTgtCreator(["ip:127.0.0.1 path:/spdk pci:0000:01:00.0,0000:02:00.0"])
    with ExitStack() as stack:
        ssh = stack.enter_context(
            patch.object(creator, "_ssh_connect", return_value=Mock())
        )
        stack.enter_context(patch.object(creator, "_setup_spdk"))
        stack.enter_context(
            patch.object(creator, "_execute_command", return_value=(driver_output, ""))
        )
        filtered = stack.enter_context(
            patch.object(
                creator,
                "_filter_spdk_ready_pci_devices",
                wraps=creator._filter_spdk_ready_pci_devices,
            )
        )
        stack.enter_context(
            patch.object(creator, "_is_spdk_tgt_running", return_value=True)
        )
        stack.enter_context(patch.object(creator, "_ensure_transport"))
        bdevs = stack.enter_context(
            patch.object(creator, "_create_bdevs", return_value=["Nvme0n1"])
        )
        stack.enter_context(
            patch.object(creator, "_ensure_subsystem", return_value="nqn.test")
        )
        stack.enter_context(patch.object(creator, "_add_namespaces"))
        stack.enter_context(patch.object(creator, "_add_listener"))
        success = creator.deploy_all_targets()
    return success, filtered, bdevs, ssh


def test_explicit_unavailable_devices_are_skipped_with_warning(caplog):
    with caplog.at_level(logging.INFO):
        success, filtered, bdevs, ssh = deploy_explicit_devices(
            "READY 0000:01:00.0 vfio-pci\nNOT_READY 0000:02:00.0 nvme\n"
        )
    assert success
    assert filtered.call_args.kwargs["strict"] is False
    assert bdevs.call_args.args[2] == ["0000:01:00.0"]
    assert "Skipping PCI devices not available to SPDK" in caplog.text
    assert "requested=2, available=1, skipped=1" in caplog.text
    ssh.return_value.close.assert_called_once()


def test_no_available_devices_still_fail_creation(caplog):
    with caplog.at_level(logging.INFO):
        success, _, bdevs, ssh = deploy_explicit_devices(
            "NOT_READY 0000:01:00.0 nvme\nNOT_READY 0000:02:00.0 nvme\n"
        )
    assert not success
    assert "requested=2, available=0, skipped=2" in caplog.text
    bdevs.assert_not_called()
    ssh.return_value.close.assert_called_once()
