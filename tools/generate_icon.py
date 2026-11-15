"""Vector source for dde's teal document-lines badge."""

import sys
from pathlib import Path

sys.dont_write_bytecode = True
from iconlib import Canvas, generate

ROOT = Path(__file__).resolve().parents[1]


def draw(canvas: Canvas) -> None:
    """Keep the teal disc and pale ring around three clear text lines."""
    teal, white = (64, 177, 158), (246, 251, 250)
    canvas.circle(50, 50, 48, teal)
    canvas.circle(50, 50, 42, white)
    canvas.circle(50, 50, 38, teal)
    canvas.line(((30, 34), (70, 34)), 7, white)
    canvas.line(((38, 50), (66, 50)), 7, white)
    canvas.line(((30, 66), (62, 66)), 7, white)


if __name__ == "__main__":
    raise SystemExit(generate(draw, ROOT / "src/res/app.ico"))
