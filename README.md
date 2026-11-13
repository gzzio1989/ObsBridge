# Syncroom OBS Bridge

DAW（REAPER / Cubase / Studio One / Logic Pro 等）の **ASIO / CoreAudio 本線モニター出力（追加遅延 0.0 ms）を一切邪魔せず**、マスタートラックのミックス音（自分のボーカル・楽器音 ＋ SYNCROOM の他メンバー全員の音）を **OBS Studio** 等の配信ソフトへ低遅延・ゼロクリックで流し込む VST3 / AU ブリッジプラグインです。

## 対応環境
- **Windows 10 / 11 (64-bit)**：`VST3`（静的リンク済み・VC++ランタイム不要）
  - 自動認識する仮想ドライバ：`Yamaha SYNCROOM Driver (WDM)` / `VB-CABLE` / `VoiceMeeter` / `VocalGzzio Driver` ほか（手動選択も可）
- **macOS 11 Big Sur 以降 (Apple Silicon M1〜M4 & Intel Universal)**：`VST3` / `AU (.component)`
  - 自動認識する仮想ドライバ：`BlackHole 2ch` / `Soundflower` / `VB-Cable` / `Loopback Audio` ほか（手動選択も可）

## ビルド＆配布パッケージ生成方法

### Windows ローカルビルド
```powershell
cmake -S . -B build_dist
cmake --build build_dist --config Release --parallel
.\build_dist\Release\verify_obs_bridge.exe
.\tools\package_windows.ps1 -BuildDir build_dist -OutDir release
```

### macOS 版の自動ビルド（GitHub Actions）
本フォルダを GitHub リポジトリへ Push すると、[release.yml](.github/workflows/release.yml) が自動実行され、**Windows 版（Setup `.exe` & `.zip`）** と **macOS Universal 版（`VST3` + `AU` 同梱 `.zip`）** の両方が Actions の Artifacts に生成されます。
