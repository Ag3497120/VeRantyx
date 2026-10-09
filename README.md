# MBP-AVP-VR

Windows Steam runs through the transferred Wine 11.9 runtime. SteamVR uses the Verantyx virtual HMD driver. Half-Life: Alyx submits stereo textures through OpenVR; the Mac capture proxy reads each eye before SteamVR's compositor rejects the texture format, then the Mac encodes a fixed 1440×810 HEVC stream for Apple Vision Pro. The Vision Pro sends head and hand poses back to SteamVR.

## このMacで確認した状態

- Half-Life: AlyxのWindows版`hlvr.exe`と約73GBのゲームデータが`~/Verantyx_VR_Drive/SteamVR_Prefix`内にあります。再ダウンロードは不要です。
- Windows Steamの`steam.exe`とSteamVR本体は同じprefixにあります。SSDから転送された`~/Verantyx_VR_Drive/Beer/wine/WineStaging.app`のWine 11.9でWindows Steamを起動し、Steam UIの表示とサインインをテスト用クローンで確認しました。
- `~/Verantyx_VR_Drive/wine_wrapper`はGame Porting Toolkit 1.1のWine 7.7を指していました。`VRDriver/wine_launcher.sh`はWine 11.9を優先し、見つからない場合のみ従来の独自wrapperへフォールバックします。
- 認証済みSteamセッションは`~/Verantyx_VR_Drive/SteamVR_Prefix_Wine11_Test`というAPFSクローン上で確認しています。本番PrefixのSteamファイルは変更していません。
- 転送済みSteamVR本体は`steamapps/common/SteamVR`にあり、Steamがインストール済みとして認識するappmanifestを所定の`steamapps`直下に登録しました。
- リポジトリのVerantyx HMDドライバーをWindows x64 DLLとしてビルドし、登録済みフォルダーとSteamVR同梱の同名ドライバーへ配置しました。古いSteamVRログで同梱側が優先されることを確認したため、元DLLは両方の場所で`.mbp-avp-vr-original`として保存します。
- Vision Proには修正版をインストールできていますが、Macからのリモート起動はCoreDeviceのタイムアウトで失敗するため、ヘッドセット上で手動起動が必要です。現時点ではSteamVRと配信エンコーダーが起動し、Vision Proから最初の姿勢パケットが届くのを待っています。
- Windows SteamとAlyxのファイルはローカルにあります。サインイン情報はリポジトリに保存しません。

これらの実行ファイル、Steamライブラリ、ゲームデータ、カスタムWindows実行環境はGitには含まれません。

## 実装

- `VRDriver/src/driver_verantyx.cpp`はSteamVRの仮想HMDとして頭部・コントローラー姿勢を登録し、SteamVRの共有D3D11フレームを読み出して共有メモリーへコピーします。Alyxの直接キャプチャが有効な間は、このフォールバック取得を止めます。ピンチをトリガーとしてSteamVR Inputへ反映します。
- `VRDriver/src/openvr_capture_proxy.cpp`はAlyxの左右`IVRCompositor::Submit`を捕捉し、typeless RGBAテクスチャをステレオ画像へ読み出します。取得間隔の既定値は1ペアです。
- `MacEncoder/HardwareEncoder.swift`は共有フレームをVideoToolboxで1440×810のHEVCに変換し、Bonjourで見つけたVision ProへUDP送信します。SteamVRのレンダー倍率は既定0.5、HEVCビットレートは12 Mbpsです。出力幅・高さは`VERANTYX_STREAM_WIDTH`と`VERANTYX_STREAM_HEIGHT`で変更できます。
- `VisionProClient/VisionSpatialTools`はBonjourでMacを探し、UDPでHEVCを受信・再構成して、VideoToolboxで専用キュー上にデコードします。遅れているときは古いデコード待ちフレームを捨て、表示時点の頭部姿勢でステレオ画像を再投影します。ARKitの頭部・手姿勢とピンチ、グリップ、ボタン、スティック値をMacへ送り、画面に受信fpsと現在の入力値を表示します。
- `scripts/install_openvr_driver.sh`は外部登録先とSteamVR同梱先の両方へ同じDLLを配置します。SteamVRが同梱側を優先する場合も更新版が使われ、置き換え前のDLLは隣にバックアップされます。
- `scripts/build_steamwebhelper_shim.sh`と`install_steamwebhelper_shim.sh`は、WineでCEFが初期化できない場合に備え、Steam WebHelperへ`--disable-gpu --single-process`を付けるWindows x64 shimを作成・導入します。元のWebHelperは`steamwebhelper_real.exe`に保存します。
- `scripts/launch_vision_pro_vr.sh`は必要なshimを導入し、WebHelper初期化とSteamの認証状態を確認してからSteamVR（App ID `250820`）を起動します。Verantyx HMDの有効化後にAlyx（App ID `546560`）を起動します。Steamの起動をバックグラウンド化し、Steamが終了まで制御を返さず後続処理が止まるのを防ぎます。

以前のテストではSteamVR、Alyx起動、Alyxの左右テクスチャ取得まで確認しています。今回の起動ではSteamVR設定を0.5にして再起動し、AlyxはVision Proの姿勢パケット到着後に起動するよう待機しています。SteamVR自身へのAlyxテクスチャ提出は`TextureUsesUnsupportedFormat`になりますが、配信用フレームは別経路で取得します。

## 起動

1. Vision Proで「MBP AVP VR」を開き、「ステレオVRを開始」をタップします。初回プロンプトが出たらローカルネットワークと手のトラッキングを許可します。
2. Mac側で起動スクリプトを実行します。SteamVR起動後、Vision Proの姿勢パケットが届くまでAlyxは起動せずに待ちます。Windows Steamに未サインインならクライアントを開き、CEF画面の準備とサインインを待ってから続行します。
3. Vision Proアプリ画面でMac接続、受信fps、頭・手の追跡状態、左右のtrigger/grip/button値を確認します。

```bash
WINEPREFIX="$HOME/Verantyx_VR_Drive/SteamVR_Prefix_Wine11_Test" ./scripts/launch_vision_pro_vr.sh
```

SteamVRだけを起動して確認する場合:

```bash
WINEPREFIX="$HOME/Verantyx_VR_Drive/SteamVR_Prefix_Wine11_Test" STEAMVR_ONLY=1 ./scripts/launch_vision_pro_vr.sh
```

ログは`logs/`に出力します。SteamVR起動待ちは`STEAMVR_STARTUP_TIMEOUT=300`、WebHelper準備待ちは`STEAM_WEBHELPER_STARTUP_TIMEOUT=120`、サインイン待ちは`STEAM_LOGIN_TIMEOUT=900`のように変更できます。別のprefixやドライバーフォルダーを使う場合は`WINEPREFIX`、`VERANTYX_VR_DRIVE`、`VR_DRIVER_DIR`、`STEAMVR_DRIVER_DIR`を指定してください。

## ドライバーのビルド

```bash
./scripts/build_openvr_driver.sh
./scripts/install_openvr_driver.sh
```

このMacには`x86_64-w64-mingw32-g++`があるため、Windows x64用ドライバーを再ビルドできます。カスタムWindowsランチャーとruntimeはSSDから転送済みのものを利用します。リポジトリはそのruntimeを置き換えません。

## 確認状況と制約

- Windows x64ドライバーDLL、Steam WebHelper shim、Mac HEVCエンコーダーはコンパイルできます。
- テストクローンではWine 11.9でSteam UI表示とオンラインサインインを確認済みです。SteamVRの起動、ドライバー有効化、D3D11デバイス作成、Alyx起動、アプリの直接キャプチャ、VideoToolboxのHEVC出力まで確認しました。
- Vision Proアプリの修正版ビルドとインストールは成功していますが、`devicectl`からの起動はCoreDeviceタイムアウトで未確認です。手トラッキング権限を署名へ追加すると、現在の`iOS Team Provisioning Profile: *`に`com.apple.developer.arkit.hand-tracking`が含まれないとXcodeが拒否します。ARKitの許可結果、頭・手トラッキング、実機映像とゲーム内入力はヘッドセットで確認が必要です。資格情報をコマンドラインやリポジトリへ書き込みません。
