# SPDX-FileCopyrightText: SearaBoom
# Gate idf.py flash* so signed images cannot be USB-written until Lucas
# confirms the signing key is stored. Build/reconfigure/monitor stay open.
#
# idf.py swallows import errors in this file ("Skipping"). Keep this module
# loadable and fail closed on flash if signing_key_backup cannot be imported.
import os
import sys

_SCRIPTS = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "scripts"))
if _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)

_IMPORT_ERR = None
FLASH_IDF_ACTIONS = frozenset(
    {
        "flash",
        "app-flash",
        "bootloader-flash",
        "encrypted-flash",
        "encrypted-app-flash",
        "partition-table-flash",
    }
)
try:
    from signing_key_backup import FLASH_IDF_ACTIONS, SigningKeyBackupError, require_backup
except Exception as exc:  # noqa: BLE001 — fail closed on flash, never skip the gate
    _IMPORT_ERR = exc
    SigningKeyBackupError = RuntimeError

    def require_backup():
        raise SigningKeyBackupError(f"signing_key_backup import failed: {_IMPORT_ERR}")


def action_extensions(base_actions, project_path=None):
    def gate_signed_flash(ctx, global_args, tasks):
        names = [getattr(t, "name", "") for t in tasks]
        if not any(n in FLASH_IDF_ACTIONS for n in names):
            return
        if _IMPORT_ERR is not None:
            print(
                "Refusing signed USB flash: firmware/idf_ext.py could not import "
                f"signing_key_backup ({_IMPORT_ERR}). See docs/SIGNED_FIRMWARE.md",
                file=sys.stderr,
            )
            sys.exit(3)
        try:
            require_backup()
        except SigningKeyBackupError as exc:
            print(exc, file=sys.stderr)
            sys.exit(3)

    return {"global_action_callbacks": [gate_signed_flash]}
