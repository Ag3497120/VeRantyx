# Vision Spatial Tools

> この文書は旧版アプリの説明です。現在のHalf-Life: Alyxストリーミング用クライアントとセットアップ手順は、リポジトリルートの[README](../README.md)を参照してください。現在のXcodeプロジェクトは`VisionProClient/VisionSpatialTools/VisionSpatialTools.xcodeproj`です。

Apple Vision Pro向けの空間コンピューティングアプリケーション。任意のオブジェクトにキーボード、メモ、描画キャンバス、計算機などのツールを配置して操作できます。

## 主な機能

### 📱 4つの仮想ツール

1. **キーボード** 🎹
   - QWERTYレイアウトの3Dキーボード
   - スペースバー、テキスト表示エリア付き
   - ホバーエフェクトで視覚的フィードバック

2. **メモ** 📝
   - 罫線付きのメモパッド
   - 最大10行のテキスト入力
   - クリア・保存機能

3. **描画キャンバス** 🎨
   - 8色のカラーパレット
   - ブラシサイズ調整（4段階）
   - 消しゴム、クリア、保存機能
   - リアルタイムで描画

4. **計算機** 🧮
   - 0-9の数字キー
   - 四則演算（+、-、×、÷）
   - クリア（C）、全クリア（AC）、バックスペース機能

### 🌐 空間コンピューティング機能

- **オブジェクト検出**: 平面や垂直面を自動検出
- **空間アンカー**: 任意の場所にツールを配置
- **ハンドトラッキング**: 手のジェスチャーで操作
  - ピンチ: ツールの選択・操作
  - ポイント: 要素の指定
  - グラブ: ツールの移動
- **永続化**: 配置したツールの位置を保存・復元

### 🎯 ジェスチャー認識

- **ピンチ**: 親指と人差し指でつまむ動作（3cm以内）
- **ポイント**: 人差し指を伸ばす動作
- **グラブ**: すべての指を握る動作
- **スワイプ**: 手を横に動かす動作

## 技術スタック

- **SwiftUI**: UIフレームワーク
- **RealityKit**: 3Dコンテンツレンダリング
- **ARKit**: 空間トラッキング
  - WorldTrackingProvider
  - PlaneDetectionProvider
  - HandTrackingProvider
- **visionOS 1.0+**: プラットフォーム

## プロジェクト構造

```
vision-spatial-tools/
├── Package.swift
├── Info.plist
├── README.md
└── VisionSpatialToolsApp/
    ├── VisionSpatialToolsApp.swift      # アプリエントリーポイント
    ├── Models/
    │   └── AppModel.swift                # アプリケーション状態管理
    ├── Views/
    │   ├── ContentView.swift             # メインUI
    │   └── ImmersiveView.swift           # 空間ビュー
    ├── Components/
    │   ├── KeyboardEntity.swift          # キーボードコンポーネント
    │   ├── MemoEntity.swift              # メモコンポーネント
    │   ├── DrawingCanvasEntity.swift     # 描画キャンバス
    │   └── CalculatorEntity.swift        # 計算機コンポーネント
    └── Services/
        ├── PersistenceManager.swift      # データ永続化
        ├── HandTrackingManager.swift     # ハンドトラッキング
        └── SpatialAnchorManager.swift    # 空間アンカー管理
```

## 使い方

### 1. セットアップ

```bash
# Xcodeでプロジェクトを開く
open vision-spatial-tools.xcodeproj

# または、Swift Package Managerを使用
swift build
```

### 2. アプリの起動

1. Apple Vision Proでアプリを起動
2. メイン画面でツールを選択（キーボード、メモ、描画、計算機）
3. 「空間モードを開始」ボタンをタップ

### 3. ツールの配置

1. 空間モードに入ると、環境がスキャンされます
2. 配置したい場所（テーブル、壁など）をタップ
3. 選択したツールがその位置に配置されます

### 4. ツールの操作

#### キーボード
- キーをタップして文字入力
- スペースバーで空白を挿入
- 上部のディスプレイエリアにテキストが表示

#### メモ
- 罫線に沿ってテキストを記入
- ゴミ箱アイコンでクリア
- 保存アイコンでメモを保存

#### 描画キャンバス
- カラーパレットで色を選択
- ブラシサイズボタンで太さ調整
- キャンバスをドラッグして描画
- 消しゴムボタンで消去モード
- ゴミ箱アイコンで全消去

#### 計算機
- 数字ボタンをタップ
- 演算子ボタンで計算
- =ボタンで結果表示
- Cでクリア、ACで全クリア

### 5. ハンドジェスチャー

- **ピンチ**: ツールのボタンを押す
- **ポイント**: 要素を指定
- **グラブ**: ツールを掴んで移動

## 必要な権限

アプリは以下の権限を要求します：

- **カメラアクセス**: 環境の認識
- **ハンドトラッキング**: ジェスチャー操作
- **ワールドセンシング**: 平面検出とアンカー配置

## 開発要件

- Xcode 15.0+
- visionOS 1.0+ SDK
- Apple Vision Pro（実機またはシミュレーター）
- Swift 5.9+

## アーキテクチャ

### データフロー

```
User Input (Gesture/Tap)
    ↓
HandTrackingManager / SpatialAnchorManager
    ↓
AppModel (State Management)
    ↓
ImmersiveView (RealityKit Scene)
    ↓
Tool Entities (Keyboard/Memo/Drawing/Calculator)
    ↓
PersistenceManager (Save/Load)
```

### 主要コンポーネント

- **AppModel**: アプリケーション全体の状態を管理
- **ImmersiveView**: 空間シーンのレンダリング
- **Tool Entities**: 各ツールの3Dモデルと機能
- **Managers**: トラッキング、永続化、アンカー管理

## カスタマイズ

### 新しいツールの追加

1. `Components/` に新しいEntityクラスを作成
2. `ToolType` enumに追加
3. `ImmersiveView.createTool()` にケースを追加
4. `ContentView` にボタンを追加

### ジェスチャーの変更

`HandTrackingManager.detectGestures()` でジェスチャー検出ロジックをカスタマイズ

### 永続化の拡張

`PersistenceManager` でツールの詳細なデータ（テキスト内容、描画データなど）を保存

## トラブルシューティング

### ツールが配置できない
- 平面検出が完了するまで待つ（数秒）
- 明るい環境で使用
- 壁やテーブルなど明確な平面を探す

### ハンドトラッキングが動作しない
- 手を視野内に入れる
- 指をしっかり開く/閉じる
- 照明が十分か確認

### アプリがクラッシュする
- メモリ不足の可能性：配置したツールを削除
- 権限が付与されているか確認

## ライセンス

MIT License

## 作者

Vision Spatial Tools Development Team

## バージョン履歴

- **v1.0.0** (2026-03-12): 初回リリース
  - キーボード、メモ、描画、計算機の4ツール
  - ハンドトラッキング対応
  - 空間アンカーと永続化

## 今後の予定

- [ ] テキスト音声入力
- [ ] マルチユーザー対応
- [ ] クラウド同期
- [ ] より多くのツール（タイマー、ブラウザなど）
- [ ] カスタムツール作成機能
- [ ] AIアシスタント統合
