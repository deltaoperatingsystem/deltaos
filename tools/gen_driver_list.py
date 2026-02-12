#!/usr/bin/env python3
import tomllib
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
KERNEL_DIR = PROJECT_ROOT / "kernel"
DRIVER_DIR = KERNEL_DIR / "drivers"

# Load registry
registry_path = DRIVER_DIR / "registry.toml"
if not registry_path.exists():
    raise SystemExit(f"Error: registry.toml not found at {registry_path}")

registry = tomllib.loads(registry_path.read_text())["drivers"]

# Scan for all .c files under kernel/drivers/
#we want paths relative to KERNEL_DIR for the Makefile
driver_files = {
    f.stem: f.relative_to(DRIVER_DIR)
    for f in DRIVER_DIR.rglob("*.c")
}

# Match registry keys to discovered files
enabled_sources = []
enabled_names = []
for name, enabled in registry.items():
    if enabled:
        if name not in driver_files:
            raise SystemExit(f"Error: driver '{name}' not found in drivers directory")
        enabled_sources.append(str(driver_files[name]))
        enabled_names.append(name)

#write makefile fragment
mk_out = DRIVER_DIR / "drivers_enabled.mk"
with mk_out.open("w") as f:
    f.write("# Auto-generated. Do not edit.\n")
    f.write("DRIVERS := " + " ".join(enabled_sources) + "\n")
