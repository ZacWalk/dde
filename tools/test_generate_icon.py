"""Pixel regressions for dde's document-lines badge."""

import math
import unittest

from generate_icon import ROOT, draw
from iconlib import ICON_SIZES, Canvas, ico


class IconTests(unittest.TestCase):
    images: dict[int, bytes]

    @classmethod
    def setUpClass(cls) -> None:
        cls.images = {}
        for size in ICON_SIZES:
            canvas = Canvas(size)
            draw(canvas)
            cls.images[size] = canvas.rgba()

    def test_disc_and_ring_are_unchanged(self) -> None:
        for size, image in self.images.items():
            with self.subTest(size=size):
                badge = Canvas(size)
                badge.circle(50, 50, 48, (64, 177, 158))
                badge.circle(50, 50, 42, (246, 251, 250))
                badge.circle(50, 50, 38, (64, 177, 158))
                expected = badge.rgba()
                for y in range(size):
                    for x in range(size):
                        if math.hypot((x + 0.5) * 100 / size - 50,
                                      (y + 0.5) * 100 / size - 50) >= 32:
                            offset = (y * size + x) * 4
                            self.assertEqual(image[offset:offset + 4],
                                             expected[offset:offset + 4], (size, x, y))

    def test_three_separate_legible_text_lines(self) -> None:
        for size, image in self.images.items():
            with self.subTest(size=size):
                bands: list[list[tuple[int, list[int]]]] = []
                low, high = int(size * 0.27), math.ceil(size * 0.73)
                for y in range(low, high):
                    ink = [x for x in range(low, high)
                           if image[(y * size + x) * 4] >= 150
                           and image[(y * size + x) * 4 + 3] == 255]
                    if ink:
                        self.assertEqual(ink, list(range(ink[0], ink[-1] + 1)))
                        if not bands or y != bands[-1][-1][0] + 1:
                            bands.append([])
                        bands[-1].append((y, ink))
                self.assertEqual(len(bands), 3)
                spans = []
                for band, centre in zip(bands, (34, 50, 66)):
                    self.assertAlmostEqual((band[0][0] + band[-1][0] + 1) / 2,
                                           size * centre / 100, delta=0.75)
                    left = min(ink[0] for _, ink in band)
                    right = max(ink[-1] for _, ink in band)
                    self.assertGreaterEqual(right - left + 1, max(3, size * 0.22))
                    spans.append((left, right))
                for previous, following in zip(bands, bands[1:]):
                    self.assertGreater(following[0][0] - previous[-1][0], 1)
                self.assertGreater(spans[1][0], spans[0][0])
                self.assertLess(spans[2][1], spans[0][1])
                self.assertLessEqual(abs(spans[2][0] - spans[0][0]), 1)

    def test_resource_contains_all_generated_sizes(self) -> None:
        self.assertTrue((ROOT / "src" / "res" / "app.ico").read_bytes() == ico(self.images),
                        "Regenerate src/res/app.ico with tools\\generate_icon.py")


if __name__ == "__main__":
    unittest.main()
