import array
import os
import sys
import unittest


sys.path.insert(0, os.path.dirname(__file__))
import workbench_pfm_compare


WIDTH = 16
HEIGHT = 16
BACKGROUND = (0.1, 0.1, 0.1)
FOREGROUND = (0.8, 0.6, 0.4)


def rectangle_image(left=4, top=4, right=12, bottom=12,
                    edge_coverage=1.0, background=BACKGROUND,
                    foreground=FOREGROUND):
    pixels = array.array("f")
    for y in range(HEIGHT):
        for x in range(WIDTH):
            inside = left <= x < right and top <= y < bottom
            on_edge = inside and (x in (left, right - 1) or
                                  y in (top, bottom - 1))
            coverage = edge_coverage if on_edge else (1.0 if inside else 0.0)
            pixels.extend(background[channel] +
                          coverage * (foreground[channel] - background[channel])
                          for channel in range(3))
    return WIDTH, HEIGHT, pixels


class EdgeCoverageContracts(unittest.TestCase):
    def test_identical_hard_edge_has_zero_error(self):
        image = rectangle_image()
        profile = workbench_pfm_compare.edge_coverage(image, image, 0.02)
        self.assertIsNotNone(profile)
        self.assertTrue(profile["available"])
        self.assertEqual(profile["coverage_mae"], 0.0)
        self.assertEqual(profile["coverage_p90_abs_error"], 0.0)

    def test_coverage_ramp_is_detected_when_masks_match(self):
        hard = rectangle_image()
        antialiased = rectangle_image(edge_coverage=0.5)
        profile = workbench_pfm_compare.edge_coverage(hard, antialiased, 0.02)
        self.assertIsNotNone(profile)
        self.assertGreater(profile["coverage_mae"], 0.0)
        self.assertGreater(profile["candidate_partial_coverage_fraction"],
                           profile["reference_partial_coverage_fraction"])

    def test_one_pixel_shift_changes_coverage_profile(self):
        reference = rectangle_image()
        shifted = rectangle_image(left=5, right=13)
        profile = workbench_pfm_compare.edge_coverage(reference, shifted, 0.02)
        self.assertIsNotNone(profile)
        self.assertGreater(profile["coverage_mae"], 0.0)

    def test_background_offset_does_not_change_coverage(self):
        reference = rectangle_image()
        shifted_background = rectangle_image(
            background=(0.12, 0.12, 0.12), foreground=(0.82, 0.62, 0.42))
        profile = workbench_pfm_compare.edge_coverage(
            reference, shifted_background, 0.02)
        self.assertIsNotNone(profile)
        self.assertLess(profile["coverage_mae"], 1.0e-6)

    def test_face_color_change_is_separated_from_coverage(self):
        reference = rectangle_image()
        recolored = rectangle_image(foreground=(0.7, 0.5, 0.3))
        profile = workbench_pfm_compare.edge_coverage(reference, recolored, 0.02)
        self.assertIsNotNone(profile)
        self.assertLess(profile["coverage_mae"], 1.0e-6)

    def test_uniform_background_reports_unavailable(self):
        pixels = array.array("f", BACKGROUND * (WIDTH * HEIGHT))
        image = WIDTH, HEIGHT, pixels
        self.assertIsNone(workbench_pfm_compare.edge_coverage(image, image, 0.02))

    def test_compare_report_separates_mask_and_edge_thresholds(self):
        image = rectangle_image()
        report = workbench_pfm_compare.compare(
            image, image, threshold=0.1, edge_threshold=0.02)
        self.assertEqual(report["threshold"], 0.1)
        self.assertEqual(report["edge_coverage_threshold"], 0.02)
        self.assertTrue(report["edge_coverage"]["available"])
        self.assertEqual(report["edge_coverage"]["threshold_rgb_distance"], 0.02)

    def test_compare_report_marks_edge_metric_unavailable(self):
        pixels = array.array("f", BACKGROUND * (WIDTH * HEIGHT))
        image = WIDTH, HEIGHT, pixels
        report = workbench_pfm_compare.compare(image, image, threshold=0.1)
        self.assertFalse(report["edge_coverage"]["available"])


if __name__ == "__main__":
    unittest.main()
