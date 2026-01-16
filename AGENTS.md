# Repository Guidelines

## Project Structure & Module Organization
- `boards/shields/`: Shield definitions, overlays, keymaps, and shield Kconfig files (e.g., `boards/shields/keyball39/keyball39.keymap`).
- `modules/keyball39_pmw3360/`: Custom Zephyr module with driver source, Kconfig, and DTS bindings.
- `config/west.yml`: West manifest for pulling ZMK and extra modules.
- `build.yaml`: CI build matrix for board/shield combos.
- `Trackball_circuit.*`: Hardware reference assets.

## Build, Test, and Development Commands
These commands assume this repo is the West workspace `config/` folder.
- `west update`: Syncs ZMK and module dependencies from `config/west.yml`.
- `west build -s zmk/app -d build/keyball39_left -b nice_nano -- -DSHIELD=keyball39_left`: Build left half firmware.
- `west build -s zmk/app -d build/keyball39_right -b nice_nano -- -DSHIELD=keyball39_right`: Build right half firmware.
- `west build -t pristine`: Cleans a build directory (run inside `build/...`).

## Coding Style & Naming Conventions
- Device tree overlays and keymaps follow ZMK style; keep 4-space indentation and trailing semicolons.
- File naming mirrors shield names: `keyball39_left.overlay`, `keyball39_right.conf`, `keyball39.keymap`.
- Match existing formatting for Kconfig and C sources in `modules/keyball39_pmw3360/`.

## Testing Guidelines
- There is no dedicated test suite here; validation is done by successful `west build` for each target shield.
- If you change keymaps or overlays, rebuild the affected shields listed in `build.yaml`.

## Commit & Pull Request Guidelines
- Commit messages follow Conventional Commits, e.g., `feat: update keymap for keyball39`.
- PRs should describe the target board/shield, the files touched, and how you validated (e.g., which `west build` commands you ran).

## Configuration Tips
- Shield configs live under `boards/shields/<shield>/`; keep left/right variants in sync where applicable.
- When adding a new build target, update `build.yaml` and include a matching `-DSHIELD=...` example in documentation.

## Language
- use Japanese Answers