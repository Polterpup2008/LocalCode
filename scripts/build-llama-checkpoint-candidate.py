#!/usr/bin/env python3
"""Build the experimental checkpoint patch in a new, isolated source tree."""

import argparse
import hashlib
import json
import os
import pathlib
import re
import stat
import subprocess
import sys
import zipfile

REVISION = "cb300598d5f90189cb69d2702f4930aaf99d32a2"
PATCH_TEXT_SHA256 = "FB454138DDAE8825CD2DED946A7E44096BF40A1DFF562EED47A82F3FAF1325BB"
PATCHED_FILES = {
    "common/common.h": "C68326FC429528A87C369BDCC7AD900EC5C5C4CADB22B264467C403DE831C720",
    "common/common.cpp": "DC696007EA906633A981AEE598D6D25D44506F8D2E48187FF7E88E1893CBF79A",
    "common/shared_checkpoint_bytes.h": "23FD648F8F5E7BB408E6DD8ACAE6E13A73D76AE9CE0774BB3F8761AFA258268B",
    "tools/server/server-context.cpp": "5DF485A8BBAE42D0A13D5916BF84E83D2946FB98CD21DB7D5A9BD2CCA926C55D",
}
CPU_PATCH_TEXT_SHA256 = "0B404530501DA2B5BD7652DAF5E0B1921603D963D5122478996B88685BAE1C8F"
CPU_PATCHED_FILES = PATCHED_FILES | {
    "ggml/src/ggml-cpu/arch/x86/cpu-feats.cpp": "14366CF289C869E68B9D8A75ED906F2BDC8D0D01B9DBD10E1851D68EE11BD9C9",
    "ggml/src/ggml-cpu/arch/x86/os-xstate.h": "55CAD822F03ACA1243BAE55C9EB2C33B30A80BABE2D1F8C977012F54F641AEE0",
    "tools/server/server.cpp": "88A71E1F98418C349C28037712CBDA48B9939C2651AD835ABFD73DF0CCE04850",
}
LIFECYCLE_PATCH_TEXT_SHA256 = "646E973A74B8225A1130F2D7BA9D68424E45EE1D6345FBBA56FC9418B2F7BD2B"
LIFECYCLE_PATCHED_FILES = {
    "tools/server/server.cpp": "88A71E1F98418C349C28037712CBDA48B9939C2651AD835ABFD73DF0CCE04850",
}
PATCH = pathlib.Path(__file__).resolve().parents[1] / "docs/performance/proofs/checkpoint-sharing-b10655.patch"


def run(args, cwd=None, env=None):
    subprocess.run([str(arg) for arg in args], cwd=cwd, env=env, check=True)


def sha(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest().upper()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", required=True, type=pathlib.Path, help="Existing llama.cpp Git checkout; never modified")
    parser.add_argument("--output", required=True, type=pathlib.Path, help="New directory; existing paths are refused")
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--control", action="store_true", help="Prepare/build the same revision without the patch")
    parser.add_argument("--variant", choices=("checkpoint", "cpu-dispatch"), default="checkpoint", help="Sharing only, or shared CPU runtime with OS-aware instruction selection and CLI lifecycle repair")
    parser.add_argument("--patch", type=pathlib.Path, help="Verified variant patch; explicit path for a standalone helper copy")
    parser.add_argument("--lifecycle-patch", type=pathlib.Path, help="Verified server lifetime patch; otherwise taken beside the selected sharing patch")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--target", action="append", help="Build target; repeatable; defaults to llama-server")
    parser.add_argument("cmake_args", nargs=argparse.REMAINDER, help="Extra CMake configure arguments after --; choose backend/toolchain explicitly")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    extra = args.cmake_args[1:] if args.cmake_args[:1] == ["--"] else args.cmake_args
    for index, item in enumerate(extra[:-1]):
        if re.fullmatch(r"-D[^=]+=[A-Za-z]:", item) and extra[index + 1].startswith(("/", "\\")):
            parser.error("A Windows CMake path was split by the shell; quote the entire -DNAME=C:/path argument")
    upstream = args.upstream.resolve(strict=True)
    patch = (args.patch or (PATCH if args.variant == "checkpoint" else PATCH.with_name("checkpoint-cpu-dispatch-b10655.patch"))).resolve()
    expected_patch = PATCH_TEXT_SHA256 if args.variant == "checkpoint" else CPU_PATCH_TEXT_SHA256
    expected_files = PATCHED_FILES if args.variant == "checkpoint" else CPU_PATCHED_FILES
    output = args.output.resolve()
    if output.exists():
        parser.error("--output must not exist; old results are never replaced")
    revision = subprocess.check_output(["git", "-C", str(upstream), "rev-parse", REVISION + "^{commit}"], text=True).strip()
    if revision != REVISION:
        raise RuntimeError("Pinned revision is unavailable")
    if not args.control and not patch.is_file():
        raise FileNotFoundError(patch)
    if not args.control:
        if patch.stat().st_size > 65536:
            parser.error("The pinned checkpoint patch exceeds its expected size")
        normalized = patch.read_text(encoding="utf-8")
        if hashlib.sha256(normalized.encode("utf-8")).hexdigest().upper() != expected_patch:
            parser.error("--patch must be the verified checkpoint patch for the pinned revision")
    # Checkpoint-only and control builds must not start the GC thread before
    # CLI parsing: --version/help can otherwise hang during shared-DLL teardown.
    # The CPU-dispatch candidate already includes exactly this lifetime repair.
    lifecycle = None
    verified_files = {} if args.control else dict(expected_files)
    if args.control or args.variant == "checkpoint":
        lifecycle = (args.lifecycle_patch or patch.with_name("checkpoint-server-lifecycle-b10655.patch")).resolve(strict=True)
        if lifecycle.stat().st_size > 8192:
            parser.error("The pinned server lifetime patch exceeds its expected size")
        lifecycle_text = lifecycle.read_text(encoding="utf-8")
        if hashlib.sha256(lifecycle_text.encode("utf-8")).hexdigest().upper() != LIFECYCLE_PATCH_TEXT_SHA256:
            parser.error("--lifecycle-patch must be the verified server lifetime patch")
        verified_files.update(LIFECYCLE_PATCHED_FILES)
    output.mkdir(parents=True)
    archive = output / "source.zip"
    run(["git", "-C", upstream, "archive", "--format=zip", "--output", archive, REVISION])
    source = output / "source"
    source.mkdir()
    with zipfile.ZipFile(archive) as bundle:
        for info in bundle.infolist():
            relative = pathlib.PurePosixPath(info.filename)
            if relative.is_absolute() or ".." in relative.parts or ":" in info.filename or "\\" in info.filename:
                raise RuntimeError("Unexpected archive path: " + info.filename)
            if stat.S_ISLNK(info.external_attr >> 16):
                raise RuntimeError("Symlink archive entries are unsupported")
            if not (source / info.filename).resolve().is_relative_to(source):
                raise RuntimeError("Archive entry escaped the isolated source directory")
        bundle.extractall(source)
    if not args.control:
        # Absolute paths are permitted only after constructing the isolated tree
        # above. Git's normal repository-prefix checks do not apply to this copy.
        patch_args = ["git", "apply", "--unsafe-paths", "--directory=" + source.as_posix(), patch]
        patch_env = os.environ.copy()
        patch_env["GIT_CEILING_DIRECTORIES"] = str(output)
        run(patch_args[:2] + ["--check"] + patch_args[2:], cwd=source, env=patch_env)
        run(patch_args, cwd=source, env=patch_env)
        for name, expected in expected_files.items():
            actual = hashlib.sha256((source / name).read_text(encoding="utf-8").encode("utf-8")).hexdigest().upper()
            if actual != expected:
                raise RuntimeError("Checkpoint patch verification failed for " + name)
    if lifecycle:
        lifecycle_args = ["git", "apply", "--unsafe-paths", "--directory=" + source.as_posix(), lifecycle]
        lifecycle_env = os.environ.copy()
        lifecycle_env["GIT_CEILING_DIRECTORIES"] = str(output)
        run(lifecycle_args[:2] + ["--check"] + lifecycle_args[2:], cwd=source, env=lifecycle_env)
        run(lifecycle_args, cwd=source, env=lifecycle_env)
        for name, expected in LIFECYCLE_PATCHED_FILES.items():
            actual = hashlib.sha256((source / name).read_text(encoding="utf-8").encode("utf-8")).hexdigest().upper()
            if actual != expected:
                raise RuntimeError("Server lifetime patch verification failed for " + name)
    variant = "control" if args.control else ("checkpoint-cpu-dispatch" if args.variant == "cpu-dispatch" else "checkpoint-candidate")
    variant_options = (["-DBUILD_SHARED_LIBS=ON", "-DGGML_BACKEND_DL=ON", "-DGGML_CPU_ALL_VARIANTS=ON",
                        "-DGGML_NATIVE=OFF", "-DGGML_CUDA=OFF"] if args.variant == "cpu-dispatch" else [])
    configure = [args.cmake, "-S", source, "-B", output / "build", "-DCMAKE_BUILD_TYPE=Release",
                 "-DLLAMA_BUILD_NUMBER=10655", "-DLLAMA_BUILD_COMMIT=" + REVISION[:9] + "-" + variant] + variant_options + extra
    build = [args.cmake, "--build", output / "build", "--config", "Release", "--parallel", str(args.jobs),
             "--target"] + (args.target or ["llama-server"])
    record = {"revision": REVISION, "variant": variant, "experimental": True, "source_archive_sha256": sha(archive),
              "patch_sha256": None if args.control else sha(patch), "lifecycle_patch_sha256": sha(lifecycle) if lifecycle else None, "source": str(source),
              "configure": [str(arg) for arg in configure], "build": [str(arg) for arg in build],
              "status": "prepared", "verified_patched_files": verified_files,
              "owner_settings_changed": False,
              "note": "Choose compatible backend/ISA/TLS settings. This does not download weights, replace a runtime, change a profile, or certify performance."}
    manifest = output / "candidate-build.json"
    def save():
        manifest.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    save()
    if not args.prepare_only:
        build_env = os.environ.copy()
        # The archived source has no Git checkout. Do not label it with the
        # unrelated parent application's commit when CMake probes Git.
        build_env["GIT_CEILING_DIRECTORIES"] = str(output)
        try:
            run(configure, env=build_env)
            run(build, env=build_env)
            record["status"] = "built"
        except Exception as exc:
            record.update(status="failed", error=type(exc).__name__ + ": " + str(exc))
            raise
        finally:
            save()
    print(json.dumps({"status": record["status"], "manifest": str(manifest), "experimental": True}))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(1)
