# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![codecov](https://codecov.io/gh/libraz/libsonare/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20WebAssembly-lightgrey)](https://github.com/libraz/libsonare)
[![Docs](https://img.shields.io/badge/docs-libsonare.libraz.net-2563eb)](https://libsonare.libraz.net/ja/)

**libsonare は、音を「データ」に、データを「音」に変換します。** 曲を読み込んで
BPM・キー・コード・構成を取り出し、放送品質のラウドネスへ整え、MIDI を内蔵
インストゥルメントで鳴らし、その上に DAW まで組めます——C++・Python・Node.js・
ブラウザで同じエンジンが動きます。C++ コアはランタイム依存ゼロで、ネイティブエンジンに実行時 Python は不要です（Python パッケージは NumPy に依存）。GPL/AGPL も
モデル重みもありません。

**こんなときに使えます**

- **音声を解析する** — BPM・キー・コード・セクション・ラウドネスを、重い Python/ML スタックなしで。
- **狙った基準へ整える** — 放送品質のラウドネス／トゥルーピーク制御を、プロセス内でもブラウザ内でも。
- **MIDI を音にする** — 内蔵インストゥルメントが全 128 GM プログラム＋ドラムをカバー、SoundFont 不要。
- **1 つのエンジンで届ける** — 同じ C++ DSP がネイティブでもブラウザ（WASM + AudioWorklet）でも、同一の結果で動く。

📖 **[ドキュメント](https://libsonare.libraz.net/ja/)** &nbsp;·&nbsp; 🎧 **[ブラウザ完結デモ](https://libsonare.libraz.net/ja/demos)** &nbsp;·&nbsp; **[はじめに](https://libsonare.libraz.net/ja/docs/getting-started)**

## これで何が作れる？

**[sonare studio](https://sonare-studio.libraz.net)** は、libsonare の WASM エンジンだけで
組み上げたブラウザ完結のフル DAW です。マルチトラックシーケンス、ピアノロール、楽譜浄書、
ミキサー、マスタリング、WAV/MP3/MIDI/MusicXML 書き出しまで、すべてクライアントサイドで
動きます。Apache-2.0 のエンジン 1 つが、解析から再生・書き出し可能なアレンジまで、どこまで
届くかを示すものです。

エンジン全体をエンドツーエンドで叩く**結合検証用のホスト済みライブデモ**であり、製品では
ありません（ソースは現時点で非公開）。ブラウザで開いて、libsonare で何が動かせるかを
確かめてください。

## できること

- **解析** — BPM、キー、コード（HMM 平滑化・転回形・キーコンテキスト）、
  ビート／ダウンビート、拍子、セクション、音色、ダイナミクス、ピッチ（YIN／pYIN）、
  テンポグラム／PLP、NNLS クロマ、EBU R128 ラウドネス、音響特性（ブラインドまたは実測 IR
  からの RT60／EDT／C50／C80／D50）。librosa と重なる範囲ではデフォルト値を揃え、CI で
  librosa のリファレンス値と照合しているため、結果をそのまま移行できます。
- **マスタリング** — 88 個の個別の名前付き DSP プロセッサ（EQ、ダイナミクス、マルチバンド、
  ステレオ、サチュレーション、リペア、マキシマイザー、リファレンスマッチング）。
  `BUILD_FX=OFF` ではクリエイティブ系ストリーミングエフェクトが外れて 71 個になります。
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
- **内蔵インストゥルメント** — パッチ駆動の NativeSynth（15 種のシンセシスエンジン。
  減算・FM・加算・Karplus-Strong・モーダル共鳴に加え、物理モデリングによるピアノ・擦弦・リード・金管・フルート・
  パイプオルガン・撥弦・声・フリーリード・打楽器）、モジュレーションマトリクス、名前付きプリセットを備え、
  全 128 プログラム＋ドラムをデータ不要でカバーする GM フォールバックにより、MIDI が
  無音になりません。ホスト供給の SoundFont を読み込めば GS 互換の 16 パート SF2
  プレーヤーが引き継ぎ、未カバーのプログラムはシンセにフォールバックします。物理モデル
  ボイスは現状でも実用できますが、音色は今後も時間をかけて調整を続けていく予定です。
- **ヘッドレス DAW ランタイム** — オーディオ＆MIDI のトラック／クリップでプロジェクトを構築
  （split／trim／move、undo／redo つき）。テイクとコンプレーン、クリップ別ワープ、
  MIDI 1.0／2.0 シーケンス、SMF・MIDI 2.0 Clip File の入出力、決定論的でバイト安定な JSON、
  内蔵インストゥルメント経由のオフラインバウンスに対応します。
- **リアルタイムエンジン** — サンプル精度・アロケーションフリーの再生エンジン。トランスポート、
  ワープ対応のクリップ再生（移調あり／ピッチ保持）、先読みページ要求付きの巨大クリップの
  ページ式ストリーミング、ライブ MIDI 入力、楽器個別パラメータまで届くロックフリーの
  オートメーション、キャプチャ／レコーディング。同じエンジンが AudioWorklet を
  通してブラウザでも動きます。

各機能・各ランタイム・各プロセッサの詳細な API は
[ドキュメント](https://libsonare.libraz.net/ja/)を参照してください。

## インストール

```bash
npm install @libraz/libsonare   # JavaScript / TypeScript（WASM、Float32Array を渡す）
pip install libsonare            # Python（WAV/MP3。M4A/AAC などは「対応フォーマット」参照）
```

[`@libraz/libsonare-native`](bindings/node/) は npm に公開していません。リポジトリを
clone してローカル依存として使ってください。ネイティブビルドと FFmpeg の詳細は
同パッケージの README にあります。

## クイックスタート

以下は代表的な機能だけを示したものです。ランタイムごとの完全な API は
ドキュメントサイトにあります。

> **どのランタイムを選ぶ？ ファイル読み込みは効く？** WASM は WAV/MP3 をデコードし、
> その他のブラウザ対応形式はブラウザのコーデックへフォールバックします。Python と Node
> ネイティブアドオンはファイルを直接読み込めます。→
> [利用環境を選ぶ](https://libsonare.libraz.net/ja/docs/getting-started#利用環境を選ぶ)

### JavaScript / TypeScript (WASM)

まずエンコード済みのバイト列を渡します。`Audio.fromMemoryWithBrowserFallback()` は内蔵の
WAV/MP3 デコーダを試し、M4A・AAC・FLAC・OGG などはブラウザの `decodeAudioData()` へ
フォールバックします。

トップレベルの一括 API はリクエストオブジェクトを正準形として受け取ります。位置引数の
呼び出しも互換性のため引き続き利用でき、ステートフルなメソッドや小さなスカラーヘルパーは
本来の形を保ちます。

```typescript
import { Audio, init } from '@libraz/libsonare';

await init();

const bytes = new Uint8Array(await file.arrayBuffer());
const audio = await Audio.fromMemoryWithBrowserFallback(bytes);
const result = audio.analyze(); // BPM・キー・コード・セクションなど
console.log(result.key.name);

// すでにデコード済みなら、Float32Array を直接渡せます。
const decoded = Audio.fromBuffer(samples, sampleRate);
```

解析・特徴量抽出・メータリングだけが必要なら、より小さい
`@libraz/libsonare/analysis` entry を使えます。マスタリング・ミキシング・リアルタイム API を除き、
emsdk 5.0.2 では 0.91 MiB raw / 368 KiB gzip（full entry は 3.88 MiB raw / 1.31 MiB gzip）です。

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
別配布の native CLI は `sonare-cli` なので、両者を `PATH` 上で共存させても挙動が
変わりません。

CLI のコマンド名は、動作モデルが異なるところをあえて分けています。`sonare mix` は
ミキサーシーンを読み込んで 1 つ以上の入力をレンダリングし、`sonare-cli mix-strip` は
1 つの入力にチャンネルストリップを適用します。native の `mix` は `mix-strip` の互換
エイリアスとして残しています。

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

### インストゥルメント & MIDI

MIDI アレンジを内蔵インストゥルメントで音にします——SoundFont は不要です。
`Project` を組み、ノートを置き、NativeSynth プリセット経由でバウンスします。

```python
import libsonare

with libsonare.Project() as project:
    project.set_sample_rate(48000)
    _, clip_id = project.add_midi_clip(0.0, 4.0)             # 開始・長さ（四分音符）
    project.set_midi_events(clip_id, [
        libsonare.Project.midi_note_on(0.0, 0, 0, 60, 100),  # ppq, group, channel, note, velocity
        libsonare.Project.midi_note_off(2.0, 0, 0, 60),
    ])
    audio = project.bounce_with_synth_instrument("e-piano", num_channels=2)  # → float32 音声
```

同じ `Project` ／バウンス API はどのランタイムでも使えます。ホスト供給の
SoundFont を読み込めば、GS 互換の SF2 プレーヤー経由でも鳴らせます。

→ [Python API](https://libsonare.libraz.net/ja/docs/python-api) · [ドキュメント](https://libsonare.libraz.net/ja/)

## 対応フォーマット

| フォーマット | デフォルト¹ | FFmpeg あり² | WASM (`@libraz/libsonare`) |
|--------------|-------------|--------------|----------------------------|
| WAV (PCM 16/24/32, float32) | ✅ | ✅ | 内蔵デコーダ |
| MP3 | ✅ | ✅ | 内蔵デコーダ |
| M4A / AAC / FLAC / OGG / Opus / WMA / … | ❌（明示エラー） | ✅ | ブラウザ対応時はコーデックフォールバック |

¹ **デフォルト**: PyPI ホイールと、FFmpeg dev libs が無い環境でのソースビルド。ホイールは
このモードに固定されているため、インストールがユーザの `libavformat` の有無に左右されません。

² **FFmpeg あり**: FFmpeg をリンクしたソースビルド。CMake が pkg-config 経由で自動検出します
（`-DSONARE_WITH_FFMPEG=AUTO`）。Python: `SONARE_FFMPEG=1 pip install libsonare --no-binary
libsonare`、Node ネイティブ: `SONARE_FFMPEG=1 yarn build`。

WASM バンドルには大きなコーデックライブラリを意図的に含めません。小さな内蔵 WAV/MP3
デコーダとブラウザの `decodeAudioData()` フォールバックが通常の入口で、すでにデコード済み
なら `Float32Array` を渡せます。

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
I/O 抽象化、サンプルデータの同梱、深層学習モデルは意図的に含みません。Windows は非対応で、
Linux・macOS・WebAssembly または WSL2 を利用してください。オーディオコールバックと UI は
呼び出し側が所有します（I/O 境界の唯一の例外が、オプトイン・非公開の実験的 macOS
バックエンドです）。この線引きが、依存ゼロと Apache-2.0 の純度を保ちます。背景は
[Non-goals](https://libsonare.libraz.net/ja/docs/architecture)を参照してください。

## ライセンス

[Apache-2.0](LICENSE)
