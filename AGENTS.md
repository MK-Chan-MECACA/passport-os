# AI maintenance entry point

This is the maintenance copy of Passport OS MY, the Malaysia edition of Passport OS. Read this file and `docs/CURRENT.md` by default, then locate the source for the task. Read `docs/MAINTENANCE.md` only when building or flashing. The edition scope is `docs/MY-EDITION.md`.

- Changes should stay small and within MY-EDITION.md. Do not add features, refactor the whole project or reopen settled decisions unless the user asks.
- The working copy is `~/Claude Code/Passport OS MY` (`~/passport-os` is a symlink to it). Push to `main` on remote `my` (`MK-Chan-MECACA/passport-os-my`). Do not edit the upstream baseline worktree in `~/.cache/passport-baseline`.
- Do not read `managed_components/`, `build*/`, generated fonts, large test fixtures or `docs/archive/` by default. Search the relevant module with `rg`.
- `main/`, `components/` and `bootloader_components/` are the complete current source. Keep generated resources and third-party licences.
- Keep other uncommitted changes. Upgrades keep the configuration, identity, card and storage partitions by default. When the user explicitly asks for a fresh install or full wipe, back up first, then the whole chip may be erased; follow the maintenance guide.
- Functional tests are not current-draw or battery-life acceptance. Serial-simulated buttons are not physical-button or real-microphone acceptance.
- Run the relevant tests after a change. When delivering firmware, run the full host regression, the ESP-IDF build and the firmware layout/SRAM checks.
- To update status, edit only the necessary entries in `docs/CURRENT.md`; do not add a new document per conversation. Put detailed temporary logs in the ignored `output/`; the next session does not need to read them.
- All user-visible text must be English and drawable by the Latin-only fonts (`tests/check_fonts.py` enforces this).
