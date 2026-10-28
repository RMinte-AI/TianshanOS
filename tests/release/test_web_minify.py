"""CSS compression must preserve calculation and selector syntax."""
import importlib.util
import unittest
import tempfile
import gzip
import re
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


class AssetVersionTests(unittest.TestCase):
    def test_new_content_changes_all_entry_urls_and_dynamic_loader_together(self):
        with tempfile.TemporaryDirectory(prefix='tianshan-assets-') as temporary:
            root = Path(temporary)
            (root / 'index.html').write_text((ROOT / 'components/ts_webui/web/index.html').read_text())
            (root / 'js/lang').mkdir(parents=True)
            (root / 'css').mkdir()
            (root / 'js/api.js').write_text('const value = 1;')
            (root / 'js/lang/en-US.js').write_text('const label = "before";')
            (root / 'css/style.css').write_text('.btn { color: red; }')
            first = module.stamp_assets(temporary)
            html = (root / 'index.html').read_text()
            self.assertEqual(set(re.findall(r'\?v=([^"\']+)', html)), {first})
            self.assertIn("window.TS_ASSET_VERSION = '" + first + "'", html)
            self.assertEqual(module.stamp_assets(temporary), first)
            (root / 'js/lang/en-US.js').write_text('const label = "after";')
            second = module.stamp_assets(temporary)
            self.assertNotEqual(second, first)
            (root / 'css/style.css').write_text('.btn { color: blue; }')
            third = module.stamp_assets(temporary)
            self.assertNotEqual(third, second)
            module.gzip_directory(temporary)
            self.assertEqual(gzip.decompress((root / 'index.html.gz').read_bytes()),
                             (root / 'index.html').read_bytes())


if __name__ == '__main__':
    unittest.main()
