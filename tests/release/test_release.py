import importlib.util
import re
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('check_release', ROOT / 'tools/check_release.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ReleaseTests(unittest.TestCase):
    def test_current_notes_match_version(self):
        self.assertEqual(module.validate('v0.5.2', '0.5.2+fixture.1234', ROOT).name, 'v0.5.2.md')

    def test_mismatch_and_missing_notes_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'version.txt').write_text('0.5.2\n')
            for tag, version in [('v0.5.1', '0.5.2'), ('v0.5.2', '0.5.1'), ('../../x', '0.5.2')]:
                with self.subTest(tag=tag, version=version), self.assertRaises(ValueError):
                    module.validate(tag, version, root)
            with self.assertRaises(FileNotFoundError):
                module.validate('v0.5.2', '0.5.2', root)
            notes = root / 'docs/releases/v0.5.2.md'
            notes.parent.mkdir(parents=True)
            notes.write_text('English-only summary')
            with self.assertRaises(ValueError):
                module.validate('v0.5.2', '0.5.2', root)

    def test_actual_workflow_release_condition(self):
        workflow = (ROOT / '.github/workflows/build.yml').read_text()
        release = workflow.split('\n  release:\n', 1)[1]
        expression = re.search(r'^    if: (.+)$', release, re.M).group(1)
        expression = expression.replace('&&', ' and ').replace('||', ' or ')
        cases = [
            ('RMinte-AI/TianshanOS', 'push', 'refs/heads/main', True),
            ('massif-01/TianshanOS', 'push', 'refs/heads/main', False),
            ('RMinte-AI/TianshanOS', 'pull_request', 'refs/pull/41/merge', False),
            ('RMinte-AI/TianshanOS', 'push', 'refs/heads/develop', False),
            ('RMinte-AI/TianshanOS', 'push', 'refs/tags/v0.5.2', True),
            ('RMinte-AI/TianshanOS', 'workflow_dispatch', 'refs/heads/main', False),
        ]
        for repo, event, ref, expected in cases:
            github = SimpleNamespace(repository=repo, event_name=event, ref=ref)
            with self.subTest(repo=repo, event=event, ref=ref):
                self.assertEqual(eval(expression, {'__builtins__': {}}, {'github': github, 'startsWith': str.startswith}), expected)
        self.assertIn('needs: [build, web-tests]', release)
        self.assertIn('target_commitish: ${{ github.sha }}', release)
        self.assertIn('body_path: docs/releases/${{ steps.release_tag.outputs.tag }}.md', release)
        self.assertIn('generate_release_notes: false', release)


if __name__ == '__main__':
    unittest.main()
