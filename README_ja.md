# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![codecov](https://codecov.io/gh/libraz/libsonare/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20WebAssembly-lightgrey)](https://github.com/libraz/libsonare)
[![Docs](https://img.shields.io/badge/docs-libsonare.libraz.net-2563eb)](https://libsonare.libraz.net/ja/)

**解析からアレンジまで。依存ライブラリ不要のオーディオエンジンです。**
librosa 互換の解析、放送品質のマスタリング／ミキシング、内蔵インストゥルメント、
リアルタイムのヘッドレス DAW ランタイムを、C++・Python・Node.js・ブラウザに、
Apache-2.0 ひとつで提供します。

ネイティブと WebAssembly を単一の C++ コードベースで動かします。楽曲を解析し、
マスタリングし、再生し、MIDI を内蔵インストゥルメントで鳴らす——その同じ DSP が、
C++ でもブラウザ（WASM + AudioWorklet）でも同一に動作します。ランタイム依存ゼロ、
実行時 Python 不要、GPL/AGPL なし、モデル重みなし。

📖 **[ドキュメント](https://libsonare.libraz.net/ja/)** &nbsp;·&nbsp; 🎧 **[ブラウザ完結デモ](https://libsonare.libraz.net/ja/demos)** &nbsp;·&nbsp; **[はじめに](https://libsonare.libraz.net/ja/docs/getting-started)**

## できること

- **解析（librosa 互換）** — BPM、キー、コード（HMM 平滑化・転回形・キーコンテキスト）、
  ビート／ダウンビート、拍子、セクション、音色、ダイナミクス、ピッチ（YIN／pYIN）、
  テンポグラム／PLP、NNLS クロマ、EBU R128 ラウドネス、音響特性（ブラインドまたは実測 IR
  からの RT60／EDT／C50／C80／D50）。デフォルト値は librosa に揃え、CI で librosa の
  リファレンス値と照合しています。
- **マスタリング** — 76 個の名前付き DSP プロセッサ（EQ、ダイナミクス、マルチバンド、
  ステレオ、サチュレーション、リペア、マキシマイザー、リファレンスマッチング）。
  ITU-R BS.1770-4 のラウドネス／トゥルーピーク制限、Linkwitz-Riley クロスオーバー、
  Vicanek matched-Z バイクァッド、ADAA クリッパー、Dempwolf 12AX7 三極管モデル、
  ポリフェーズ FIR オーバーサンプリングなど、公開規格・論文に基づく実装です。
  リペア系は DNN 音源分離ではなく古典的 DSP で構成しています。
- **ミキシング／ルーティング** — リアルタイムセーフなチャンネルストリップ／バスモデル
  （デノーマル対策、ロックフリーなパラメータ変更、プラグインディレイ補償）。パンモード、
  センド、FX バス、計測、シーンプリセット、オフラインレンダリングを備えます。
- **編集 & クリエイティブ FX** — タイムストレッチ／ピッチシフト、ピッチ補正、ノート区間
  ストレッチ、ボイスチェンジ、5 種のリバーブエンジン、モジュレーション系エフェクト、
  ステレオディレイ、ギターアンプシミュ、ダッキング。
- **ルームアコースティクス** — シューボックス形状からルームインパルスレスポンスを合成、
  録音から等価なルームをブラインド推定、録音の残響を目標ルームへモーフィング。
  依存なし・決定論的です。
- **内蔵インストゥルメント** — パッチ駆動の NativeSynth（7 種のシンセシスエンジン、
  モジュレーションマトリクス、名前付きプリセット）と、全 128 プログラム＋ドラムを
  データ不要でカバーする GM フォールバックにより、MIDI が無音になりません。ホスト供給の
  SoundFont を読み込めば GS 互換の 16 パート SF2 プレーヤーが引き継ぎ、未カバーの
  プログラムはシンセにフォールバックします。
- **ヘッドレス DAW ランタイム** — オーディオ＆MIDI のトラック／クリップでプロジェクトを構築
  （split／trim／move、undo／redo つき）。テイクとコンプレーン、クリップ別ワープ、
  MIDI 1.0／2.0 シーケンス、SMF・MIDI 2.0 Clip File の入出力、決定論的でバイト安定な JSON、
  内蔵インストゥルメント経由のオフラインバウンスに対応します。
- **リアルタイムエンジン** — サンプル精度・アロケーションフリーの再生エンジン。トランスポート、
  ワープ対応のクリップ再生、巨大クリップのページ式ストリーミング、ライブ MIDI 入力、
  ロックフリーのオートメーション、キャプチャ／レコーディング。同じエンジンが AudioWorklet を
  通してブラウザでも動きます。

各機能・各ランタイム・各プロセッサの詳細な API は
[ドキュメント](https://libsonare.libraz.net/ja/)を参照してください。

## インストール

```bash
npm install @libraz/libsonare   # JavaScript / TypeScript（WASM、Float32Array を渡す）
pip install libsonare            # Python（WAV/MP3。M4A/AAC などは「対応フォーマット」参照）
```

Node.js でファイルをネイティブにデコードしたい場合は、
[`@libraz/libsonare-native`](bindings/node/) をソースからビルドします（pkg-config で
FFmpeg を自動検出します）。ネイティブビルドや FFmpeg まわりの詳細は
[インストールガイド](https://libsonare.libraz.net/ja/docs/installation)を参照してください。

## クイックスタート

以下は代表的な機能だけを示したものです。ランタイムごとの完全な API は
ドキュメントサイトにあります。

### JavaScript / TypeScript (WASM)

`@libraz/libsonare` はデコード済みの `Float32Array` を受け取ります（Web Audio API や
JS デコーダで用意してください）。

```typescript
import { init, analyze, detectKey, masterAudio } from '@libraz/libsonare';

await init();

const result = analyze(samples, sampleRate);   // BPM・キー・コード・セクションなど
const key = detectKey(samples, sampleRate);     // { name: "C major", confidence: 0.95 }

// 名前付きプリセットで一括マスタリング。
const mastered = masterAudio(samples, sampleRate, 'aiMusic', { 'loudness.targetLufs': -13 });
```

→ [JavaScript API](https://libsonare.libraz.net/ja/docs/js-api) · [ブラウザ / WASM](https://libsonare.libraz.net/ja/docs/wasm)

### Python

`pip install libsonare` で入るホイールは WAV/MP3 のみ対応です。それ以外の形式は事前に
`ffmpeg` で変換するか、FFmpeg をリンクしてビルドしてください（[対応フォーマット](#対応フォーマット)参照）。

```python
import libsonare

audio = libsonare.Audio.from_file("song.mp3")
print(f"BPM: {audio.detect_bpm()}, Key: {audio.detect_key()}")

result = audio.mastering(target_lufs=-14.0, ceiling_db=-1.0)
print(f"{result.input_lufs:.1f} LUFS → {result.output_lufs:.1f} LUFS")
```

→ [Python API](https://libsonare.libraz.net/ja/docs/python-api) · [CLI](https://libsonare.libraz.net/ja/docs/cli)

`sonare` コマンドは Python パッケージに同梱されています
（`sonare analyze song.mp3`、`sonare mastering …`、`sonare project …` など）。

### C++

```cpp
#include "sonare.h"            // 解析 + 特徴量 + エフェクト
#include "mastering/master.h"  // マスタリングチェイン & プロセッサ

auto audio = sonare::Audio::from_file("music.mp3");
auto result = sonare::MusicAnalyzer(audio).analyze();
std::cout << "BPM: " << result.bpm
          << ", Key: " << result.key.to_string() << std::endl;
```

→ [C++ API](https://libsonare.libraz.net/ja/docs/cpp-api)

## 対応フォーマット

| フォーマット | デフォルト¹ | FFmpeg あり² | WASM (`@libraz/libsonare`) |
|--------------|-------------|--------------|----------------------------|
| WAV (PCM 16/24/32, float32) | ✅ | ✅ | n/a（sample を直接渡す） |
| MP3 | ✅ | ✅ | n/a |
| M4A / AAC / FLAC / OGG / Opus / WMA / … | ❌（明示エラー） | ✅ | n/a（Web Audio API を使用） |

¹ **デフォルト**: PyPI ホイールと、FFmpeg dev libs が無い環境でのソースビルド。ホイールは
このモードに固定されているため、インストールがユーザの `libavformat` の有無に左右されません。

² **FFmpeg あり**: FFmpeg をリンクしたソースビルド。CMake が pkg-config 経由で自動検出します
（`-DSONARE_WITH_FFMPEG=AUTO`）。Python: `SONARE_FFMPEG=1 pip install libsonare --no-binary
libsonare`、Node ネイティブ: `SONARE_FFMPEG=1 yarn build`。

WASM は設計上ファイルデコーダを同梱しません。Web Audio API などでデコードした
`Float32Array` を渡してください。

## ソースからビルド

```bash
make build && make test   # ネイティブ。FFmpeg 自動検出、マスタリング＋ミキシングは既定 ON
make wasm                  # WebAssembly
make release               # 最適化ビルド
```

`-DBUILD_MASTERING=OFF` / `-DBUILD_MIXING=OFF` で解析専用の小さいバイナリにできます。
任意・実験的・既定 OFF の macOS ホストバックエンド（CoreAudio／CoreMIDI／AU ホスト）が
ソースビルド向けにデバイス I/O と AU ホスティングを補いますが、公開パッケージには含まれません。
ビルドオプションの詳細は[アーキテクチャ](https://libsonare.libraz.net/ja/docs/architecture)を参照してください。

## ドキュメント

完全なドキュメントとブラウザ完結デモは
**[libsonare.libraz.net](https://libsonare.libraz.net/ja/)**（[デモ](https://libsonare.libraz.net/ja/demos)）にあります。

- **学ぶ** — [イントロダクション](https://libsonare.libraz.net/ja/docs/introduction) · [はじめに](https://libsonare.libraz.net/ja/docs/getting-started) · [インストール](https://libsonare.libraz.net/ja/docs/installation) · [使用例](https://libsonare.libraz.net/ja/docs/examples)
- **ランタイム別 API** — [ブラウザ / WASM](https://libsonare.libraz.net/ja/docs/wasm) · [JavaScript](https://libsonare.libraz.net/ja/docs/js-api) · [Python](https://libsonare.libraz.net/ja/docs/python-api) · [Node.js ネイティブ](https://libsonare.libraz.net/ja/docs/native-bindings) · [C++](https://libsonare.libraz.net/ja/docs/cpp-api) · [CLI](https://libsonare.libraz.net/ja/docs/cli)
- **目的別** — [マスタリングプロセッサ](https://libsonare.libraz.net/ja/docs/mastering-processors) · [ミキシング](https://libsonare.libraz.net/ja/docs/mixing) · [編集 DSP](https://libsonare.libraz.net/ja/docs/editing-dsp) · [リアルタイムとストリーミング](https://libsonare.libraz.net/ja/docs/realtime-streaming) · [ルーム音響](https://libsonare.libraz.net/ja/docs/acoustic-analysis)
- **詳細** — [アーキテクチャ](https://libsonare.libraz.net/ja/docs/architecture) · [librosa 互換性](https://libsonare.libraz.net/ja/docs/librosa-compatibility) · [ベンチマーク](https://libsonare.libraz.net/ja/docs/benchmarks) · [用語集](https://libsonare.libraz.net/ja/docs/glossary)

## 含まないもの（Non-goals）

libsonare はアプリケーションではなくヘッドレスなエンジンです。UI や DAW ワークフロー、
サードパーティのプラグインホスティング（VST/CLAP）、クロスプラットフォームのリアルタイム
I/O 抽象化、サンプルデータの同梱、深層学習モデルは意図的に含みません。オーディオコールバックと
UI は呼び出し側が所有します（I/O 境界の唯一の例外が、オプトイン・非公開の実験的 macOS
バックエンドです）。この線引きが、依存ゼロと Apache-2.0 の純度を保ちます。背景は
[Non-goals](https://libsonare.libraz.net/ja/docs/architecture)を参照してください。

## ライセンス

[Apache-2.0](LICENSE)
