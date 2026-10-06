# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![codecov](https://codecov.io/gh/libraz/libsonare/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20WebAssembly-lightgrey)](https://github.com/libraz/libsonare)
[![Docs](https://img.shields.io/badge/docs-libsonare.libraz.net-2563eb)](https://libsonare.libraz.net/ja/)

**libsonare は、音を「データ」に、データを「音」に変換します。** 曲を読み込んで BPM・キー・コード・構成を取り出し、放送品質のラウドネスへ整え、MIDI を内蔵インストゥルメントで鳴らし、その上に DAW まで組めます——C++・Python・Node.js・ブラウザで同じエンジンが動きます。C++ コアはランタイム依存ゼロで、Python パッケージは NumPy に依存します。GPL/AGPL のコードもモデル重みもありません。

📖 **[ドキュメント](https://libsonare.libraz.net/ja/)** &nbsp;·&nbsp; 🎧 **[ブラウザ完結デモ](https://libsonare.libraz.net/ja/demos)** &nbsp;·&nbsp; **[はじめに](https://libsonare.libraz.net/ja/docs/getting-started)**

## sonare studio

**[sonare studio](https://sonare-studio.libraz.net)** は、音声処理を libsonare の WASM エンジンで動かすブラウザ完結の DAW です。マルチトラック再生、インストゥルメント、ミキサー、マスタリング、音声の書き出しがクライアントサイドで動きます。音楽理論と作曲機能は [`@libraz/libcantus`](https://github.com/libraz/libcantus)、楽譜の描画は別のライブラリが担当しています。エンジンをエンドツーエンドで試すホスト済みのライブデモで、製品ではありません（ソースは非公開）。

## できること

- **解析** — BPM、キー、コード、ビート、セクション、ピッチ、ラウドネス（EBU R128）、音響特性。librosa と重なる範囲ではデフォルト値を揃え、CI で librosa のリファレンス値と照合しています。[解析](https://libsonare.libraz.net/ja/docs/analysis)
- **ステム分離と採譜** — `decomposeStems` でミックスをステムに分け、`transcribe` で音声を MIDI にします。[音源分離](https://libsonare.libraz.net/ja/docs/analysis#音源分離) · [CLI `transcribe`](https://libsonare.libraz.net/ja/docs/cli-examples)
- **マスタリング** — 94 個の名前付き DSP プロセッサ（EQ、ダイナミクス、マルチバンド、ステレオ、サチュレーション、リペア、マキシマイザー、リファレンスマッチング）。`BUILD_FX=OFF` ではクリエイティブ系ストリーミングエフェクトが外れて 75 個になります。リストア用に 5 つのプリセット（ビニール、テープヒス、フィールド録音、ボイスメモ、シェラック 78）があります。[マスタリングプロセッサ](https://libsonare.libraz.net/ja/docs/mastering-processors) · [マスタリングアシスタント](https://libsonare.libraz.net/ja/docs/mastering-assistant)
- **ミキシング／ルーティング** — チャンネルストリップ、バス、センド、バス間ルーティング、トラックまたはバスをキーにしたサイドチェイン、シーンプリセット、ミキサーシーンを提案する任意のアシスタント。[ミキシング](https://libsonare.libraz.net/ja/docs/mixing) · [リアルタイムエンジン](https://libsonare.libraz.net/ja/docs/realtime-engine) · [ミキシングアシスタント](https://libsonare.libraz.net/ja/docs/mixing-assistant)
- **編集 & クリエイティブ FX** — タイムストレッチ、ピッチシフト、ピッチ補正、ボイスチェンジ、リバーブ、モジュレーション、ディレイ、アンプシミュ。[編集 DSP](https://libsonare.libraz.net/ja/docs/editing-dsp) · [スペクトル編集](https://libsonare.libraz.net/ja/docs/spectral-editing)
- **ノート編集とテイク** — モノフォニック／ポリフォニックのテイクをノート単位で編集、テイクをリファレンスへ揃え（`alignTakeToReference`）、MIDI のメロディへチューニング（`tune-to-midi`）、ボーカルテイクを undo/redo 付きでオフライン編集（`createVocalEditSession`）。[MIDI 編集](https://libsonare.libraz.net/ja/docs/project-editing-midi) · [テイク](https://libsonare.libraz.net/ja/docs/recording-and-takes)
- **ルームアコースティクス** — ルームインパルスレスポンスの合成・推定・モーフィング。[音響解析](https://libsonare.libraz.net/ja/docs/acoustic-analysis)
- **内蔵インストゥルメント** — 17 種のシンセシスエンジンを持つ NativeSynth と、全 128 プログラムをカバーする GM/GS フォールバック（MIDI が無音になりません）。パートごとにリグを選べます（`setPartRig`）。アコースティックピアノは校正済みで、ほかの物理モデルボイスは調整中のため、1.8.x のパッチリリースで音が変わります。[NativeSynth](https://libsonare.libraz.net/ja/docs/native-synth) · [物理モデル](https://libsonare.libraz.net/ja/docs/physical-models)
- **GS と SoundFont** — GS SysEx の受信、GS インサーションエフェクトの 2 つの実装（modern / classic）、ホスト供給の SoundFont を鳴らす GS 互換 16 パート SF2 プレーヤー。`effects.gsEfx` insert で、どちらの実装もオーディオトラックやバスに適用できます。[GM/GS](https://libsonare.libraz.net/ja/docs/gm-gs) · [SoundFont プレーヤー](https://libsonare.libraz.net/ja/docs/soundfont-player)
- **ヘッドレス DAW ランタイム** — オーディオ／MIDI トラック、テイク、ワープ、MIDI 1.0/2.0 シーケンス、SMF 入出力、オフラインバウンス、リアルタイムエンジンで再生するタイムラインへのコンパイル（`compileTimeline`）。[プロジェクト編集](https://libsonare.libraz.net/ja/docs/project-editing)
- **リアルタイムエンジン** — アロケーションフリーの再生、ストリーミング、ライブ MIDI 1.0/2.0 入力、ロックフリーのオートメーション、レコーディング。AudioWorklet 経由でブラウザでも動きます。[リアルタイムエンジン](https://libsonare.libraz.net/ja/docs/realtime-engine) · [MIDI 入力](https://libsonare.libraz.net/ja/docs/midi-input)
- **プレイバックレンダラー** — チャンネル変換、ラウドネス整合、低音管理、ヘッドホンとスピーカー向けの HRTF バイノーラル化。[プレイバック](https://libsonare.libraz.net/ja/docs/playback)
- **C++ パッケージ** — C++ ライブラリは CMake パッケージとしてインストールできます（`find_package(sonare)`）。[C++ API](https://libsonare.libraz.net/ja/docs/cpp-api)

## インストール

```bash
npm install @libraz/libsonare   # JavaScript / TypeScript（WASM、Float32Array を渡す）
pip install libsonare            # Python（WAV/MP3。ほかの形式は FFmpeg 付きビルドが必要）
```

[`@libraz/libsonare-native`](bindings/node/) は npm に公開していません。リポジトリを clone してローカル依存として使ってください。対応フォーマット、FFmpeg、ランタイムの選び方は[インストール](https://libsonare.libraz.net/ja/docs/installation)を参照してください。

## クイックスタート

### JavaScript / TypeScript (WASM)

```typescript
import { Audio, init } from '@libraz/libsonare';

await init();

const bytes = new Uint8Array(await file.arrayBuffer());
const audio = await Audio.fromMemoryWithBrowserFallback(bytes);
const result = audio.analyze(); // BPM・キー・コード・セクションなど
console.log(result.key.name);
```

→ [JavaScript API](https://libsonare.libraz.net/ja/docs/js-api) · [ブラウザ / WASM](https://libsonare.libraz.net/ja/docs/wasm)

### Python

```python
import libsonare

audio = libsonare.Audio.from_file("song.mp3")
print(f"BPM: {audio.detect_bpm()}, Key: {audio.detect_key()}")

result = audio.mastering(target_lufs=-14.0, ceiling_db=-1.0)
print(f"{result.input_lufs:.1f} LUFS → {result.output_lufs:.1f} LUFS")
```

`sonare` コマンドは Python パッケージに同梱されています。native CLI は `sonare-cli` です。→ [Python API](https://libsonare.libraz.net/ja/docs/python-api) · [CLI](https://libsonare.libraz.net/ja/docs/cli)

### C++

```cmake
find_package(sonare REQUIRED)
target_link_libraries(app PRIVATE sonare::sonare)
```

```cpp
#include "sonare.h"

auto audio = sonare::Audio::from_file("music.mp3");
auto result = sonare::MusicAnalyzer(audio).analyze();
std::cout << "BPM: " << result.bpm
          << ", Key: " << result.key.to_string() << std::endl;
```

→ [C++ API](https://libsonare.libraz.net/ja/docs/cpp-api)

## ソースからビルド

```bash
make build && make test   # ネイティブ
make wasm                  # WebAssembly
make release               # 最適化ビルド
```

ビルドオプション（`BUILD_MASTERING`、`BUILD_MIXING`、`BUILD_MIXING_ASSISTANT`、FFmpeg）は[アーキテクチャ](https://libsonare.libraz.net/ja/docs/architecture)を参照してください。

## ドキュメント

完全なドキュメントとブラウザ完結デモは **[libsonare.libraz.net](https://libsonare.libraz.net/ja/)** にあります。

- **学ぶ** — [イントロダクション](https://libsonare.libraz.net/ja/docs/introduction) · [はじめに](https://libsonare.libraz.net/ja/docs/getting-started) · [インストール](https://libsonare.libraz.net/ja/docs/installation) · [使用例](https://libsonare.libraz.net/ja/docs/examples)
- **ランタイム別 API** — [ブラウザ / WASM](https://libsonare.libraz.net/ja/docs/wasm) · [JavaScript](https://libsonare.libraz.net/ja/docs/js-api) · [Python](https://libsonare.libraz.net/ja/docs/python-api) · [Node.js ネイティブ](https://libsonare.libraz.net/ja/docs/native-bindings) · [C++](https://libsonare.libraz.net/ja/docs/cpp-api) · [CLI](https://libsonare.libraz.net/ja/docs/cli)
- **詳細** — [アーキテクチャ](https://libsonare.libraz.net/ja/docs/architecture) · [librosa 互換性](https://libsonare.libraz.net/ja/docs/librosa-compatibility) · [ベンチマーク](https://libsonare.libraz.net/ja/docs/benchmarks) · [用語集](https://libsonare.libraz.net/ja/docs/glossary)

どのランタイムも同じ C++17 の DSP コアを呼びますが、API サーフェスはランタイムごとの手書きで同一ではありません。[バインディング対応表](https://libsonare.libraz.net/ja/docs/binding-parity)と、生成表の[ランタイム別カバレッジ表](tools/parity/surface-coverage.md)を参照してください。

## 含まないもの（Non-goals）

libsonare はアプリケーションではなくヘッドレスなエンジンです。UI や DAW ワークフロー、VST/CLAP のプラグインホスティング、クロスプラットフォームのリアルタイム I/O 抽象化、サンプルデータの同梱、深層学習モデルは含みません。Windows は非対応で、Linux・macOS・WebAssembly または WSL2 を利用してください。ノート単位の作曲は別ライブラリの [`@libraz/libcantus`](https://github.com/libraz/libcantus) の担当です。背景は[やらないこと](https://libsonare.libraz.net/ja/docs/architecture#やらないこと)を参照してください。

## ライセンス

[Apache-2.0](LICENSE)
