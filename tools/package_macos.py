#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
macOS 配布用パッケージ作成スクリプト (Universal Binary: Apple Silicon arm64 + Intel x86_64)
VST3 バンドルと Audio Unit (.component) にアドホック署名を施し、取扱説明書・導入手順と共に ZIP 化する。
"""
import argparse
import hashlib
import pathlib
import re
import shutil
import subprocess


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", default="build", help="CMake build directory")
    parser.add_argument("--out", default="release", help="Output directory")
    args = parser.parse_args()

    root = pathlib.Path(__file__).resolve().parent.parent
    cmake_text = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"project\(SyncroomObsBridge\s+VERSION\s+(\d+\.\d+\.\d+)", cmake_text)
    if not m:
        raise SystemExit("CMakeLists.txt からバージョン番号を取得できません")
    version = m.group(1)

    artefacts = root / args.build / "SyncroomObsBridge_artefacts" / "Release"
    vst3_src = artefacts / "VST3" / "Syncroom OBS Bridge.vst3"
    au_src   = artefacts / "AU" / "Syncroom OBS Bridge.component"

    if not vst3_src.exists():
        raise SystemExit(f"VST3 が見つかりません: {vst3_src}")

    stage_root = root / args.build / "mac_stage"
    pkg_dir = stage_root / f"SyncroomOBSBridge_v{version}_macOS_Universal"
    if stage_root.exists():
        shutil.rmtree(stage_root)
    pkg_dir.mkdir(parents=True)

    vst3_dst = pkg_dir / "Syncroom OBS Bridge.vst3"
    shutil.copytree(vst3_src, vst3_dst)
    subprocess.run(["codesign", "--force", "--deep", "-s", "-", str(vst3_dst)], check=True)

    if au_src.exists():
        au_dst = pkg_dir / "Syncroom OBS Bridge.component"
        shutil.copytree(au_src, au_dst)
        subprocess.run(["codesign", "--force", "--deep", "-s", "-", str(au_dst)], check=True)

    shutil.copy2(root / "docs" / "取扱説明書.html", pkg_dir / "取扱説明書.html")
    shutil.copy2(root / "LICENSE.txt", pkg_dir / "LICENSE.txt")

    guide = f"""============================================================
 Syncroom OBS Bridge v{version} (macOS Universal: Apple Silicon & Intel)
============================================================

【事前準備（初回のみ）】
Mac では仮想オーディオドライバ「BlackHole 2ch」（無料）をインストールしておくと、
プラグインを挿した瞬間に自動で接続されます:
https://existential.audio/blackhole/

【導入手順】
・REAPER / Cubase / Studio One / Ableton Live 等をお使いの場合:
  「Syncroom OBS Bridge.vst3」を /Library/Audio/Plug-Ins/VST3/ にコピーしてください。

・Logic Pro / GarageBand 等をお使いの場合:
  「Syncroom OBS Bridge.component」を /Library/Audio/Plug-Ins/Components/ にコピーしてください。

※ macOS のセキュリティ警告が出た場合は、ターミナルで以下を1回実行すると解除できます:
  sudo xattr -rd com.apple.quarantine "/Library/Audio/Plug-Ins/VST3/Syncroom OBS Bridge.vst3"
  sudo xattr -rd com.apple.quarantine "/Library/Audio/Plug-Ins/Components/Syncroom OBS Bridge.component"
"""
    (pkg_dir / "Macでの導入手順.txt").write_text(guide, encoding="utf-8")

    out_dir = root / args.out
    out_dir.mkdir(parents=True, exist_ok=True)
    zip_name = f"SyncroomOBSBridge_v{version}_macOS_Universal.zip"
    zip_path = out_dir / zip_name
    if zip_path.exists():
        zip_path.unlink()

    subprocess.run(
        ["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", str(pkg_dir), str(zip_path)],
        check=True,
    )

    digest = hashlib.sha256(zip_path.read_bytes()).hexdigest()
    (out_dir / f"{zip_name}.sha256").write_text(f"{digest}  {zip_name}\n", encoding="utf-8")
    print(f"Created macOS package: {zip_path}")


if __name__ == "__main__":
    main()
