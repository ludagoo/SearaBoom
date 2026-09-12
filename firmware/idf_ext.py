# SPDX-FileCopyrightText: SearaBoom
# Gate idf.py flash* so signed images cannot be USB-written until Lucas
# confirms the signing key is stored. Build/reconfigure/monitor stay open.
import os
import sys

_SCRIPTS = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "scripts"))
if _SCRIPTS not in sys.path:
    sys.path.insert(0, _SCRIPTS)
from signing_key_backup import FLASH_IDF_ACTIONS, SigningKeyBackupError, require_backup


def action_extensions(base_actions, project_path=None):
    def gate_signed_flash(ctx, global_args, tasks):
        names = [getattr(t, "name", "") for t in tasks]
        if any(n in FLASH_IDF_ACTIONS for n in names):
            try:
                require_backup()
            except SigningKeyBackupError as exc:
                print(exc, file=sys.stderr)
                sys.exit(3)

    return {"global_action_callbacks": [gate_signed_flash]}
