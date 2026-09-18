#!/usr/bin/env python3
"""Check distribution completeness and preview publication in isolated fixtures."""

import hashlib
import importlib.util
import io
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import textwrap
import unittest
from unittest import mock
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


def workflow_job(workflow: Path, job: str) -> str:
    # Only extract indentation-delimited job/step blocks; execute their real
    # Bash commands below instead of maintaining a parallel copy in the test.
    text = workflow.read_text()
    match = re.search(rf"^  {re.escape(job)}:\n(.*?)(?=^  [A-Za-z0-9_-]+:|\Z)",
                      text, re.MULTILINE | re.DOTALL)
    if match is None:
        raise AssertionError(f"missing workflow job: {job}")
    return match.group(1)


def workflow_step(job: str, name: str) -> str:
    marker = f"      - name: {name}\n"
    if job.count(marker) != 1:
        raise AssertionError(f"expected one workflow step: {name}")
    step = job.split(marker, 1)[1].split("\n      - ", 1)[0]
    if "        run: |\n" in step:
        return textwrap.dedent(step.split("        run: |\n", 1)[1])
    return step.split("        run: ", 1)[1].splitlines()[0]


@unittest.skipUnless(os.name == "posix", "workflow shell fixtures run on POSIX")
class WorkflowGateTests(unittest.TestCase):
    """Verify the actual CI command gates without claiming native host runs."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="youknow-ci-gates-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.bin = self.directory / "bin"
        self.bin.mkdir()
        self.log = self.directory / "commands.jsonl"
        mock_script = self.bin / "mock-tool"
        mock_script.write_text(textwrap.dedent("""\
            #!/usr/bin/env python3
            import json, os, re, subprocess, sys
            from pathlib import Path
            name, args = Path(sys.argv[0]).name, sys.argv[1:]
            with open(os.environ['GATE_LOG'], 'a') as log:
                log.write(json.dumps([name, args]) + '\\n')
            if name == 'xvfb-run':
                assert args[0] == '--auto-servernum'
                sys.exit(subprocess.run(args[1:]).returncode)
            if name == 'ctest':
                pattern = args[args.index('-R') + 1]
                registered = os.environ.get('GATE_REGISTERED',
                    'PluginProcessor,CLAPBundle,VST3Bundle').split(',')
                matches = [suite for suite in registered
                    if re.search(pattern, 'YouKnow.' + suite)]
                if not matches:
                    sys.exit(7 if '--no-tests=error' in args else 0)
                if os.environ.get('GATE_FAIL') in matches:
                    sys.exit(8)
            elif name != 'cmake':
                sys.exit(99)
        """))
        mock_script.chmod(0o755)
        for name in ("cmake", "ctest", "xvfb-run"):
            (self.bin / name).symlink_to(mock_script)
        self.environment = {**os.environ, "PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}",
                            "GATE_LOG": str(self.log)}

    def execute(self, command, **environment):
        self.log.write_text("")
        result = subprocess.run(["bash", "-e", "-o", "pipefail", "-c", command],
                                cwd=self.directory,
                                env={**self.environment, **environment},
                                text=True, capture_output=True, check=False)
        import json
        calls = [json.loads(line) for line in self.log.read_text().splitlines()]
        return result, calls

    def test_each_platform_builds_and_requires_all_three_host_suites(self):
        workflow = SCRIPTS.parent / ".github/workflows/ci.yml"
        suites = ("PluginProcessor", "CLAPBundle", "VST3Bundle")
        targets = {"YouKnowPluginProcessorTests", "YouKnowCLAPBundleSmokeTests",
                   "YouKnowVST3BundleSmokeTests", "YouKnow_VST3", "YouKnow_CLAP",
                   "YouKnow_Standalone"}
        for platform, job_id in (("Linux", "dsp-tests"), ("Windows", "dsp-tests-windows")):
            job = workflow_job(workflow, job_id)
            configure = workflow_step(job, f"Configure {platform} plug-in build")
            result, calls = self.execute(configure)
            self.assertEqual(result.returncode, 0, result.stderr)
            options = calls[0][1]
            for option in ("-DYOUKNOW_BUILD_PLUGIN=ON", "-DYOUKNOW_BUILD_CLAP=ON",
                           "-DBUILD_TESTING=ON"):
                self.assertIn(option, options)
            build_name = f"Build {platform} plug-ins and host-boundary tests"
            result, calls = self.execute(workflow_step(job, build_name))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(targets <= set(calls[0][1]))
            gate_name = f"Test {platform} processor and plug-in host boundaries"
            gate = workflow_step(job, gate_name)
            self.assertLess(job.index(gate_name), job.index(f"Package {platform}"))
            result, calls = self.execute(gate)
            self.assertEqual(result.returncode, 0, result.stderr)
            test_calls = [args for name, args in calls if name == "ctest"]
            self.assertEqual(len(test_calls), 3)
            patterns = {args[args.index("-R") + 1] for args in test_calls}
            self.assertEqual(patterns, {rf"^YouKnow\.{suite}$" for suite in suites})
            for args in test_calls:
                self.assertIn("--no-tests=error", args)
                if platform == "Windows":
                    self.assertEqual(args[args.index("-C") + 1], "Release")
            self.assertEqual(sum(name == "xvfb-run" for name, _ in calls),
                             3 if platform == "Linux" else 0)
            # Any failing or missing individual suite must reject the gate,
            # even when all other registrations exist and return success.
            for suite in suites:
                with self.subTest(platform=platform, failing=suite):
                    result, _ = self.execute(gate, GATE_FAIL=suite)
                    self.assertNotEqual(result.returncode, 0)
                with self.subTest(platform=platform, missing=suite):
                    registered = ",".join(item for item in suites if item != suite)
                    result, _ = self.execute(gate, GATE_REGISTERED=registered)
                    self.assertNotEqual(result.returncode, 0)
        linux = workflow_job(workflow, "dsp-tests")
        dependencies = workflow_step(linux, "Install JUCE Linux dependencies")
        self.assertIn("xvfb", dependencies)
        self.assertIn("xauth", dependencies)

    def test_release_waits_for_the_same_source_cross_platform_ci(self):
        release = SCRIPTS.parent / ".github/workflows/release.yml"
        gate = workflow_job(release, "ci")
        self.assertRegex(gate, r"(?m)^    uses: \./\.github/workflows/ci\.yml$")
        self.assertNotRegex(gate, r"(?m)^    (?:if|continue-on-error):")
        publisher = workflow_job(release, "macos")
        self.assertRegex(publisher, r"(?m)^    needs: ci$")
        # With no overriding job condition, Actions' default success() gate
        # blocks both a failed and a skipped dependency before credentials.
        self.assertNotRegex(publisher, r"(?m)^    (?:if|continue-on-error):")
        ci = SCRIPTS.parent / ".github/workflows/ci.yml"
        self.assertRegex(ci.read_text(), r"(?m)^  workflow_call:$")
        for job in ("dsp-tests", "plugin-macos", "dsp-tests-windows"):
            header = workflow_job(ci, job).split("    steps:", 1)[0]
            self.assertNotRegex(header, r"(?m)^    (?:if|continue-on-error):")


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

    def test_failed_repack_preserves_valid_archive_and_checksum(self):
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stderr)
        dist = self.build / "dist"
        archive = next(dist.glob("*.zip"))
        old_archive = archive.read_bytes()
        old_checksum = (dist / "SHA256SUMS.txt").read_bytes()
        spec = importlib.util.spec_from_file_location("fixture_windows_packager", self.script)
        packager = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(packager)
        # Exercise actual I/O failures, not just input preflight rejection.
        # Both rebuilding the current version and starting a new version must
        # leave the preceding distribution intact if preparation fails.
        for build_number in (BUILD_NUMBER, "34393416911.2"):
            (self.build / "CMakeCache.txt").write_bytes(
                version_cache().replace(BUILD_NUMBER.encode(), build_number.encode())
            )
            faults = (
                mock.patch.object(packager.zipfile.ZipFile, "write",
                                  side_effect=OSError("simulated ZIP write failure")),
                mock.patch.object(packager.zipfile.ZipFile, "testzip",
                                  return_value="damaged member"),
                mock.patch.object(packager.Path, "write_text",
                                  side_effect=OSError("simulated checksum write failure")),
            )
            for index, fault in enumerate(faults):
                with self.subTest(build_number=build_number, fault=index):
                    with fault, self.assertRaises((OSError, ValueError)):
                        packager.package(self.build)
                    self.assertEqual(archive.read_bytes(), old_archive)
                    self.assertEqual((dist / "SHA256SUMS.txt").read_bytes(), old_checksum)
                    self.assertEqual(set(dist.iterdir()), {archive, dist / "SHA256SUMS.txt"})


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

    def test_missing_or_empty_binary_preserves_previous_distribution(self):
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stderr)
        old_archive = self.archive.read_bytes()
        checksums = self.archive.parent / "SHA256SUMS.txt"
        old_checksums = checksums.read_bytes()
        # A subsequent build must fail before deleting the complete previous
        # archive, even if its bundle folder and executable bits remain.
        (self.project / "build-dsp/CMakeCache.txt").write_bytes(
            version_cache().replace(BUILD_NUMBER.encode(), b"34393416911.2")
        )
        for relative in self.payload:
            path = self.artifacts / relative
            original = path.read_bytes()
            mode = path.stat().st_mode
            for empty in (False, True):
                with self.subTest(binary=relative, empty=empty):
                    if empty:
                        path.write_bytes(b"")
                    else:
                        path.unlink()
                    result = self.package()
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("missing or empty Linux x64 binary", result.stderr)
                    self.assertEqual(self.archive.read_bytes(), old_archive)
                    self.assertEqual(checksums.read_bytes(), old_checksums)
                    self.assertEqual(list(self.archive.parent.glob("*.tar.gz")), [self.archive])
                    path.write_bytes(original)
                    path.chmod(mode)

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
        self.dist = self.directory / "dist"
        self.dist.mkdir()
        self.environment = os.environ.copy()
        self.environment.update({"GIT_CONFIG_GLOBAL": os.devnull, "GIT_CONFIG_NOSYSTEM": "1"})
        self.git(self.directory, "init", "--bare", "--initial-branch=main", str(self.remote))
        self.git(self.directory, "clone", str(self.remote), str(self.seed))
        self.git(self.seed, "config", "user.name", "Fixture")
        self.git(self.seed, "config", "user.email", "fixture@example.invalid")
        self.original = {
            # The repository's own ignore rules: they decide what `git add`
            # takes from dist/, and they ignore built packages by extension.
            ".gitignore": (SCRIPTS.parent / ".gitignore").read_bytes(),
            "README.md": b"Original README and peak table\n",
            "Source/engine.cpp": b"original source\n",
            "Docs/audio/demo.wav": b"original demo",
            "Docs/audio/composition/youknow-composition.wav": b"original composition",
            "Docs/audio/frozen-review/take.wav": b"frozen evidence",
            "Docs/screenshots/youknow-standalone.png": b"original screenshot",
        }
        # The packages of an earlier main build, committed by an earlier refresh.
        self.original_packages = self.package_set("1.1", b"old ")
        self.original.update(self.original_packages)
        self.original["dist/youknow-standalone.png"] = b"old screenshot"
        self.original["dist/BUILD.txt"] = b"source=earlier\nversion=1.2.3-build.1.1\n"
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
            "PREVIEW_DIR": str(self.previews), "DIST_DIR": str(self.dist),
        })
        self.rendered = {
            "README.md": b"Original README with refreshed peak table\n",
            "Docs/audio/demo.wav": b"rendered demo",
            "Docs/audio/composition/youknow-composition.wav": b"rendered composition",
            "Docs/screenshots/youknow-standalone.png": b"rendered screenshot",
        }
        self.archives(self.rendered)
        # This run's packages, downloaded one platform per directory with the
        # checksum file each packaging step wrote.
        self.packages = self.package_set("2.1", b"new ", write=True)

    def package_set(self, build, tag, write=False):
        names = {
            "macos": [f"YouKnow-1.2.3-build.{build}-macOS-universal.pkg",
                      f"YouKnow-1.2.3-build.{build}-macOS-universal.zip"],
            "windows": [f"YouKnow-1.2.3-build.{build}-Windows-x64.zip"],
            "linux": [f"YouKnow-1.2.3-build.{build}-Linux-x64.tar.gz"],
        }
        files = {}
        for platform, archives in names.items():
            sums = ""
            for name in archives:
                contents = tag + name.encode()
                files[f"dist/{platform}/{name}"] = contents
                sums += f"{hashlib.sha256(contents).hexdigest()}  {name}\n"
            files[f"dist/{platform}/SHA256SUMS.txt"] = sums.encode()
        if write:
            for relative, contents in files.items():
                write_path = self.dist / relative.removeprefix("dist/")
                write_path.parent.mkdir(parents=True, exist_ok=True)
                write_path.write_bytes(contents)
        return files

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

    def refresh_fails(self):
        result = subprocess.run(
            ["bash", str(SCRIPTS / "refresh-previews.sh")], cwd=self.checkout,
            env=self.environment, capture_output=True, text=True, check=False,
        )
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def remote_head(self):
        return self.git(self.remote, "rev-parse", "main").stdout.strip()

    def remote_tracked(self):
        return self.git(self.remote, "ls-tree", "-r", "--name-only", "main").stdout.splitlines()

    def push_source_change(self):
        head = self.push_change("Source/engine.cpp", b"newer source\n")
        self.environment["GITHUB_SHA"] = head
        return head

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
        # The source did not change since the committed packages, so a rebuild
        # of the same main (the nightly run) keeps that set rather than
        # stacking another build number's files into the history.
        for relative, contents in self.original_packages.items():
            self.assertEqual(self.remote_contents(relative), contents)
        self.assertEqual(self.remote_contents("dist/BUILD.txt"), self.original["dist/BUILD.txt"])

    def test_packages_follow_a_source_change(self):
        head = self.push_source_change()
        self.refresh()
        for relative, contents in {**self.rendered, **self.packages}.items():
            self.assertEqual(self.remote_contents(relative), contents)
        self.assertEqual(self.remote_contents("dist/youknow-standalone.png"),
                         self.rendered["Docs/screenshots/youknow-standalone.png"])
        self.assertEqual(self.remote_contents("dist/BUILD.txt"),
                         f"source={head}\nversion=1.2.3-build.2.1\n".encode())
        tracked = self.remote_tracked()
        for relative in self.original_packages:
            if not relative.endswith("SHA256SUMS.txt"):
                self.assertNotIn(relative, tracked)
        self.assertEqual(self.remote_contents("Docs/audio/frozen-review/take.wav"), b"frozen evidence")
        changed = self.git(self.remote, "diff-tree", "--no-commit-id", "--name-only", "-r", "main").stdout
        expected = set(self.rendered) | set(self.packages) | {"dist/youknow-standalone.png", "dist/BUILD.txt"}
        expected |= {relative for relative in self.original_packages if not relative.endswith("SHA256SUMS.txt")}
        self.assertEqual(set(changed.splitlines()), expected)

    def test_corrupt_package_never_reaches_main(self):
        head = self.push_source_change()
        archive = next(self.dist.glob("linux/*.tar.gz"))
        archive.write_bytes(b"truncated download")
        self.assertIn("FAILED", self.refresh_fails())
        self.assertEqual(self.remote_head(), head)
        for relative, contents in self.original_packages.items():
            self.assertEqual(self.remote_contents(relative), contents)

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
