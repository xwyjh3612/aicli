"""Prepare or publish both signed installers and the public update manifest.

Default: prepare files locally. --publish uploads a draft, then publishes only
after all five assets have arrived. Credentials stay out of logs and artifacts.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
REPO = "xwyjh3612/aicli"


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def checksum(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def prepare(tag):
    if not re.fullmatch(r"[A-Za-z0-9_-][A-Za-z0-9._-]*", tag):
        raise ValueError("Use a release tag containing only letters, digits, dots, underscores and hyphens")
    identity = ET.parse(ROOT / "desktop/package/AppxManifest.xml").getroot().find(
        "{http://schemas.microsoft.com/appx/manifest/foundation/windows10}Identity")
    windows_version = identity.attrib["Version"]
    gradle = (ROOT / "android-app/app/build.gradle").read_text(encoding="utf-8")
    android_version = re.search(r'versionName\s+"([0-9.]+)"', gradle)[1]
    android_code = int(re.search(r"versionCode\s+(\d+)", gradle)[1])
    manifest = {"schemaVersion": 1}
    assets = []
    for platform, version, name in (
        ("windows", windows_version, f"AI-CLI-Setup-{windows_version}-x64.exe"),
        ("android", android_version, f"AI-CLI-Android-{android_version}-release.apk"),
    ):
        file = ROOT / "release" / name
        if not file.is_file() or file.stat().st_size == 0:
            raise ValueError(f"Build the current release first: {file}")
        digest = checksum(file)
        hash_file = file.with_name(name + ".sha256")
        if not hash_file.is_file() or hash_file.read_text().split()[0].lower() != digest:
            raise ValueError(f"Build checksum missing or stale: {file}")
        manifest[platform] = {
            "version": version,
            "url": f"https://github.com/{REPO}/releases/download/{tag}/{name}",
            "sha256": digest,
            "size": file.stat().st_size,
        }
        if platform == "android":
            manifest[platform]["versionCode"] = android_code
        assets.extend([file, hash_file])
    output = ROOT / "release/update-manifest.json"
    output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    assets.append(output)
    return assets


def credential():
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    if token:
        return token
    env = {**os.environ, "GIT_TERMINAL_PROMPT": "0", "GCM_INTERACTIVE": "never"}
    result = subprocess.run(["git", "credential", "fill"],
                            input="protocol=https\nhost=github.com\n\n", text=True,
                            capture_output=True, cwd=ROOT, env=env, check=False)
    fields = dict(line.split("=", 1) for line in result.stdout.splitlines() if "=" in line)
    if not fields.get("password"):
        raise ValueError("Authenticate Git with GitHub or set GH_TOKEN before publishing")
    return fields["password"]


class GitHub:
    def __init__(self):
        self.token = credential()

    def request(self, route, method="GET", data=None, content_type="application/json"):
        url = route if route.startswith("https://uploads.github.com/") else "https://api.github.com/" + route
        if data is not None and not isinstance(data, bytes):
            data = json.dumps(data).encode()
        request = urllib.request.Request(url, method=method, data=data, headers={
            "Authorization": "Bearer " + self.token,
            "Accept": "application/vnd.github+json", "Content-Type": content_type,
            "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "AiCli-release-publisher",
        })
        with urllib.request.urlopen(request, timeout=300) as response:
            body = response.read()
            return json.loads(body) if body else None


def publish(tag, notes, assets):
    if git("status", "--porcelain", "--untracked-files=no"):
        raise ValueError("Commit source changes before publishing")
    github = GitHub()
    repo = github.request(f"repos/{REPO}")
    if repo["private"]:
        raise ValueError("The updater requires a public repository")
    head = git("rev-parse", "HEAD")
    remote = github.request(f"repos/{REPO}/commits/{repo['default_branch']}")
    if remote["sha"] != head:
        raise ValueError("Push the exact release commit to the default branch first")
    releases = github.request(f"repos/{REPO}/releases?per_page=100")
    release = next((value for value in releases if value["tag_name"] == tag), None)
    if release and not release["draft"]:
        raise ValueError("This tag is already published. Bump versions and use a new tag; published releases are immutable")
    if release and release["target_commitish"] != head:
        raise ValueError("Existing draft targets another commit; inspect it before retrying")
    if release is None:
        release = github.request(f"repos/{REPO}/releases", "POST", {
            "tag_name": tag, "target_commitish": head, "name": f"AiCli {tag}",
            "body": notes, "draft": True, "prerelease": False,
        })
    release_id = release["id"]
    # A retry may replace incomplete draft assets, never a published release.
    for old in release["assets"]:
        github.request(f"repos/{REPO}/releases/assets/{old['id']}", "DELETE")
    upload_url = release["upload_url"].split("{")[0]
    for asset in assets:
        print(f"Uploading {asset.name} ({asset.stat().st_size:,} bytes)", flush=True)
        content_type = "application/json" if asset.suffix == ".json" else "application/octet-stream"
        uploaded = github.request(upload_url + "?name=" + urllib.parse.quote(asset.name),
                                  "POST", asset.read_bytes(), content_type)
        if uploaded["state"] != "uploaded" or uploaded["size"] != asset.stat().st_size:
            raise ValueError("Upload incomplete; release remains a draft")
        if uploaded.get("digest") and uploaded["digest"] != "sha256:" + checksum(asset):
            raise ValueError("GitHub asset checksum mismatch; release remains a draft")
    published = github.request(f"repos/{REPO}/releases/{release_id}", "PATCH", {
        "body": notes, "draft": False, "make_latest": "true",
    })
    print("Published: " + published["html_url"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--notes-file", type=Path)
    parser.add_argument("--publish", action="store_true", help="Upload and publish to GitHub (default: local preparation only)")
    args = parser.parse_args()
    assets = prepare(args.tag)
    print("Prepared: " + str(assets[-1]))
    if args.publish:
        if not args.notes_file:
            parser.error("--notes-file is required with --publish")
        publish(args.tag, args.notes_file.read_text(encoding="utf-8"), assets)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from None
