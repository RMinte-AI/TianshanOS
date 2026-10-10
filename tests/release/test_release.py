import importlib.util
import re
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('check_release', ROOT / 'tools/check_release.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ReleaseTests(unittest.TestCase):
    def test_current_notes_match_version(self):
        version = (ROOT / 'version.txt').read_text().strip()
        self.assertEqual(module.validate(f'v{version}', f'{version}+fixture.1234', ROOT).name, f'v{version}.md')

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
        policy_spec = importlib.util.spec_from_file_location('check_profile', ROOT / 'tools/ci/check_profile.py')
        policy = importlib.util.module_from_spec(policy_spec)
        policy_spec.loader.exec_module(policy)
        cases = [
            ('RMinte-AI/TianshanOS', 'push', 'refs/heads/main', {}, True),
            ('massif-01/TianshanOS', 'push', 'refs/heads/main', {}, False),
            ('RMinte-AI/TianshanOS', 'pull_request', 'refs/pull/41/merge', {}, False),
            ('RMinte-AI/TianshanOS', 'push', 'refs/heads/develop', {}, False),
            ('RMinte-AI/TianshanOS', 'push', 'refs/tags/v0.5.2', {}, True),
            ('RMinte-AI/TianshanOS', 'release', 'refs/tags/v0.5.2', {'action': 'published'}, True),
            ('RMinte-AI/TianshanOS', 'workflow_dispatch', 'refs/heads/main', {}, False),
        ]
        for repo, event, ref, payload, expected in cases:
            with self.subTest(repo=repo, event=event, ref=ref):
                self.assertEqual(policy.classify(repo, event, ref, payload, ['README.md'])['publish_release'], expected)
        workflow = (ROOT / '.github/workflows/build.yml').read_text()
        jobs = workflow.split('\njobs:\n', 1)[1]
        def job(name):
            return re.search(r'^  ' + re.escape(name) + r':\n(.*?)(?=^  [\w-]+:|\Z)', jobs, re.M | re.S).group(1)
        release = job('release')
        dependencies = re.search(r'^    needs: \[(.*?)\]$', release, re.M).group(1)
        self.assertEqual(set(x.strip() for x in dependencies.split(',')), {'changes', 'build', 'ci-gate'})
        expression = re.search(r'^    if: (.+)$', release, re.M).group(1)
        self.assertEqual(re.sub(r'\s+', '', expression), "${{!cancelled()&&needs.changes.outputs.publish_release=='true'&&needs.build.result=='success'&&needs['ci-gate'].result=='success'}}")
        self.assertIn('target_commitish: ${{ needs.build.outputs.commit }}', release)
        self.assertIn('body_path: docs/releases/${{ steps.preflight.outputs.tag }}.md', release)
        self.assertIn('generate_release_notes: false', release)
        self.assertIn('python3 tools/ci/check_profile.py', job('changes'))
        gate = job('ci-gate')
        selected = re.search(r'^    needs: \[(.*?)\]$', gate, re.M).group(1)
        self.assertEqual(set(x.strip() for x in selected.split(',')), {'changes', 'web-tests', 'runtime-tests', 'build', 'web-artifacts'})
        self.assertIn('if: always()', gate)
        self.assertIn('if: ${{ always() && !cancelled() }}', gate)
        self.assertIn('if: ${{ always() && cancelled() }}', gate)
        self.assertIn('CI Gate failed: workflow cancelled at the final decision', gate)
        self.assertNotIn('paths-ignore:', workflow)


if __name__ == '__main__':
    unittest.main()
