#!/usr/bin/env python3
"""Bake UltimateXR's skeletal Wave pose into basketball-only UXRH meshes."""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path


def load_converter(path: Path):
    spec = importlib.util.spec_from_file_location("ultimatexr_converter", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load converter: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ultimatexr", type=Path, required=True)
    parser.add_argument("--gltf-dir", type=Path, required=True)
    parser.add_argument("--converter", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    converter = load_converter(args.converter)
    hands = args.ultimatexr / "Runtime" / "Art" / "Avatars" / "BigHands"
    poses = hands / "HandPoses"
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for side in ("Left", "Right"):
        converter.convert_hand(
            args.gltf_dir / f"BigHand{side}_out" / f"BigHand{side}.gltf",
            poses / "Wave.asset",
            poses / "Grab.asset",
            side,
            args.output_dir / f"BigHand{side}Palm.uxrh",
        )


if __name__ == "__main__":
    main()
