"""CSS compression must preserve calculation and selector syntax."""
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('minify_web', ROOT / 'tools/minify_web.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class CssMinifyTests(unittest.TestCase):
    def test_calculation_operators_keep_required_spaces(self):
        result = module.minify_css('.slider { width: calc(100% + 2px); left: calc(50% - 1px); }')
        self.assertIn('calc(100% + 2px)', result)
        self.assertIn('calc(50% - 1px)', result)

    def test_pseudo_class_and_media_negation_remain_distinct(self):
        result = module.minify_css('@media not (hover: hover) { .btn:not(.disabled) { color: red; } }')
        self.assertIn('@media not (hover:hover)', result)
        self.assertIn('.btn:not(.disabled)', result)
        self.assertNotIn(':not (', result)


if __name__ == '__main__':
    unittest.main()
