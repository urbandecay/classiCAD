#!/usr/bin/env python3
"""Audit source-layer dependencies and production implementation includes."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src"
INCLUDE_PATTERN = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]')

# Transitional model.h remains a public compatibility umbrella while callers
# migrate to the owning service contracts.
UPWARD_INCLUDE_EXCEPTIONS = {
    (
        "core/model.h",
        "services/sampling/curve_sample_data.h",
        "model.h forwards the screen-sample compatibility contract during R2.",
    ),
    (
        "core/model.h",
        "services/snapping/snap_types.h",
        "model.h forwards the snap-value compatibility contract during R2.",
    ),
}

CPP_INCLUDE_EXCEPTIONS: set[tuple[str, str, str]] = set()

FORBIDDEN_IMPORTS = {
    "core/geometry": ("services/", "tools/", "ui/"),
    "core/document": ("services/", "tools/", "ui/"),
    "core/history": ("services/", "tools/", "ui/"),
    "core/layers": ("services/", "tools/", "ui/"),
    "core/serialization": ("services/", "tools/", "ui/"),
    "services": ("tools/", "ui/"),
    "tools": ("ui/",),
}


def source_relative(path: Path) -> str:
    return path.relative_to(SOURCE).as_posix()


def resolve_project_include(source_file: Path, include_name: str) -> Path | None:
    candidates = (source_file.parent / include_name, SOURCE / include_name)
    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved.is_file():
            try:
                resolved.relative_to(SOURCE)
            except ValueError:
                continue
            return resolved
    return None


def layer_for(source_path: str) -> tuple[str, tuple[str, ...]] | None:
    parts = source_path.split("/")
    if len(parts) < 2 or parts[0] != "core":
        return None
    category = "/".join(parts[:2])
    if category in FORBIDDEN_IMPORTS:
        return category, FORBIDDEN_IMPORTS[category]
    if parts[0] == "core":
        return "core", ("services/", "tools/", "ui/")
    if parts[0] in FORBIDDEN_IMPORTS:
        return parts[0], FORBIDDEN_IMPORTS[parts[0]]
    return None


def main() -> int:
    errors: list[str] = []
    files = sorted(path for path in (SOURCE / ".").rglob("*")
                   if path.is_file() and path.suffix in {".h", ".hpp", ".cpp"})
    files += sorted(path for path in (ROOT / "tests").rglob("*.cpp")
                    if path.is_file())

    for file_path in files:
        in_source = file_path.is_relative_to(SOURCE)
        relative_path = (source_relative(file_path) if in_source
                         else file_path.relative_to(ROOT).as_posix())
        rule = layer_for(relative_path) if in_source else None

        for line_number, line in enumerate(
                file_path.read_text(encoding="utf-8").splitlines(), start=1):
            match = INCLUDE_PATTERN.match(line)
            if not match:
                continue
            include_name = match.group(1)
            if include_name.endswith(".cpp"):
                exception = next((entry for entry in CPP_INCLUDE_EXCEPTIONS
                                  if entry[0] == relative_path
                                  and entry[1] == include_name), None)
                if exception is None:
                    errors.append(
                        f"{relative_path}:{line_number}: implementation include "
                        f"'{include_name}' is not allowed")

            imported = resolve_project_include(file_path, include_name)
            if imported is None:
                continue
            imported_path = source_relative(imported)
            if rule is not None and any(
                    imported_path.startswith(prefix) for prefix in rule[1]):
                exception = next((entry for entry in UPWARD_INCLUDE_EXCEPTIONS
                                  if entry[0] == relative_path
                                  and entry[1] == imported_path), None)
                if exception is None:
                    errors.append(
                        f"{relative_path}:{line_number}: {rule[0]} must not include "
                        f"upward dependency '{imported_path}'")

    if errors:
        print("Dependency audit failed:")
        print("\n".join(f"  {error}" for error in errors))
        return 1

    print(f"Dependency audit passed ({len(files)} source files checked).")
    print("Allowed temporary includes:")
    for file_path, include_name, reason in sorted(
            CPP_INCLUDE_EXCEPTIONS | UPWARD_INCLUDE_EXCEPTIONS):
        print(f"  {file_path} -> {include_name}: {reason}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
