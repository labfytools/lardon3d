#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import re
import sys
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

EXTENSIONS = {".jpg", ".jpeg", ".png", ".tif", ".tiff", ".webp"}


@dataclass
class Result:
    path: Path
    category: str
    sharpness: float
    white: float
    black: float
    brightness: float
    reason: str


def natural_key(path: Path) -> list[object]:
    return [
        int(part) if part.isdigit() else part.lower()
        for part in re.split(r"(\d+)", path.name)
    ]


def load_gray(path: Path) -> np.ndarray:
    with Image.open(path) as source:
        source = source.convert("L")
        gray = np.asarray(source)

    height, width = gray.shape
    maximum_dimension = 1600

    if max(width, height) > maximum_dimension:
        scale = maximum_dimension / max(width, height)
        gray = cv2.resize(
            gray,
            (
                max(1, round(width * scale)),
                max(1, round(height * scale)),
            ),
            interpolation=cv2.INTER_AREA,
        )

    return gray


def analyze(path: Path) -> tuple[dict[str, float] | None, str]:
    try:
        gray = load_gray(path)
    except Exception as error:
        return None, f"illisible: {error}"

    return {
        "sharpness": float(cv2.Laplacian(gray, cv2.CV_64F).var()),
        "white": float(np.count_nonzero(gray >= 250) / gray.size * 100),
        "black": float(np.count_nonzero(gray <= 5) / gray.size * 100),
        "brightness": float(gray.mean()),
    }, ""


def classify(path: Path, metrics: dict[str, float] | None, error: str) -> Result:
    if metrics is None:
        return Result(path, "mauvaises", 0.0, 0.0, 0.0, 0.0, error)

    sharpness = metrics["sharpness"]
    white = metrics["white"]
    black = metrics["black"]
    brightness = metrics["brightness"]

    reasons: list[str] = []

    if sharpness < 100:
        reasons.append("très floue")
    elif sharpness < 170:
        reasons.append("floue possible")

    if white > 20:
        reasons.append("très surexposée")
    elif white > 8:
        reasons.append("surexposition possible")

    if black > 35:
        reasons.append("très sombre")
    elif black > 15:
        reasons.append("ombres bouchées possibles")

    if brightness > 225 or brightness < 25:
        reasons.append("luminosité extrême")

    severe = (
        sharpness < 100
        or white > 20
        or black > 35
        or brightness > 225
        or brightness < 25
    )

    if severe:
        category = "mauvaises"
    elif reasons:
        category = "suspectes"
    else:
        category = "bonnes"

    return Result(
        path,
        category,
        sharpness,
        white,
        black,
        brightness,
        "; ".join(reasons) or "ok",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("images", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    images_dir = args.images.expanduser().resolve()
    output_dir = args.output.expanduser().resolve()

    excluded_roots = {
        output_dir,
        images_dir / "scan3d",
        images_dir / "tri_resultat",
        images_dir / "test_colmap",
        images_dir / "colmap_complet",
    }

    images: list[Path] = []
    for path in images_dir.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        if path.suffix.lower() not in EXTENSIONS:
            continue
        if any(root == path or root in path.parents for root in excluded_roots):
            continue
        images.append(path)

    images.sort(key=natural_key)

    if not images:
        print("Aucune image trouvée.", file=sys.stderr)
        return 1

    print(f"{len(images)} images trouvées.")

    results: list[Result] = []
    for index, path in enumerate(images, 1):
        metrics, error = analyze(path)
        results.append(classify(path, metrics, error))
        print(
            f"\rAnalyse {index}/{len(images)} : {path.name[:55]:55s}",
            end="",
            flush=True,
        )
    print()

    output_dir.mkdir(parents=True, exist_ok=True)
    for category in ("bonnes", "suspectes", "mauvaises"):
        category_dir = output_dir / category
        if category_dir.exists():
            for child in category_dir.iterdir():
                if child.is_symlink() or child.is_file():
                    child.unlink()
        category_dir.mkdir(parents=True, exist_ok=True)

    with (output_dir / "resultats.csv").open(
        "w", encoding="utf-8", newline=""
    ) as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(
            [
                "fichier",
                "categorie",
                "netteté",
                "blanc_pct",
                "noir_pct",
                "luminosité",
                "raison",
            ]
        )

        counters = {"bonnes": 0, "suspectes": 0, "mauvaises": 0}
        for result in results:
            counters[result.category] += 1
            link_name = f"{counters[result.category]:06d}_{result.path.name}"
            link_path = output_dir / result.category / link_name
            link_path.symlink_to(result.path.resolve())

            writer.writerow(
                [
                    str(result.path),
                    result.category,
                    f"{result.sharpness:.2f}",
                    f"{result.white:.2f}",
                    f"{result.black:.2f}",
                    f"{result.brightness:.2f}",
                    result.reason,
                ]
            )

    for category in ("bonnes", "suspectes", "mauvaises"):
        count = sum(result.category == category for result in results)
        print(f"{category}: {count}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
