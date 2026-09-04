#!/usr/bin/env python3
"""Package a tex_codec release, and optionally publish it on GitHub.

Produces tex_codec_v<version>.zip laid out as:

    wasm/tex_codec.js, .wasm, tex_codec_api.mjs, .d.ts
    cli/texc.exe, tex_codec.lib
    include/tex_codec.h
    README.md
    CHANGELOG.md

The version comes from include/tex_codec.h, the single source of truth, and
the script refuses to package if the built binaries disagree with it - so a
zip can never ship a stale artifact under a new version number.

Usage:
    python tools/make_release.py                # build the zip
    python tools/make_release.py --publish      # ...and create the GitHub
                                                #    release from CHANGELOG
    python tools/make_release.py --publish --draft
    python tools/make_release.py --out build    # choose output directory

Publishing needs either the `gh` CLI (authenticated) or a GITHUB_TOKEN
environment variable with `repo` scope. The token is read from the
environment and never printed.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path
from urllib import error, request

ROOT = Path(__file__).resolve().parent.parent


def fail(msg):
    print("error: " + msg, file=sys.stderr)
    sys.exit(1)


def read_version():
    text = (ROOT / "include" / "tex_codec.h").read_text(encoding="utf-8")
    parts = []
    for name in ("MAJOR", "MINOR", "PATCH"):
        m = re.search(r"#define\s+TEXC_VERSION_" + name + r"\s+(\d+)", text)
        if not m:
            fail("TEXC_VERSION_" + name + " not found in include/tex_codec.h")
        parts.append(m.group(1))
    return ".".join(parts)


def changelog_section(version):
    """The '## [version]' block, used verbatim as the release notes."""
    lines = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8").splitlines()
    start = None
    for i, line in enumerate(lines):
        if line.startswith("## [" + version + "]"):
            start = i
            break
    if start is None:
        return None
    body = []
    for line in lines[start + 1:]:
        if line.startswith("## ["):
            break
        body.append(line)
    return "\n".join(body).strip()


def check_binary_version(version):
    """Refuse to ship artifacts that disagree with the header."""
    problems = []

    exe = ROOT / "build" / "Release" / "texc.exe"
    if exe.exists():
        try:
            out = subprocess.run([str(exe), "version", "--short"],
                                 capture_output=True, text=True, timeout=30)
            got = out.stdout.strip()
            if got and got != version:
                problems.append("texc.exe reports " + got)
        except Exception as exc:  # noqa: BLE001
            problems.append("could not run texc.exe (" + str(exc) + ")")

    api = ROOT / "dist" / "tex_codec_api.mjs"
    if api.exists():
        m = re.search(r'export const VERSION = "([^"]+)"',
                      api.read_text(encoding="utf-8"))
        if m and m.group(1) != version:
            problems.append("dist/tex_codec_api.mjs reports " + m.group(1))

    if problems:
        fail("built artifacts disagree with include/tex_codec.h (" + version
             + "): " + "; ".join(problems)
             + "\n       rebuild, and re-run wasm/build_wasm.sh, first")


def collect():
    """(path_on_disk, name_in_zip) for everything shipped."""
    return [
        (ROOT / "dist" / "tex_codec.js", "wasm/tex_codec.js"),
        (ROOT / "dist" / "tex_codec.wasm", "wasm/tex_codec.wasm"),
        (ROOT / "dist" / "tex_codec_api.mjs", "wasm/tex_codec_api.mjs"),
        (ROOT / "dist" / "tex_codec_api.d.ts", "wasm/tex_codec_api.d.ts"),
        (ROOT / "build" / "Release" / "texc.exe", "cli/texc.exe"),
        (ROOT / "build" / "Release" / "tex_codec.lib", "cli/tex_codec.lib"),
        # the .lib is unusable without its header, so ship that too
        (ROOT / "include" / "tex_codec.h", "include/tex_codec.h"),
        (ROOT / "README.md", "README.md"),
        (ROOT / "CHANGELOG.md", "CHANGELOG.md"),
    ]


def build_zip(version, out_dir):
    items = collect()
    missing = [dst for src, dst in items if not src.exists()]
    if missing:
        fail("missing build outputs: " + ", ".join(missing)
             + "\n       build natively (cmake --build build --config Release)"
               " and run wasm/build_wasm.sh first")

    out_dir.mkdir(parents=True, exist_ok=True)
    zip_path = out_dir / ("tex_codec_v" + version + ".zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for src, dst in items:
            z.write(src, dst)

    raw = sum(src.stat().st_size for src, _ in items)
    print("packaged " + str(zip_path))
    print("  " + str(len(items)) + " files, " + str(raw // 1024)
          + " KB -> " + str(zip_path.stat().st_size // 1024) + " KB")
    for _, dst in items:
        print("    " + dst)
    return zip_path


def git(*args):
    return subprocess.run(["git"] + list(args), capture_output=True,
                          text=True, cwd=ROOT).stdout.strip()


def repo_slug():
    m = re.search(r"github\.com[:/]+([^/]+/[^/.]+)",
                  git("remote", "get-url", "origin"))
    return m.group(1) if m else None


def find_gh():
    """Path to the gh CLI, or None.

    Falls back to the standard install locations because a shell opened
    before gh was installed will not have it on PATH yet - otherwise a
    fresh `winget install` looks like it did not work.
    """
    found = shutil.which("gh")
    if found:
        return found
    candidates = [
        Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
        / "GitHub CLI" / "gh.exe",
        Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
        / "GitHub CLI" / "gh.exe",
        Path(os.environ.get("LOCALAPPDATA", "")) / "GitHubCLI" / "bin"
        / "gh.exe",
        Path("/usr/bin/gh"),
        Path("/usr/local/bin/gh"),
        Path("/opt/homebrew/bin/gh"),
    ]
    for c in candidates:
        try:
            if c.is_file():
                return str(c)
        except OSError:
            pass
    return None


def publish(version, zip_path, notes, draft):
    tag = "v" + version

    if git("status", "--porcelain", "--untracked-files=no"):
        fail("working tree has uncommitted changes - commit before releasing")
    slug = repo_slug()
    if not slug:
        fail("could not determine the GitHub repository from remote 'origin'")
    print("publishing " + tag + " to " + slug + " ...")

    # Preferred path: the gh CLI, which handles authentication itself.
    gh = find_gh()
    if gh:
        cmd = [gh, "release", "create", tag, str(zip_path),
               "--title", "tex_codec " + version, "--notes", notes]
        if draft:
            cmd.append("--draft")
        if subprocess.run(cmd, cwd=ROOT).returncode != 0:
            fail("gh release create failed")
        print("released " + tag + " via gh")
        return

    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if not token:
        fail("no GitHub credentials found. Either:\n"
             "\n"
             "  A) install the GitHub CLI, which remembers the login:\n"
             "       winget install --id GitHub.cli\n"
             "       gh auth login\n"
             "     (reopen the shell afterwards so gh lands on PATH)\n"
             "\n"
             "  B) set a token in THIS shell, then re-run:\n"
             "       PowerShell:  $env:GITHUB_TOKEN = '<token>'\n"
             "       bash:        export GITHUB_TOKEN=<token>\n"
             "     create one at https://github.com/settings/tokens with\n"
             "       classic PAT:  the 'repo' scope\n"
             "       fine-grained: 'Contents: Read and write' on "
             + (slug or "the repository"))

    def api(url, data=None, method="GET", ctype="application/json"):
        req = request.Request(url, data=data, method=method)
        req.add_header("Authorization", "Bearer " + token)
        req.add_header("Accept", "application/vnd.github+json")
        req.add_header("X-GitHub-Api-Version", "2022-11-28")
        req.add_header("User-Agent", "tex_codec-release")
        if data is not None:
            req.add_header("Content-Type", ctype)
        try:
            with request.urlopen(req) as resp:
                return json.loads(resp.read().decode() or "{}")
        except error.HTTPError as exc:
            fail("GitHub API " + str(exc.code) + " on " + url + ": "
                 + exc.read().decode()[:400])

    payload = json.dumps({
        "tag_name": tag,
        "name": "tex_codec " + version,
        "body": notes,
        "draft": bool(draft),
    }).encode()
    rel = api("https://api.github.com/repos/" + slug + "/releases",
              data=payload, method="POST")

    upload = rel["upload_url"].split("{")[0] + "?name=" + zip_path.name
    api(upload, data=zip_path.read_bytes(), method="POST",
        ctype="application/zip")
    print("released " + tag + ": " + rel.get("html_url", ""))


def main():
    ap = argparse.ArgumentParser(
        description="Package (and optionally publish) a tex_codec release.")
    ap.add_argument("--publish", action="store_true",
                    help="create a GitHub release using the CHANGELOG notes")
    ap.add_argument("--draft", action="store_true",
                    help="with --publish, create the release as a draft")
    ap.add_argument("--out", default=".",
                    help="directory for the zip (default: repo root)")
    args = ap.parse_args()

    version = read_version()
    print("tex_codec " + version + " (from include/tex_codec.h)")
    check_binary_version(version)
    zip_path = build_zip(version, (ROOT / args.out).resolve())

    notes = changelog_section(version)
    if args.publish:
        if not notes:
            fail("CHANGELOG.md has no '## [" + version + "]' section to use as"
                 " release notes")
        publish(version, zip_path, notes, args.draft)
    elif notes:
        print("\nCHANGELOG.md [" + version + "] will be the release notes -"
              " add --publish to create the GitHub release")
    else:
        print("\nnote: CHANGELOG.md has no [" + version + "] section yet;"
              " --publish would refuse until it does")


if __name__ == "__main__":
    main()
