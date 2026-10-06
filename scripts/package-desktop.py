#!/usr/bin/env python3
"""Package existing Linux/Windows products without changing their host identities."""
import argparse
from pathlib import Path
import os
import shutil
import tarfile
import tempfile
import zipfile
from distribution import PRODUCT, PROJECT, identity, output_dir, manifest, require_new_destination


def package(build, platform, config="Release"):
    formats=["VST3", "Standalone"]
    if PRODUCT=="YouKnow": formats.append("CLAP")
    artifacts=build/(PRODUCT+"_artefacts")/config
    machine="x86_64-linux" if platform=="Linux" else "x86_64-win"
    binary=PRODUCT+".so" if platform=="Linux" else PRODUCT+".vst3"
    required=[f"VST3/{PRODUCT}.vst3/Contents/{machine}/{binary}",
              "Standalone/"+PRODUCT+(".exe" if platform=="Windows" else "")]
    if "CLAP" in formats: required.append("CLAP/"+PRODUCT+".clap")
    for relative in required:
        f=artifacts/relative
        if not f.is_file() or not f.stat().st_size:
            raise ValueError("missing or empty "+platform+" x64 binary: "+str(f))
    if platform=="Linux" and not os.access(artifacts/required[1],os.X_OK):
        raise ValueError("standalone binary is not executable")
    record=identity(build,config,formats)
    files={f.relative_to(artifacts).as_posix(): f for f in (artifacts/"VST3"/(PRODUCT+".vst3")).rglob("*") if f.is_file()}
    for relative in required[1:]: files[relative]=artifacts/relative
    docs=["LICENSE","THIRD_PARTY_NOTICES.md","ThirdParty/JUCE-LICENSE.md"]
    if PRODUCT=="YouKnow":
        docs += ["PRIVACY.md","ThirdParty/CLAP-LICENSE.md","INSTALL_MACOS.md","INSTALL_WINDOWS.md","INSTALL_LINUX.md"]
        files["README.md"]=PROJECT/"USER_GUIDE.md"
        files["JUCE-LICENSE.md"]=PROJECT/"ThirdParty/JUCE-LICENSE.md"
    else:
        docs += ["ThirdParty/CC0-1.0.txt","ThirdParty/Eastman-E1D-README.md","ThirdParty/Shinyguitar-README.txt","Assets/SampleBank/manifest.json"]
        files["README.md"]=PROJECT/"README.md"
    for doc in docs: files[doc]=PROJECT/doc
    for f in files.values():
        if not f.is_file() or not f.stat().st_size: raise ValueError("missing or empty distribution document: "+str(f))
    dist=output_dir(record,platform,"x64");require_new_destination(dist);dist.mkdir(parents=True,exist_ok=True)
    stem=PRODUCT+"-"+record["distribution_version"]+"-"+platform+"-x64"
    archive=dist/(stem+(".tar.gz" if platform=="Linux" else ".zip"))
    with tempfile.TemporaryDirectory(prefix=".package-",dir=dist) as work:
        staging=Path(work); staged=staging/archive.name
        if platform=="Linux":
            with tarfile.open(staged,"w:gz") as output:
                for relative,f in sorted(files.items()): output.add(f,arcname=relative,recursive=False)
            with tarfile.open(staged) as output: output.getmembers()
        else:
            with zipfile.ZipFile(staged,"w",compression=zipfile.ZIP_DEFLATED) as output:
                for relative,f in sorted(files.items()): output.write(f,relative)
            with zipfile.ZipFile(staged) as output:
                if output.testzip() is not None: raise ValueError("corrupt archive")
        manifest(record,staging,platform,"x64",[staged])
        for f in staging.iterdir(): f.replace(dist/f.name)
    return archive

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir",type=Path,required=True)
    parser.add_argument("--platform",choices=["Linux","Windows"],required=True)
    parser.add_argument("--config",default="Release")
    args=parser.parse_args();print(package(args.build_dir.resolve(),args.platform,args.config))
