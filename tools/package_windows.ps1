param(
    [string]$BuildDir = "build",
    [string]$OutDir   = "release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

try {
    # CMakeLists.txt からバージョン番号を取得
    $cmakeText = Get-Content "CMakeLists.txt" -Raw -Encoding UTF8
    if ($cmakeText -match "project\(SyncroomObsBridge\s+VERSION\s+(\d+\.\d+\.\d+)") {
        $version = $Matches[1]
    } else {
        throw "CMakeLists.txt からバージョン番号を取得できませんでした。"
    }

    $absBuildDir = (Resolve-Path (Join-Path $root $BuildDir)).Path
    $vst3Bundle = Join-Path $absBuildDir "SyncroomObsBridge_artefacts\Release\VST3\Syncroom OBS Bridge.vst3"
    if (-not (Test-Path $vst3Bundle)) {
        throw "VST3 バンドルが見つかりません: $vst3Bundle"
    }

    $fullOutDir = Join-Path $root $OutDir
    New-Item -ItemType Directory -Path $fullOutDir -Force | Out-Null
    $absOutDir = (Resolve-Path $fullOutDir).Path

    # 1. ポータブル配布用 ZIP の作成
    $stageDir = Join-Path $absBuildDir "zip_stage\SyncroomOBSBridge_v${version}_Windows_x64"
    if (Test-Path $stageDir) { Remove-Item $stageDir -Recurse -Force }
    New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

    Copy-Item -Path $vst3Bundle -Destination (Join-Path $stageDir "Syncroom OBS Bridge.vst3") -Recurse -Force
    Copy-Item -Path "docs\取扱説明書.html" -Destination (Join-Path $stageDir "取扱説明書.html") -Force
    Copy-Item -Path "LICENSE.txt" -Destination (Join-Path $stageDir "LICENSE.txt") -Force

    $guideText = @"
============================================================
 Syncroom OBS Bridge v${version} (Windows 64-bit ポータブル版)
============================================================

【導入手順】
1. 同梱の「Syncroom OBS Bridge.vst3」フォルダを丸ごと、以下のフォルダへコピーしてください:
   C:\Program Files\Common Files\VST3\

2. REAPER / Cubase / Studio One 等の DAW を起動（または FX 画面で F5 再スキャン）し、
   Master トラックの「syncroom_vst_bridge2」の下（後段）に「Syncroom OBS Bridge」を挿してください。

3. OBS Studio の「設定 → 音声」で、マイク/補助音声に
   「ライン (Yamaha SYNCROOM Driver (WDM))」（または VB-CABLE 等）を選ぶだけで音が入ります！

詳しい使い方は同梱の「取扱説明書.html」をダブルクリックしてご覧ください。
"@
    $utf8Bom = New-Object System.Text.UTF8Encoding($true)
    [System.IO.File]::WriteAllText((Join-Path $stageDir "かんたん導入ガイド.txt"), $guideText, $utf8Bom)

    $zipPath = Join-Path $absOutDir "SyncroomOBSBridge_v${version}_Windows_x64.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path "$stageDir\*" -DestinationPath $zipPath -Force
    $zipHash = (Get-FileHash $zipPath -Algorithm SHA256).Hash.ToLower()
    [System.IO.File]::WriteAllText("$zipPath.sha256", "$zipHash  $(Split-Path $zipPath -Leaf)`n", (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "Created ZIP: $zipPath"

    # 2. Inno Setup インストーラー (.exe) の作成（UTF-8 BOM 付きで .iss を生成）
    $issContent = @"
#define MyAppName "Syncroom OBS Bridge"
#define MyAppVersion "${version}"
#define MyAppPublisher "Gzzio"
#define MyVst3Name "Syncroom OBS Bridge.vst3"

[Setup]
AppId={{9B4E62D1-3F8A-4D77-9A15-6E2C8F01B4A9}
CloseApplications=yes
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
VersionInfoVersion={#MyAppVersion}
DefaultDirName={commoncf64}\VST3
DisableDirPage=no
DirExistsWarning=no
DisableProgramGroupPage=yes
DisableWelcomePage=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=${absOutDir}
OutputBaseFilename=SyncroomOBSBridge_Setup_v{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#MyAppName} v{#MyAppVersion}

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"
Name: "english";  MessagesFile: "compiler:Default.isl"

[InstallDelete]
Type: filesandordirs; Name: "{app}\{#MyVst3Name}"

[Files]
Source: "${absBuildDir}\SyncroomObsBridge_artefacts\Release\VST3\{#MyVst3Name}\*"; DestDir: "{app}\{#MyVst3Name}"; Flags: recursesubdirs createallsubdirs ignoreversion
Source: "${root}\docs\取扱説明書.html"; DestDir: "{app}\{#MyVst3Name}"; Flags: ignoreversion
Source: "${root}\LICENSE.txt"; DestDir: "{app}\{#MyVst3Name}"; Flags: ignoreversion

[Run]
Filename: "{app}\{#MyVst3Name}\取扱説明書.html"; Description: "取扱説明書を開く"; Flags: nowait postinstall skipifsilent shellexec unchecked
"@

    $issPath = Join-Path $absBuildDir "Installer_generated.iss"
    [System.IO.File]::WriteAllText($issPath, $issContent, $utf8Bom)

    $isccCandidates = @(
        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        "C:\Program Files\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    )
    $iscc = $isccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) {
        $cmd = Get-Command iscc -ErrorAction SilentlyContinue
        if ($cmd) { $iscc = $cmd.Source }
    }

    if ($iscc) {
        & $iscc $issPath
        if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
        $exePath = Join-Path $absOutDir "SyncroomOBSBridge_Setup_v${version}.exe"
        $exeHash = (Get-FileHash $exePath -Algorithm SHA256).Hash.ToLower()
        [System.IO.File]::WriteAllText("$exePath.sha256", "$exeHash  $(Split-Path $exePath -Leaf)`n", (New-Object System.Text.UTF8Encoding($false)))
        Write-Host "Created Installer: $exePath"
    } else {
        Write-Warning "Inno Setup (ISCC.exe) が見つからないため ZIP のみ作成しました。"
    }
} finally {
    Pop-Location
}
