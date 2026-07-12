import unittest

from validate_log import parse_records, validate


VALID = """
I m0: M0_DISPLAY_CONFIG tear_mode=triple_partial
I m0: M0_PSRAM bytes=1
I m0: M0_JPEG result=ok
I m0: M0_DISPLAY_CASE result=ok
I m0: M0_SD_READ pass=1 throughput_mib_s=1.5
I m0: M0_SD_READ pass=2 throughput_mib_s=1.6
I m0: M0_SD result=ok
I m0: M0_COMBINED requested_frames=90 completed_frames=89 failed_frames=1 dropped_frames=2 elapsed_us=3000000 achieved_fps=29.67 sd_read_us=1 jpeg_decode_us=2 lvgl_display_us=3 psram_free=4 psram_largest=4 internal_free=5
I m0: M0_HEAP checkpoint=after_combined
I m0: M0_COMPLETE touch_grid_ready=1
"""


class ValidateLogTests(unittest.TestCase):
    def test_valid_log(self):
        self.assertEqual([], validate(parse_records(VALID)))

    def test_detects_impossible_counts(self):
        text = VALID.replace("completed_frames=89", "completed_frames=91")
        self.assertIn("M0_COMBINED completed+failed exceeds requested", validate(parse_records(text)))

    def test_requires_both_sd_passes(self):
        text = VALID.replace("I m0: M0_SD_READ pass=2 throughput_mib_s=1.6\n", "")
        self.assertIn("M0_SD_READ requires pass=1 and pass=2", validate(parse_records(text)))


if __name__ == "__main__":
    unittest.main()
