"""Bounded policy and emitted-resource regressions; no GitHub writes or devices."""
import gzip
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'tools/ci' / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

profile, gate, web, release = [load(name) for name in ('check_profile', 'check_gate', 'check_web_artifacts', 'check_release_state')]

class PolicyTests(unittest.TestCase):
    def test_events_keep_main_release_and_force_full(self):
        cases = [
            ('RMinte-AI/TianshanOS','push','refs/heads/main',{},True),
            ('RMinte-AI/TianshanOS','push','refs/heads/develop',{},False),
            ('massif-01/TianshanOS','push','refs/heads/main',{},False),
            ('RMinte-AI/TianshanOS','push','refs/tags/v0.6.1',{},True),
            ('RMinte-AI/TianshanOS','release','refs/tags/v0.6.1',{'action':'published'},True),
            ('RMinte-AI/TianshanOS','workflow_dispatch','refs/heads/main',{},False),
        ]
        for repo,event,ref,payload,publish in cases:
            with self.subTest(repo=repo,event=event):
                self.assertEqual(profile.classify(repo,event,ref,payload),{'run_full':True,'publish_release':publish})

    def test_only_known_document_pr_is_light(self):
        for paths,full in [(['README.md','docs/ci/plan.md'],False),([],True),
                           (['README.md','app.c'],True),(['docs/vendor.json'],True),
                           (['.github/workflows/build.yml'],True),(['app.c','docs/app.md'],True)]:
            with self.subTest(paths=paths):
                self.assertEqual(profile.classify(profile.UPSTREAM,'pull_request','refs/pull/1/merge',{},paths),{'run_full':full,'publish_release':False})
        with self.assertRaises(ValueError):
            profile.classify(profile.UPSTREAM,'pull_request','refs/pull/1/merge',{})

    def test_git_diff_keeps_both_rename_paths_and_deletions(self):
        with tempfile.TemporaryDirectory() as folder:
            def git(*args):
                return subprocess.check_output(['git','-C',folder,*args],text=True).strip()
            git('init','-q');git('config','user.email','fixture@example.invalid');git('config','user.name','Fixture')
            root=Path(folder);(root/'code.c').write_text('int existing;\n');(root/'gone.c').write_text('int removed;\n')
            git('add','.');git('commit','-qm','before');base=git('rev-parse','HEAD')
            (root/'docs').mkdir();git('mv','code.c','docs/code.md');git('rm','gone.c');git('commit','-qm','after')
            from unittest.mock import patch
            original=subprocess.check_output;head=git('rev-parse','HEAD')
            with patch.object(profile.subprocess,'check_output',side_effect=lambda args:original(args,cwd=folder)):
                paths=profile.changed_paths(base,head)
            self.assertEqual(set(paths),{'code.c','docs/code.md','gone.c'})

    def test_document_pr_ignores_changes_only_on_advanced_main(self):
        with tempfile.TemporaryDirectory() as folder:
            def git(*args):
                return subprocess.check_output(['git','-C',folder,*args],text=True).strip()
            git('init','-q','-b','main');git('config','user.email','fixture@example.invalid');git('config','user.name','Fixture')
            root=Path(folder);(root/'README.md').write_text('Original documentation\n');(root/'main.c').write_text('int original;\n')
            git('add','.');git('commit','-qm','A: common start');start=git('rev-parse','HEAD')
            (root/'main.c').write_text('int main_only;\n')
            git('commit','-qam','B: main code change');base=git('rev-parse','HEAD')
            git('checkout','-qb','docs',start)
            (root/'README.md').write_text('Updated documentation\n')
            git('commit','-qam','C: documentation change');head=git('rev-parse','HEAD')
            previous=Path.cwd()
            try:
                os.chdir(folder)
                paths=profile.changed_paths(base,head)
            finally:
                os.chdir(previous)
            self.assertEqual(paths,['README.md'])
            self.assertEqual(profile.classify(profile.UPSTREAM,'pull_request','refs/pull/1/merge',{},paths),
                             {'run_full':False,'publish_release':False})

    def needs(self,full=True):
        return {'changes':{'result':'success','outputs':{'run_full':str(full).lower(),'publish_release':'false'}},
                **{job:{'result':'success' if full else 'skipped'} for job in gate.REQUIRED}}

    def test_gate_requires_selected_success_and_current_run_not_cancelled(self):
        for full in (True,False):
            gate.check_gate(self.needs(full),False)
        for result in ('failure','cancelled','skipped'):
            n=self.needs();n['build']['result']=result
            with self.subTest(result=result),self.assertRaises(ValueError):gate.check_gate(n,False)
        with self.assertRaises(ValueError):gate.check_gate(self.needs(),True)
        n=self.needs();n['changes']['result']='failure'
        with self.assertRaises(ValueError):gate.check_gate(n,False)
        for missing in (None,'yes'):
            n=self.needs();n['changes']['outputs']['run_full']=missing
            with self.assertRaises(ValueError):gate.check_gate(n,False)
        n=self.needs(False);n['changes']['outputs']['publish_release']='true'
        with self.assertRaises(ValueError):gate.check_gate(n,False)
        n=self.needs(False);n['build']['result']='success'
        with self.assertRaises(ValueError):gate.check_gate(n,False)

    def test_release_main_skip_precedes_old_tag_lookup(self):
        calls=[]
        def get(path):calls.append(path);return {'tag_name':'v0.6.1'}
        skip,message=release.prepare_release(profile.UPSTREAM,'main','v0.6.1','new',get)
        self.assertTrue(skip);self.assertEqual(message,'该版本已存在，本次未发布新资产');self.assertEqual(len(calls),1)

    def test_main_new_release_allows_absent_tag_but_refuses_wrong_commit(self):
        self.assertFalse(release.prepare_release(profile.UPSTREAM,'main','v0.6.1','new',lambda path:None)[0])
        for commit in ('new','old'):
            def get(path):return None if '/releases/' in path else {'object':{'type':'commit','sha':commit}}
            if commit=='new':self.assertFalse(release.prepare_release(profile.UPSTREAM,'main','v0.6.1','new',get)[0])
            else:
                with self.assertRaises(ValueError):release.prepare_release(profile.UPSTREAM,'main','v0.6.1','new',get)

    def test_published_upload_and_query_errors(self):
        def get(path):return {'tag_name':'v0.6.1'} if '/releases/' in path else {'object':{'type':'commit','sha':'new'}}
        for mode in ('published','tag'):
            self.assertFalse(release.prepare_release(profile.UPSTREAM,mode,'v0.6.1','new',get)[0])
        def broken(path):raise RuntimeError('GitHub query failed')
        with self.assertRaises(RuntimeError):release.prepare_release(profile.UPSTREAM,'main','v0.6.1','new',broken)
        from unittest.mock import patch
        import urllib.error
        for status in (404,403,500):
            with patch.dict('os.environ',{'GH_TOKEN':'fixture'}),patch.object(release.urllib.request,'urlopen',side_effect=urllib.error.HTTPError('https://api.github.com/fixture',status,'fixture',{},None)):
                if status==404:self.assertIsNone(release.query('fixture'))
                else:
                    with self.assertRaises(RuntimeError):release.query('fixture')

class ArtifactTests(unittest.TestCase):
    def test_required_gzip_and_emitted_syntax(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            for name in web.ESSENTIAL:
                p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text('const fixture = 1;' if p.suffix=='.js' else '<p>fixture</p>' if p.suffix=='.html' else 'p{color:red}')
                Path(str(p)+'.gz').write_bytes(gzip.compress(p.read_bytes()))
            web.check_web(root)
            packed=root/'index.html.gz';correct=packed.read_bytes();packed.unlink()
            with self.assertRaisesRegex(ValueError,'Missing required gzip'):web.check_web(root)
            packed.write_bytes(gzip.compress(b'wrong'))
            with self.assertRaisesRegex(ValueError,'Gzip differs'):web.check_web(root)
            packed.write_bytes(correct);js=root/'js/app.js';js.write_text('const = ;');Path(str(js)+'.gz').write_bytes(gzip.compress(js.read_bytes()))
            with self.assertRaises(subprocess.CalledProcessError):web.check_web(root)

    def test_www_must_exist_and_be_mapped_in_same_build(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);(root/'flasher_args.json').write_text(json.dumps({'flash_files':{'0x6a0000':'www.bin'}}))
            with self.assertRaises(ValueError):web.check_build(root)
            (root/'www.bin').write_bytes(b'fixture');web.check_build(root)
            (root/'flasher_args.json').write_text(json.dumps({'flash_files':{}}))
            with self.assertRaises(ValueError):web.check_build(root)

if __name__=='__main__':unittest.main()
