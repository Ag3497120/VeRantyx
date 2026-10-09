# Vision Spatial Tools - セットアップガイド

> このガイドは旧版アプリ用です。現在のXcodeプロジェクトとセットアップ手順は、リポジトリルートの[README](../README.md)を参照してください。

このガイドでは、Vision Spatial Toolsプロジェクトのセットアップ手順を詳しく説明します。

## 前提条件

### ハードウェア
- Apple Vision Pro（実機）または
- Mac（Apple Silicon推奨）+ visionOS Simulator

### ソフトウェア
- macOS Sonoma 14.0以降
- Xcode 15.0以降
- Apple Developer Account（実機デプロイ時）

## セットアップ手順

### 1. Xcodeプロジェクトの作成

現在のファイル構造はSwiftファイルのみです。Xcodeプロジェクトを作成する必要があります。

```bash
# プロジェクトディレクトリに移動
cd /Users/motonishikoudai/vision-spatial-tools

# Xcodeを起動
open /Applications/Xcode.app
```

#### Xcode内での操作

1. **File > New > Project** を選択
2. **visionOS** タブを選択
3. **App** テンプレートを選択して **Next**
4. プロジェクト設定:
   - Product Name: `VisionSpatialTools`
   - Team: あなたの開発者アカウント
   - Organization Identifier: `com.yourname` など
   - Bundle Identifier: `com.yourname.VisionSpatialTools`
   - Interface: **SwiftUI**
   - Language: **Swift**
   - Include Tests: チェック
5. 保存場所を選択（`vision-spatial-tools`ディレクトリ）

### 2. ファイルの配置

既存のSwiftファイルをXcodeプロジェクトに追加します。

1. Xcodeのプロジェクトナビゲーターで右クリック
2. **Add Files to "VisionSpatialTools"...** を選択
3. 以下のディレクトリを選択して追加:
   - `VisionSpatialToolsApp/`
   - `Models/`
   - `Views/`
   - `Components/`
   - `Services/`

### 3. Info.plistの設定

1. プロジェクトナビゲーターで `Info.plist` を選択
2. 以下のキーを追加:

```xml
<key>NSCameraUsageDescription</key>
<string>This app needs camera access to detect objects and surfaces in your environment.</string>

<key>NSHandsTrackingUsageDescription</key>
<string>This app uses hand tracking to enable natural gesture-based interactions with virtual tools.</string>

<key>NSWorldSensingUsageDescription</key>
<string>This app needs to understand your environment to place virtual tools on real-world objects.</string>
```

### 4. Capabilitiesの設定

1. プロジェクト設定を開く
2. **Signing & Capabilities** タブを選択
3. **+ Capability** をクリック
4. 以下を追加:
   - **ARKit**
   - **Camera**

### 5. ビルド設定

#### Minimum Deployment Target
- visionOS 1.0

#### Supported Destinations
- visionOS

#### Swift Language Version
- Swift 5.9

### 6. RealityKitContentの追加（オプション）

高度な3Dコンテンツを使用する場合:

1. **File > New > Package...** を選択
2. **RealityKit Package** を選択
3. 名前を `RealityKitContent` に設定

### 7. ビルドとテスト

#### シミュレーターでのテスト

1. スキームを **VisionSpatialTools** に設定
2. デバイスを **Apple Vision Pro (Simulator)** に設定
3. **⌘ + R** でビルド＆実行

#### 実機でのテスト

1. Vision Proを接続
2. デバイスを **Apple Vision Pro** に設定
3. プロビジョニングプロファイルが正しく設定されていることを確認
4. **⌘ + R** でビルド＆実行

## トラブルシューティング

### ビルドエラー: "Missing required module"

**解決策**:
```swift
// 各ファイルの先頭に必要なimportを追加
import SwiftUI
import RealityKit
import ARKit
```

### エラー: "No such module 'RealityKitContent'"

**解決策**:
`ImmersiveView.swift`から以下の行を削除またはコメントアウト:
```swift
// import RealityKitContent
```

### 実機デプロイ時の署名エラー

**解決策**:
1. Apple Developer Programに登録
2. Xcode > Settings > Accounts でアカウントを追加
3. プロジェクト設定で正しいTeamを選択

### シミュレーターでハンドトラッキングが動作しない

**注意**: ハンドトラッキングは実機のみで完全に機能します。シミュレーターでは制限があります。

**代替方法**:
- マウスでタップジェスチャーを使用
- 開発時はタップインタラクションを優先

## 推奨開発環境

### Xcodeプラグイン
- Reality Composer Pro
- visionOS Simulator

### 開発ツール
- Instruments（パフォーマンス測定）
- RealityKit Debugger
- ARKit Recording（実機テスト記録）

## デバッグのヒント

### ARKitセッションのデバッグ

```swift
// AppModel.swiftに追加
func debugARKitStatus() {
    print("WorldTracking: \(worldTrackingProvider.state)")
    print("PlaneDetection: \(planeDetectionProvider.state)")
    print("HandTracking: \(handTrackingProvider.state)")
}
```

### ジェスチャー認識のデバッグ

```swift
// HandTrackingManager.swiftに追加
private func debugHandPosition(_ anchor: HandAnchor) {
    print("Hand: \(anchor.chirality)")
    print("Position: \(getHandPosition(from: anchor))")
    print("Gesture: \(gestureType)")
}
```

### 空間アンカーのデバッグ

```swift
// SpatialAnchorManager.swiftに追加
func debugPlanes() {
    print("Detected planes: \(planeAnchors.count)")
    for plane in planeAnchors {
        print("Plane: \(plane.classification), Size: \(plane.geometry.extent)")
    }
}
```

## パフォーマンス最適化

### メモリ使用量の削減

1. 不要なツールは削除
2. 描画データはバッチ処理
3. Entity数を制限（推奨: 最大20個）

### フレームレートの維持

- 60fps以上を維持（visionOSの推奨）
- 複雑なジオメトリを避ける
- マテリアルを最適化

### バッテリー消費の抑制

- 不要な時はトラッキングを停止
- 更新頻度を調整
- バックグラウンドタスクを最小化

## 次のステップ

1. **基本機能のテスト**: 各ツールが正しく動作するか確認
2. **ジェスチャーの調整**: 実機でハンドトラッキングを試す
3. **UXの改善**: ユーザーからのフィードバックを収集
4. **追加機能の実装**: README.mdの「今後の予定」を参照

## サポート

問題が発生した場合:

1. このドキュメントを確認
2. README.mdのトラブルシューティングセクションを確認
3. Xcodeのコンソールログをチェック
4. GitHubでIssueを作成

## 参考資料

- [Apple Vision Pro Documentation](https://developer.apple.com/visionos/)
- [RealityKit Documentation](https://developer.apple.com/documentation/realitykit)
- [ARKit Documentation](https://developer.apple.com/documentation/arkit)
- [SwiftUI for visionOS](https://developer.apple.com/documentation/swiftui)
