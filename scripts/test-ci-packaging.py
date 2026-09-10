#!/usr/bin/env python3
"""Check distribution completeness and preview publication in isolated fixtures."""

import hashlib
import io
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import textwrap
import unittest
import zipfile


SCRIPTS = Path(__file__).resolve().parent
BUILD_NUMBER = "34393416911.1"
DISTRIBUTION_VERSION = f"1.2.3-build.{BUILD_NUMBER}"


def version_cache() -> bytes:
    return (
        "CMAKE_PROJECT_VERSION:STATIC=1.2.3\n"
        f"YOUKNOW_BUILD_NUMBER:STRING={BUILD_NUMBER}\n"
        f"YOUKNOW_DISTRIBUTION_VERSION:INTERNAL={DISTRIBUTION_VERSION}\n"
    ).encode()


def invalid_build_caches():
    valid = version_cache()
    build_entry = f"YOUKNOW_BUILD_NUMBER:STRING={BUILD_NUMBER}\n".encode()
    for build in (b"", b"0", b"1.0", b"01", b"1.01", b"1.2.3", b"../bad", b"1\n2"):
        yield "invalid build number", valid.replace(build_entry, b"YOUKNOW_BUILD_NUMBER:STRING=" + build + b"\n")
    yield "missing build number", valid.replace(build_entry, b"")
    yield "duplicate build number", valid + build_entry
    distribution_entry = f"YOUKNOW_DISTRIBUTION_VERSION:INTERNAL={DISTRIBUTION_VERSION}\n".encode()
    yield "missing distribution version", valid.replace(distribution_entry, b"")
    yield "duplicate distribution version", valid + distribution_entry
    yield "mismatched project version", valid.replace(b"STATIC=1.2.3", b"STATIC=1.2.4")
    yield "mismatched build number", valid.replace(build_entry, b"YOUKNOW_BUILD_NUMBER:STRING=34393416911.2\n")
    yield "unsafe distribution version", valid.replace(distribution_entry, b"YOUKNOW_DISTRIBUTION_VERSION:INTERNAL=../bad\n")


def write(path: Path, contents: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(contents)


class WindowsPackagingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="youknow-package-test-")
        self.addCleanup(self.temporary.cleanup)
        self.project = Path(self.temporary.name)
        self.script = self.project / "scripts/package-windows.py"
        self.script.parent.mkdir()
        shutil.copyfile(SCRIPTS / "package-windows.py", self.script)
        self.build = self.project / "build-win"
        self.artifacts = self.build / "YouKnow_artefacts/Release"
        self.payload = {
            "VST3/YouKnow.vst3/Contents/x86_64-win/YouKnow.vst3": b"vst3 binary",
            "VST3/YouKnow.vst3/Contents/Resources/moduleinfo.json": b'{"Name":"YouKnow"}',
            "CLAP/YouKnow.clap": b"clap binary",
            "Standalone/YouKnow.exe": b"standalone binary",
        }
        for relative, contents in self.payload.items():
            write(self.artifacts / relative, contents)
        write(self.build / "CMakeCache.txt", version_cache())
        self.notices = (
            "LICENSE", "THIRD_PARTY_NOTICES.md", "PRIVACY.md", "USER_GUIDE.md",
            "ThirdParty/JUCE-LICENSE.md", "ThirdParty/CLAP-LICENSE.md",
            "INSTALL_MACOS.md", "INSTALL_WINDOWS.md", "INSTALL_LINUX.md",
        )
        for relative in self.notices:
            write(self.project / relative, relative.encode())

    def package(self):
        return subprocess.run(
            [sys.executable, str(self.script), "--build-dir", str(self.build)],
            capture_output=True, text=True, check=False,
        )

    def test_archive_contains_complete_bundle_formats_notices_and_matching_checksum(self):
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stderr)
        archive = self.build / f"dist/YouKnow-{DISTRIBUTION_VERSION}-Windows-x64.zip"
        with zipfile.ZipFile(archive) as output:
            expected = set(self.payload) | (set(self.notices) - {"USER_GUIDE.md"}) | {"README.md"}
            self.assertEqual(set(output.namelist()), expected)
            self.assertEqual(len(output.namelist()), len(expected))
            for relative, contents in self.payload.items():
                self.assertEqual(output.read(relative), contents)
            self.assertEqual(output.read("README.md"), b"USER_GUIDE.md")
            self.assertIsNone(output.testzip())
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        self.assertEqual(
            (archive.parent / "SHA256SUMS.txt").read_text(),
            f"{digest}  {archive.name}\n",
        )

    def test_missing_or_empty_binary_and_notice_fail_before_archiving(self):
        required = [
            self.artifacts / "VST3/YouKnow.vst3/Contents/x86_64-win/YouKnow.vst3",
            self.artifacts / "CLAP/YouKnow.clap",
            self.artifacts / "Standalone/YouKnow.exe",
            *(self.project / relative for relative in self.notices),
        ]
        for path in required:
            original = path.read_bytes()
            for empty in (False, True):
                with self.subTest(path=str(path.relative_to(self.project)), empty=empty):
                    if empty:
                        path.write_bytes(b"")
                    else:
                        path.unlink()
                    result = self.package()
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("Missing or empty", result.stderr)
                    self.assertFalse((self.build / "dist").exists())
                    path.write_bytes(original)

    def test_invalid_or_ambiguous_version_fails_before_archiving(self):
        for version in (b"", b"CMAKE_PROJECT_VERSION:STATIC=../bad\n",
                        b"CMAKE_PROJECT_VERSION:STATIC=1.2\nCMAKE_PROJECT_VERSION:STATIC=2.3\n"):
            with self.subTest(version=version):
                (self.build / "CMakeCache.txt").write_bytes(version)
                result = self.package()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("one valid project version", result.stderr)
                self.assertFalse((self.build / "dist").exists())

    def test_invalid_or_inconsistent_build_identity_fails_before_archiving(self):
        for description, cache in invalid_build_caches():
            with self.subTest(description=description):
                (self.build / "CMakeCache.txt").write_bytes(cache)
                result = self.package()
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex(result.stderr, "valid build number|distribution version must match")
                self.assertFalse((self.build / "dist").exists())

    def test_new_build_replaces_old_archive_and_checksum(self):
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stderr)
        unrelated = self.build / "dist/keep.txt"
        unrelated.write_text("preserve")
        (self.build / "CMakeCache.txt").write_bytes(
            version_cache().replace(BUILD_NUMBER.encode(), b"34393416911.2")
        )
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stderr)
        dist = self.build / "dist"
        archive = dist / "YouKnow-1.2.3-build.34393416911.2-Windows-x64.zip"
        self.assertEqual(set(dist.iterdir()), {archive, dist / "SHA256SUMS.txt", unrelated})
        self.assertEqual(unrelated.read_text(), "preserve")
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        self.assertEqual((dist / "SHA256SUMS.txt").read_text(), f"{digest}  {archive.name}\n")


@unittest.skipUnless(sys.platform == "darwin", "macOS bundle validation requires PlistBuddy")
class MacPackagingVersionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="youknow-macos-version-test-")
        self.addCleanup(self.temporary.cleanup)
        self.project = Path(self.temporary.name)
        self.script = self.project / "scripts/sign-and-package-macos.sh"
        self.script.parent.mkdir()
        shutil.copyfile(SCRIPTS / "sign-and-package-macos.sh", self.script)
        self.build = self.project / "build-macos"
        self.juce = self.build / "juce"
        self.juce.mkdir(parents=True)
        self.cache = version_cache() + (
            f"JUCE_SOURCE_DIR:STATIC={self.juce}\n"
            "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++\n"
            "CMAKE_OSX_DEPLOYMENT_TARGET:STRING=11.0\n"
        ).encode()
        write(self.build / "CMakeCache.txt", self.cache)
        for relative in (
            "LICENSE", "README.md", "USER_GUIDE.md", "THIRD_PARTY_NOTICES.md", "PRIVACY.md",
            "ThirdParty/JUCE-LICENSE.md", "ThirdParty/CLAP-LICENSE.md",
            "INSTALL_MACOS.md", "INSTALL_WINDOWS.md", "INSTALL_LINUX.md",
        ):
            write(self.project / relative, relative.encode())
        self.plists = {}
        for relative, identifier in (
            ("VST3/YouKnow.vst3", "cz.protocodus.youknow.vst3"),
            ("AU/YouKnow.component", "cz.protocodus.youknow.au"),
            ("CLAP/YouKnow.clap", "cz.protocodus.youknow.clap"),
            ("Standalone/YouKnow.app", "cz.protocodus.youknow"),
        ):
            path = self.build / "YouKnow_artefacts/Release" / relative / "Contents/Info.plist"
            contents = {
                "CFBundleShortVersionString": "1.2.3", "CFBundleVersion": BUILD_NUMBER,
                "CFBundleIdentifier": identifier, "CFBundleDisplayName": "YouKnow",
                "NSHumanReadableCopyright": "Copyright (c) 2026 Protocodus",
                "AudioComponents": [{"type": "aumu", "subtype": "Yk06", "manufacturer": "Ykno"}],
            }
            self.plists[path] = contents
            write(path, plistlib.dumps(contents))

    def package(self):
        environment = os.environ.copy()
        environment.update({"BUILD_DIR": str(self.build), "RELEASE_MODE": "0", "CONFIG": "Release"})
        environment.pop("VERSION", None)
        return subprocess.run(
            ["bash", str(self.script)], env=environment,
            capture_output=True, text=True, check=False,
        )

    def test_all_four_bundle_build_numbers_must_match_configured_build(self):
        # The complete version identity is accepted before the intentionally
        # absent module metadata stops this non-signing fixture.
        result = self.package()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("missing VST3 module metadata", result.stderr)
        for path, contents in self.plists.items():
            with self.subTest(bundle=path.parent.parent.name):
                write(path, plistlib.dumps({**contents, "CFBundleVersion": "34393416911.2"}))
                result = self.package()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"{path.parent.parent.name} build number", result.stderr)
                self.assertIn(f"expected '{BUILD_NUMBER}'", result.stderr)
                self.assertFalse((self.build / "dist").exists())
                write(path, plistlib.dumps(contents))

    def test_bundle_marketing_version_must_match_configured_version(self):
        for path, contents in self.plists.items():
            write(path, plistlib.dumps({**contents, "CFBundleShortVersionString": "1.2.4"}))
        result = self.package()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bundle version is '1.2.4', expected '1.2.3'", result.stderr)
        self.assertFalse((self.build / "dist").exists())

    def test_invalid_or_inconsistent_build_identity_fails_before_staging(self):
        for description, cache in invalid_build_caches():
            with self.subTest(description=description):
                (self.build / "CMakeCache.txt").write_bytes(cache)
                result = self.package()
                self.assertNotEqual(result.returncode, 0)
                self.assertRegex(result.stderr, "valid build number|distribution version must match")
                self.assertFalse((self.build / "dist").exists())


class LinuxPackagingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="youknow-linux-package-test-")
        self.addCleanup(self.temporary.cleanup)
        self.project = Path(self.temporary.name)
        write(self.project / "build-dsp/CMakeCache.txt", version_cache())
        self.artifacts = self.project / "build-dsp/YouKnow_artefacts/Release"
        self.payload = {
            "VST3/YouKnow.vst3/Contents/x86_64-linux/YouKnow.so": b"vst3 binary",
            "Standalone/YouKnow": b"standalone binary",
        }
        for relative, contents in self.payload.items():
            write(self.artifacts / relative, contents)
        (self.artifacts / "Standalone/YouKnow").chmod(0o755)
        self.documents = (
            "LICENSE", "THIRD_PARTY_NOTICES.md", "PRIVACY.md", "USER_GUIDE.md",
            "ThirdParty/JUCE-LICENSE.md", "ThirdParty/CLAP-LICENSE.md",
            "INSTALL_MACOS.md", "INSTALL_WINDOWS.md", "INSTALL_LINUX.md",
        )
        for relative in self.documents:
            write(self.project / relative, relative.encode())
        # Execute the actual inline CI packaging step without a YAML dependency.
        workflow = (SCRIPTS.parent / ".github/workflows/ci.yml").read_text()
        step = workflow.split("      - name: Package Linux VST3 and standalone\n", 1)[1]
        run_block = step.split("        run: |\n", 1)[1].split("\n      - name:", 1)[0]
        self.script = textwrap.dedent(run_block)
        self.archive = self.project / f"build-dsp/dist/YouKnow-{DISTRIBUTION_VERSION}-Linux-x64.tar.gz"

    def package(self):
        # macOS tar adds AppleDouble files by default; the production step runs
        # on Linux, so keep the local fixture archive equivalent to that output.
        environment = os.environ.copy()
        environment["COPYFILE_DISABLE"] = "1"
        return subprocess.run(
            ["bash", "-c", self.script], cwd=self.project,
            env=environment,
            capture_output=True, text=True, check=False,
        )

    def test_archive_preserves_binaries_and_includes_linked_documentation(self):
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stderr)
        with tarfile.open(self.archive) as output:
            files = {entry.name for entry in output.getmembers() if entry.isfile()}
            expected = set(self.payload) | (set(self.documents) - {"USER_GUIDE.md"})
            expected |= {"README.md", "JUCE-LICENSE.md"}
            self.assertEqual(files, expected)
            for relative, contents in self.payload.items():
                self.assertEqual(output.extractfile(relative).read(), contents)
            self.assertEqual(output.extractfile("README.md").read(), b"USER_GUIDE.md")
            for relative in set(self.documents) - {"USER_GUIDE.md"}:
                self.assertEqual(output.extractfile(relative).read(), relative.encode())
            self.assertTrue(output.getmember("Standalone/YouKnow").mode & 0o111)
        digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.assertEqual(
            (self.archive.parent / "SHA256SUMS.txt").read_text(),
            f"{digest}  {self.archive.name}\n",
        )

    def test_missing_or_empty_document_fails_before_archiving(self):
        for relative in self.documents:
            path = self.project / relative
            original = path.read_bytes()
            for empty in (False, True):
                with self.subTest(document=relative, empty=empty):
                    if empty:
                        path.write_bytes(b"")
                    else:
                        path.unlink()
                    result = self.package()
                    self.assertNotEqual(result.returncode, 0)
                    self.assertFalse(self.archive.exists())
                    path.write_bytes(original)

    def test_invalid_or_inconsistent_build_identity_fails_before_archiving(self):
        for description, cache in invalid_build_caches():
            with self.subTest(description=description):
                (self.project / "build-dsp/CMakeCache.txt").write_bytes(cache)
                result = self.package()
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.archive.exists())

    def test_new_build_replaces_old_archive_and_checksum(self):
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stderr)
        dist = self.archive.parent
        unrelated = dist / "keep.txt"
        unrelated.write_text("preserve")
        (self.project / "build-dsp/CMakeCache.txt").write_bytes(
            version_cache().replace(BUILD_NUMBER.encode(), b"34393416911.2")
        )
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stderr)
        archive = dist / "YouKnow-1.2.3-build.34393416911.2-Linux-x64.tar.gz"
        self.assertEqual(set(dist.iterdir()), {archive, dist / "SHA256SUMS.txt", unrelated})
        self.assertEqual(unrelated.read_text(), "preserve")
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        self.assertEqual((dist / "SHA256SUMS.txt").read_text(), f"{digest}  {archive.name}\n")


class PreviewPublicationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="youknow-preview-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.remote = self.directory / "remote.git"
        self.seed = self.directory / "seed"
        self.checkout = self.directory / "checkout"
        self.previews = self.directory / "previews"
        self.previews.mkdir()
        self.environment = os.environ.copy()
        self.environment.update({"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"})
        self.git(self.directory, "init", "--bare", "--initial-branch=main", str(self.remote))
        self.git(self.directory, "clone", str(self.remote), str(self.seed))
        self.git(self.seed, "config", "user.name", "Fixture")
        self.git(self.seed, "config", "user.email", "fixture@example.invalid")
        self.original = {
            "README.md": b"Original README and peak table\n",
            "Source/engine.cpp": b"original source\n",
            "Docs/audio/demo.wav": b"original demo",
            "Docs/audio/composition/youknow-composition.wav": b"original composition",
            "Docs/audio/frozen-review/take.wav": b"frozen evidence",
            "Docs/screenshots/youknow-standalone.png": b"original screenshot",
        }
        for relative, contents in self.original.items():
            write(self.seed / relative, contents)
        self.git(self.seed, "add", ".")
        self.git(self.seed, "commit", "-m", "Initial fixture")
        self.git(self.seed, "push", "origin", "main")
        self.source_commit = self.git(self.seed, "rev-parse", "HEAD").stdout.strip()
        self.git(self.directory, "clone", str(self.remote), str(self.checkout))
        # Redirect only the test credential URL to the isolated bare repository.
        # The production script and this fixture never contact a real origin.
        self.git(self.checkout, "config", f"url.{self.remote.as_uri()}.insteadOf",
                 "https://x-access-token:test-token@github.com/test/repo.git")
        self.environment.update({
            "GH_TOKEN": "test-token", "GITHUB_REPOSITORY": "test/repo",
            "GITHUB_SHA": self.source_commit, "GITHUB_REF": "refs/heads/main",
            "PREVIEW_DIR": str(self.previews),
        })
        self.rendered = {
            "README.md": b"Original README with refreshed peak table\n",
            "Docs/audio/demo.wav": b"rendered demo",
            "Docs/audio/composition/youknow-composition.wav": b"rendered composition",
            "Docs/screenshots/youknow-standalone.png": b"rendered screenshot",
        }
        self.archives(self.rendered)

    def git(self, cwd, *arguments):
        return subprocess.run(
            ["git", *arguments], cwd=cwd, env=self.environment,
            capture_output=True, text=True, check=True,
        )

    def archives(self, files):
        for archive, screenshot in (("audio-previews.tar.gz", False), ("editor-preview.tar.gz", True)):
            with tarfile.open(self.previews / archive, "w:gz") as output:
                for relative, contents in files.items():
                    if relative.startswith("Docs/screenshots/") != screenshot:
                        continue
                    member = tarfile.TarInfo(relative)
                    member.size = len(contents)
                    output.addfile(member, io.BytesIO(contents))

    def refresh(self):
        result = subprocess.run(
            ["bash", str(SCRIPTS / "refresh-previews.sh")], cwd=self.checkout,
            env=self.environment, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def remote_head(self):
        return self.git(self.remote, "rev-parse", "main").stdout.strip()

    def remote_contents(self, relative):
        return self.git(self.remote, "show", f"main:{relative}").stdout.encode()

    def push_change(self, relative, contents):
        write(self.seed / relative, contents)
        self.git(self.seed, "add", relative)
        self.git(self.seed, "commit", "-m", "Concurrent upstream edit")
        self.git(self.seed, "push", "origin", "main")
        return self.remote_head()

    def test_changed_previews_are_committed_together_without_frozen_evidence(self):
        self.refresh()
        self.assertNotEqual(self.remote_head(), self.source_commit)
        for relative, contents in self.rendered.items():
            self.assertEqual(self.remote_contents(relative), contents)
        self.assertEqual(self.remote_contents("Docs/audio/frozen-review/take.wav"), b"frozen evidence")
        self.assertEqual(self.remote_contents("Source/engine.cpp"), b"original source\n")
        changed = self.git(self.remote, "diff-tree", "--no-commit-id", "--name-only", "-r", "main").stdout
        self.assertEqual(set(changed.splitlines()), set(self.rendered))

    def test_unchanged_previews_do_not_create_a_commit(self):
        self.archives({relative: self.original[relative] for relative in self.rendered})
        self.assertIn("unchanged", self.refresh())
        self.assertEqual(self.remote_head(), self.source_commit)

    def test_composition_refreshes_while_its_frozen_neighbours_do_not(self):
        # The composition is the one maintained file below Docs/audio, and it
        # sits in a subdirectory only so the demo renderer's stale-file sweep
        # cannot reach it. That puts it beside the frozen review evidence, so
        # the two have to be shown moving independently: refreshing only the
        # composition must publish it and leave the frozen take alone.
        composition = "Docs/audio/composition/youknow-composition.wav"
        self.archives({
            "README.md": self.original["README.md"],
            "Docs/audio/demo.wav": self.original["Docs/audio/demo.wav"],
            composition: b"a newly rendered composition",
            "Docs/screenshots/youknow-standalone.png":
                self.original["Docs/screenshots/youknow-standalone.png"],
        })
        self.refresh()
        self.assertEqual(self.remote_contents(composition), b"a newly rendered composition")
        self.assertEqual(self.remote_contents("Docs/audio/frozen-review/take.wav"), b"frozen evidence")
        changed = self.git(self.remote, "diff-tree", "--no-commit-id", "--name-only", "-r", "main").stdout
        self.assertEqual(changed.split(), [composition])

    def test_renamed_demo_removes_the_previous_file(self):
        renamed = dict(self.rendered)
        renamed["Docs/audio/renamed-demo.wav"] = renamed.pop("Docs/audio/demo.wav")
        self.archives(renamed)
        self.refresh()
        tracked = self.git(self.remote, "ls-tree", "-r", "--name-only", "main").stdout.splitlines()
        self.assertNotIn("Docs/audio/demo.wav", tracked)
        self.assertIn("Docs/audio/renamed-demo.wav", tracked)
        self.assertEqual(self.remote_contents("Docs/audio/frozen-review/take.wav"), b"frozen evidence")

    def test_newer_source_prevents_stale_preview_publication(self):
        upstream = self.push_change("Source/engine.cpp", b"newer source\n")
        self.assertIn("Main changed since this render", self.refresh())
        self.assertEqual(self.remote_head(), upstream)
        for relative in self.rendered:
            self.assertEqual(self.remote_contents(relative), self.original[relative])

    def test_newer_readme_prose_is_preserved(self):
        upstream = self.push_change("README.md", b"Updated instructions\n")
        self.assertIn("Main changed since this render", self.refresh())
        self.assertEqual(self.remote_head(), upstream)
        self.assertEqual(self.remote_contents("README.md"), b"Updated instructions\n")

    def test_preview_only_upstream_commit_allows_publication(self):
        upstream = self.push_change("Docs/screenshots/youknow-standalone.png", b"earlier run screenshot")
        self.refresh()
        self.assertNotEqual(self.remote_head(), upstream)
        parent = self.git(self.remote, "rev-parse", "main^").stdout.strip()
        self.assertEqual(parent, upstream)
        for relative, contents in self.rendered.items():
            self.assertEqual(self.remote_contents(relative), contents)


if __name__ == "__main__":
    unittest.main(verbosity=2)
