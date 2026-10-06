#!/usr/bin/env python3
"""Exercise packaging, immutable identities and linked-worktree paths in fixtures."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from unittest import mock

SCRIPTS=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('distribution_under_test',SCRIPTS/'distribution.py')
distribution=importlib.util.module_from_spec(spec);spec.loader.exec_module(distribution)
PRODUCT=distribution.PRODUCT
VERSION='1.2.3'
BUILD='34393416911.1'


def frozen_identity(build,formats,number=BUILD):
    value={'product':PRODUCT,'version':VERSION,'build_number':number,
        'distribution_version':VERSION+'-build.'+number,'source_revision':'a'*40,'source_state':'clean'}
    build.mkdir(parents=True,exist_ok=True)
    (build/'CMakeCache.txt').write_text('CMAKE_PROJECT_VERSION:STATIC='+VERSION+'\n'+PRODUCT.upper()+'_BUILD_NUMBER:STRING='+number+'\n'+PRODUCT.upper()+'_DISTRIBUTION_VERSION:INTERNAL='+value['distribution_version']+'\n')
    for path in [build/(PRODUCT+'-build-identity.json'),*[build/(PRODUCT+'_artefacts')/'Release'/f/(PRODUCT+'-build-identity.json') for f in formats]]:
        path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(value))


class StablePathTests(unittest.TestCase):
    def test_unversioned_checkout_and_linked_worktree_share_primary_dist(self):
        with tempfile.TemporaryDirectory() as temp:
            repo=Path(temp).resolve()/'repo';repo.mkdir();work=Path(temp).resolve()/'work'
            def git(*args):
                return subprocess.check_output(['git','-C',str(repo),*args],stderr=subprocess.DEVNULL,text=True)
            git('init');git('config','user.name','Fixture');git('config','user.email','fixture@example.invalid')
            (repo/'source.txt').write_text('fixture');git('add','.');git('commit','-m','fixture')
            git('worktree','add','--detach',str(work),'HEAD')
            (repo/'nested').mkdir();(work/'nested').mkdir()
            with mock.patch.dict(os.environ,{},clear=False):
                os.environ.pop('DIST_ROOT',None)
                self.assertEqual(distribution.stable_dist_root(repo),repo/'dist')
                self.assertEqual(distribution.stable_dist_root(work),repo/'dist')
                self.assertEqual(distribution.stable_dist_root(work/'nested'),repo/'nested/dist')

    def test_no_git_fallback_and_explicit_override(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp).resolve()
            with mock.patch.dict(os.environ,{},clear=False):
                os.environ.pop('DIST_ROOT',None)
                self.assertEqual(distribution.stable_dist_root(root),root/'dist')
            with mock.patch.dict(os.environ,{'DIST_ROOT':str(root/'other')}):
                self.assertEqual(distribution.stable_dist_root(root),root/'other')


class DesktopPackageTests:
    platform='Linux'
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.project=Path(self.temp.name);self.build=self.project/'build';self.scripts=self.project/'scripts';self.scripts.mkdir()
        for name in ('distribution.py','package-desktop.py','package-windows.py'):
            if (SCRIPTS/name).exists(): shutil.copyfile(SCRIPTS/name,self.scripts/name)
        self.formats=['VST3','Standalone']+(['CLAP'] if PRODUCT=='YouKnow' else [])
        frozen_identity(self.build,self.formats)
        machine='x86_64-linux' if self.platform=='Linux' else 'x86_64-win'
        self.payload={f'VST3/{PRODUCT}.vst3/Contents/{machine}/{PRODUCT}'+('.so' if self.platform=='Linux' else '.vst3'):b'vst3 binary',
          'Standalone/'+PRODUCT+('.exe' if self.platform=='Windows' else ''):b'standalone binary'}
        if 'CLAP' in self.formats:self.payload['CLAP/'+PRODUCT+'.clap']=b'clap binary'
        self.artifacts=self.build/(PRODUCT+'_artefacts')/'Release'
        for name,value in self.payload.items():
            p=self.artifacts/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(value);p.chmod(0o755)
        self.documents=['LICENSE','THIRD_PARTY_NOTICES.md','ThirdParty/JUCE-LICENSE.md']
        if PRODUCT=='YouKnow': self.documents+=['PRIVACY.md','ThirdParty/CLAP-LICENSE.md','USER_GUIDE.md','INSTALL_MACOS.md','INSTALL_WINDOWS.md','INSTALL_LINUX.md']
        else:self.documents+=['README.md','ThirdParty/CC0-1.0.txt','ThirdParty/Eastman-E1D-README.md','ThirdParty/Shinyguitar-README.txt','Assets/SampleBank/manifest.json']
        for name in self.documents:
            p=self.project/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(name)
        self.env=os.environ.copy();self.env.pop('DIST_ROOT',None)
    def run_package(self):
        if self.platform=='Windows' and PRODUCT=='YouKnow':
            command=[sys.executable,str(self.scripts/'package-windows.py'),'--build-dir',str(self.build)]
        else: command=[sys.executable,str(self.scripts/'package-desktop.py'),'--build-dir',str(self.build),'--platform',self.platform]
        return subprocess.run(command,cwd=self.project,env=self.env,text=True,capture_output=True)
    def directory(self,number=BUILD):return self.project/'dist'/VERSION/('build.'+number)/(self.platform+'-x64')
    def verify_set(self,number=BUILD):
        directory=self.directory(number);stem=PRODUCT+'-'+VERSION+'-build.'+number+'-'+self.platform+'-x64'
        files=list(directory.iterdir());self.assertEqual(len(files),3)
        self.assertTrue(all(p.name.startswith(stem) for p in files))
        checksum=directory/(stem+'-SHA256SUMS.txt')
        for row in checksum.read_text().splitlines():
            expected,name=row.split('  ',1);self.assertEqual(distribution.digest(directory/name),expected)
        m=json.loads((directory/(stem+'.manifest.json')).read_text())
        self.assertEqual(m['source_revision'],'a'*40);self.assertEqual(m['build_number'],number)
        return directory/(stem+('.zip' if self.platform=='Windows' else '.tar.gz'))
    def test_complete_archive_identity_and_named_checksums(self):
        result=self.run_package();self.assertEqual(result.returncode,0,result.stderr)
        archive=self.verify_set()
        if self.platform=='Windows':
            with zipfile.ZipFile(archive) as z:
                for path,data in self.payload.items():self.assertEqual(z.read(path),data)
                self.assertIsNone(z.testzip())
        else:
            with tarfile.open(archive) as t:
                for path,data in self.payload.items():self.assertEqual(t.extractfile(path).read(),data)
        self.assertIn(str(archive),result.stdout)
    def test_new_build_preserves_previous_versioned_set(self):
        self.assertEqual(self.run_package().returncode,0)
        before={p.name:p.read_bytes() for p in self.directory().iterdir()}
        frozen_identity(self.build,self.formats,'34393416911.2')
        result=self.run_package();self.assertEqual(result.returncode,0,result.stderr);self.verify_set('34393416911.2')
        self.assertEqual(before,{p.name:p.read_bytes() for p in self.directory().iterdir()})
    def test_existing_identity_is_immutable(self):
        self.assertEqual(self.run_package().returncode,0)
        before={p.name:p.read_bytes() for p in self.directory().iterdir()}
        result=self.run_package();self.assertNotEqual(result.returncode,0)
        self.assertIn('distribution identity already exists',result.stderr)
        self.assertEqual(before,{p.name:p.read_bytes() for p in self.directory().iterdir()})

    def test_relabelled_cache_cannot_package_old_binaries(self):
        cache=self.build/'CMakeCache.txt';cache.write_text(cache.read_text().replace(BUILD,'34393416911.2'))
        result=self.run_package();self.assertNotEqual(result.returncode,0);self.assertIn('identity mismatch',result.stderr)
        self.assertFalse((self.project/'dist').exists())
    def test_missing_or_different_compiled_identity_rejected(self):
        for fmt in self.formats:
            path=self.artifacts/fmt/(PRODUCT+'-build-identity.json');before=path.read_bytes()
            for data in (None,b'{}'):
                with self.subTest(format=fmt,data=data):
                    if data is None:path.unlink()
                    else:path.write_bytes(data)
                    self.assertNotEqual(self.run_package().returncode,0)
                    self.assertFalse((self.project/'dist').exists())
                    path.write_bytes(before)
    def test_invalid_build_numbers_and_duplicate_identity_rejected(self):
        cache=self.build/'CMakeCache.txt';before=cache.read_text()
        for number in ('','0','1.0','01','1.01','1.2.3','../bad','1\n2'):
            with self.subTest(number=number):
                cache.write_text(before.replace(BUILD,number));self.assertNotEqual(self.run_package().returncode,0)
        cache.write_text(before+PRODUCT.upper()+'_BUILD_NUMBER:STRING='+BUILD+'\n')
        self.assertNotEqual(self.run_package().returncode,0);self.assertFalse((self.project/'dist').exists())
    def test_archive_write_failure_preserves_previous_distribution(self):
        self.assertEqual(self.run_package().returncode,0)
        before={p.name:p.read_bytes() for p in self.directory().iterdir()}
        script=self.scripts/('package-windows.py' if self.platform=='Windows' and PRODUCT=='YouKnow' else 'package-desktop.py')
        arguments=[str(script),'--build-dir',str(self.build)]
        if script.name=='package-desktop.py':arguments+=['--platform',self.platform]
        fault='zipfile.ZipFile.write' if self.platform=='Windows' else 'tarfile.TarFile.add'
        code="import runpy,sys,zipfile,tarfile; from unittest.mock import patch; sys.path.insert(0,sys.argv[1]); sys.argv=sys.argv[2:];\nwith patch('"+fault+"',side_effect=OSError('simulated archive failure')): runpy.run_path(sys.argv[0],run_name='__main__')"
        for number in ('34393416911.2',):
            frozen_identity(self.build,self.formats,number)
            result=subprocess.run([sys.executable,'-c',code,str(self.scripts),*arguments],cwd=self.project,env=self.env,capture_output=True,text=True)
            self.assertNotEqual(result.returncode,0);self.assertIn('simulated archive failure',result.stderr)
            self.assertEqual(before,{p.name:p.read_bytes() for p in self.directory().iterdir()})

    def test_missing_or_empty_payload_and_documents_preserves_previous_set(self):
        self.assertEqual(self.run_package().returncode,0)
        before={p.name:p.read_bytes() for p in self.directory().iterdir()}
        for path in [*[self.artifacts/f for f in self.payload],*[self.project/f for f in self.documents]]:
            contents=path.read_bytes()
            for absent in (True,False):
                with self.subTest(path=str(path),absent=absent):
                    if absent:path.unlink()
                    else:path.write_bytes(b'')
                    self.assertNotEqual(self.run_package().returncode,0)
                    self.assertEqual(before,{p.name:p.read_bytes() for p in self.directory().iterdir()})
                    path.write_bytes(contents);path.chmod(0o755)

class LinuxPackagingTests(DesktopPackageTests,unittest.TestCase):platform='Linux'
class WindowsPackagingTests(DesktopPackageTests,unittest.TestCase):platform='Windows'

if __name__=='__main__':unittest.main()
