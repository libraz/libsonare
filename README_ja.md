# libsonare

[![CI](https://img.shields.io/github/actions/workflow/status/libraz/libsonare/ci.yml?branch=main&label=CI)](https://github.com/libraz/libsonare/actions)
[![npm](https://img.shields.io/npm/v/@libraz/libsonare)](https://www.npmjs.com/package/@libraz/libsonare)
[![PyPI](https://img.shields.io/pypi/v/libsonare)](https://pypi.org/project/libsonare/)
[![codecov](https://codecov.io/gh/libraz/libsonare/branch/main/graph/badge.svg)](https://codecov.io/gh/libraz/libsonare)
[![License](https://img.shields.io/badge/license-Apache--2.0-blue)](https://github.com/libraz/libsonare/blob/main/LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20WebAssembly-lightgrey)](https://github.com/libraz/libsonare)
[![Docs](https://img.shields.io/badge/docs-libsonare.libraz.net-2563eb)](https://libsonare.libraz.net/ja/)

**依存ライブラリ不要の、C++・Python・ブラウザ向けオーディオDSPツールキット —
librosa互換の解析に、放送品質のマスタリング・ミキシング・編集を加えたもの。**

Apache-2.0、ランタイム依存ゼロ、ネイティブとWebAssemblyを単一コードベースで。
C++で動くプロセッサがそのままWASMでブラウザでも動きます — Python不要、
GPL/AGPLなし、モデル重みなし。

📖 **[ドキュメント](https://libsonare.libraz.net/ja/)** &nbsp;·&nbsp; 🎧 **[ブラウザ完結デモ](https://libsonare.libraz.net/ja/demos)** &nbsp;·&nbsp; [はじめに](https://libsonare.libraz.net/ja/docs/getting-started)

- **解析（librosa互換）** — BPM・キー・コード（Viterbi/HMM平滑化・転回形・
  キーコンテキスト）・ビート・ダウンビート・拍子・セクション・音色・ダイナミクス・
  ピッチ（YIN / pYIN）・テンポグラム／PLP・NNLSクロマ・EBU R128 ラウドネス（LUFS）・
  音響特性（ブラインドRT60/EDT、または実測IRからの ISO準拠 RT60/EDT/C50/C80/D50）。
  デフォルト値はlibrosaに揃え、CIで生成したlibrosaリファレンス値と照合検証しています。
- **マスタリング（66個の名前付きDSPプロセッサ、うち18個がデフォルトチェーンに）** —
  EQ、ダイナミクス、マルチバンド、ステレオ、サチュレーション、リペア、マキシマイザー、
  リファレンスマッチング。公開された規格・論文に基づく実装です:
  ITU-R BS.1770-4 ラウドネスとインターサンプル・トゥルーピークリミッティング、
  オールパス位相補償付き Linkwitz-Riley クロスオーバー、Vicanek matched-Z バイクァッド、
  ADAAアンチエイリアスのクリッパー、チューブサチュレーションの Dempwolf 12AX7 三極管モデル、
  Lemireスライディング最大、ポリフェーズFIRオーバーサンプリング。
  リペア系は意図的に古典的DSP（spectral subtraction / MMSE-STSA / LogMMSE）で、
  DNN音源分離やスペクトル修復は対象外です。
- **ミキシング / ルーティング** — リアルタイムセーフなチャンネルストリップ／バスモデル
  （デノーマル対策・ロックフリーなパラメータ変更・プラグインディレイ補償）に、
  パンモード、幅、センド、FXバス、ゴニオメーター／トゥルーピーク計測、シーンプリセット、
  オフラインステレオレンダリングを備えます。
- **編集 & クリエイティブFX** — タイムストレッチ／ピッチシフト、ピッチ補正、
  ノート区間ストレッチ、ボイスチェンジ（ピッチ＋フォルマント）、5種のリバーブエンジン
  （convolution / Dattorro plate / FDN / velvet-noise、そして幾何ベースのルームエンジン）、
  コーラス／フランジャー／フェイザー、ステレオディレイ、ダッキング。
- **幾何ベースのルームアコースティクス** — シューボックス形状からルームインパルス
  レスポンスを合成（`synthesizeRir`）、録音から等価なルームをブラインド推定
  （`estimateRoom` → 体積／寸法／帯域別吸音率／DRR ＋ 正直な信頼度）、録音の残響を
  目標ルームへモーフィング（`roomMorph`）。Apache-2.0・依存なし・決定論的。
- **ヘッドレス DAW / アレンジメントランタイム** — オーディオ＆MIDIのトラック／クリップで
  プロジェクトを構築（split / trim / move を完全な undo/redo つきで）、MIDIシーケンス、
  Standard MIDI File と MIDI 2.0 Clip File（`SMF2CLIP`）の入出力、オートテンポと
  スナップトゥグリッド、決定論的でバイト安定なJSONの保存／読み込み、構造化診断つきの
  レンダリング可能タイムラインへのコンパイル、インターリーブ音声へのオフラインバウンス。
  UI・デバイス設定・プラグインホスト実装は含まない、ヘッドレスのコアのみ。
  C ABI・Python・Node・WASM・CLI で利用できます。
- **どこでも同一ライセンス** — スタック全体（C++・C・Python・Node・WASM・CLI）が Apache-2.0。

## インストール

```bash
npm install @libraz/libsonare   # JavaScript / TypeScript (WASM、Float32Arrayを受け取り)
pip install libsonare            # Python（WAV/MP3対応。M4A/AACは「対応音声フォーマット」参照）
```

Node.jsでネイティブにファイルをデコードしたい場合は、
[`@libraz/libsonare-native`](bindings/node/) をソースからビルドします:

```bash
cd bindings/node
yarn install
yarn build  # pkg-configでFFmpegを自動検出（無ければWAV/MP3、有ればM4A/AAC/FLAC/OGGも対応）
```

明示的に切り替えたい場合:

```bash
SONARE_FFMPEG=0 yarn build  # FFmpegを使わない
SONARE_FFMPEG=1 yarn build  # FFmpegを必須（dev libs不在ならビルド失敗）
```

## クイックスタート

### JavaScript / TypeScript (WASM)

`@libraz/libsonare` はデコード済みの `Float32Array` を受け取ります（Web Audio APIや
JSデコーダで取得してください）。マスタリングDSPは標準のWASMビルドに同梱されています。

**解析**

```typescript
import { init, detectBpm, detectKey, analyze } from '@libraz/libsonare';

await init();

const bpm = detectBpm(samples, sampleRate);
const key = detectKey(samples, sampleRate);  // { name: "C major", confidence: 0.95 }
const result = analyze(samples, sampleRate);

// 詳細なキー判定オプションは明示指定時のみ有効です。デフォルト動作は維持されます。
const keyWithOptions = detectKey(samples, sampleRate, {
  useHpss: true,
  loudnessWeighted: true,
  highPassHz: 80,
  nFft: 4096,
  hopLength: 512,
});
```

**音響特性**

```typescript
import {
  analyzeImpulseResponse,
  detectAcoustic,
  estimateRoom,
  synthesizeRir,
  roomMorph,
} from '@libraz/libsonare';

// 通常音源: ブラインドRT60/EDT推定。ブラインド解析では C50/C80/D50 は NaN。
const blind = detectAcoustic(samples, sampleRate);

// インパルス応答: ISO方式の RT60/EDT と明瞭度指標。
const room = analyzeImpulseResponse(irSamples, sampleRate);

// 録音から等価なルームをブラインド推定: 体積／寸法／帯域別吸音率／DRR と信頼度。
const estimate = estimateRoom(samples, sampleRate);

// シューボックス形状からルームインパルスレスポンスを合成。
const { rir } = synthesizeRir({ lengthM: 7, widthM: 5, heightM: 3, absorption: 0.2 });

// 録音の残響を目標ルームへモーフィング（クリエイティブFX）。
const morphed = roomMorph(samples, sampleRate, { lengthM: 12, widthM: 9, wet: 0.6 });
```

**リズム & コード**

```typescript
import { analyze, detectDownbeats, detectChords } from '@libraz/libsonare';

const downbeats = detectDownbeats(samples, sampleRate);  // 小節頭の時刻（秒）
const { timeSignature } = analyze(samples, sampleRate);  // { numerator: 4, denominator: 4 }

// コード検出の追加機能はすべて明示指定時のみ有効です（デフォルト動作は維持されます）。
const chords = detectChords(samples, sampleRate, {
  useHmm: true,            // Viterbi/HMM による時間方向の平滑化
  detectInversions: true,  // 検出したベース音からスラッシュコードを判定
  useKeyContext: true,     // キー内のコードを優先
  chromaMethod: 'nnls',    // 通常のSTFTクロマの代わりにNNLSクロマを使用
});
```

**テンポグラム・NNLSクロマ・ラウドネス**

```typescript
import {
  onsetEnvelope, tempogram, fourierTempogram, tempogramRatio, plp,
  nnlsChroma, lufs,
} from '@libraz/libsonare';

// オンセット強度包絡線がテンポ領域の特徴量の入力になります。
const env = onsetEnvelope(samples, sampleRate);
const tg = tempogram(env, sampleRate);          // { winLength, nFrames, data }
const ft = fourierTempogram(env, sampleRate);   // { nBins, nFrames, data }
const ratios = tempogramRatio(tg.data, tg.winLength, sampleRate);
const pulse = plp(env, sampleRate);             // 主要な局所パルス

const chroma = nnlsChroma(samples, sampleRate); // { nChroma: 12, nFrames, data }

// EBU R128 ラウドネス計測（マスタリングのラウドネス目標とは別物）。
const loud = lufs(samples, sampleRate);
// { integratedLufs, momentaryLufs, shortTermLufs, loudnessRange }
```

**ピッチと音色の結果**

`pitchYin` と `pitchPyin` は、デフォルトでは無声フレームの `f0` を `NaN` のまま返します。これは librosa に近いピッチ列として扱うためです。後段の処理で有限値だけを扱いたい場合は `fillNa: true` を指定すると、無声フレームが `0` になります。

`analyzeTimbre` は音色の集計値に加えて、解析窓ごとの時系列 `timbreOverTime` も返します。各要素には brightness、warmth、density、roughness、complexity が含まれます。

| 実行環境 | ピッチのオプション | 時間変化する音色フィールド |
|----------|--------------------|----------------------------|
| JavaScript / WASM / Node | `fillNa` | `timbreOverTime` |
| Python | `fill_na` | `timbre_over_time`（`timbreOverTime` エイリアスあり） |
| C ABI | `fill_na` | `timbre_over_time` + `timbre_over_time_count` |

**スペクトル特徴・分解・エフェクト（librosa互換）**

```typescript
import {
  spectralContrast, polyFeatures, zeroCrossings, pitchTuning, estimateTuning,
  decompose, nnFilter, remix, phaseVocoder, hpssWithResidual,
  lufsInterleaved, ebur128LoudnessRange,
} from '@libraz/libsonare';

// スペクトル特徴
const contrast = spectralContrast(samples, sampleRate); // Matrix2d (nBands+1) x nFrames
const poly = polyFeatures(samples, sampleRate);         // Matrix2d (order+1) x nFrames
const crossings = zeroCrossings(samples);               // ゼロ交差インデックス（Int32Array）

// チューニング推定（ビン単位の偏差）
const tuning = estimateTuning(samples, sampleRate);
const fromF0 = pitchTuning(frequencies);

// スペクトログラム分解: NMF 因子 + 最近傍フィルタリング
const { w, h } = decompose(spectrogram, nFeatures, nFrames, 8); // n_components = 8
const filtered = nnFilter(spectrogram, nFeatures, nFrames);

// エフェクト: 区間リミックス、フェーズボコーダによる時間伸縮、HPSS + 残差
const remixed = remix(samples, Int32Array.from([0, 22050, 44100, 66150]));
const faster = phaseVocoder(samples, 1.5, sampleRate);  // rate > 1 で高速化
const { harmonic, percussive, residual } = hpssWithResidual(samples, sampleRate);

// マルチチャンネル／規格準拠ラウドネス
const multi = lufsInterleaved(interleaved, 2, sampleRate); // チャンネル重み付き LUFS + LRA
const lra = ebur128LoudnessRange(samples, sampleRate);     // EBU R128 ラウドネスレンジ（LU）
```

**マスタリング**

```typescript
import {
  init,
  masteringChain,
  masteringChainStereo,
  masteringPairAnalyze,
  masteringPairProcess,
  masteringPairProcessorNames,
  masteringProcess,
  masteringProcessorNames,
} from '@libraz/libsonare';

await init();

const mastered = masteringChain(samples, sampleRate, {
  eq: { tiltDb: 1.0 },
  dynamics: { compressor: { thresholdDb: -24, ratio: 1.5 } },
  saturation: { tape: { driveDb: 1.0, saturation: 0.2 } },
  loudness: { targetLufs: -14, ceilingDb: -1, truePeakOversample: 4 },
});

const stereo = masteringChainStereo(left, right, sampleRate, {
  stereo: { imager: { width: 1.1 }, monoMaker: { amount: 0.2 } },
  loudness: { targetLufs: -14, ceilingDb: -1, truePeakOversample: 4 },
});

// 単一の名前付きプロセッサを適用
const compressed = masteringProcess('dynamics.compressor', samples, sampleRate, {
  thresholdDb: -24,
  ratio: 1.5,
});

// リファレンス参照のマスタリング
const matched = masteringPairProcess('match.abCrossfade', source, reference, sampleRate, {
  mix: 0.25,
});
const loudnessJson = masteringPairAnalyze(
  'match.referenceLoudness', source, reference, sampleRate,
);

// 利用可能なプロセッサ名を取得
masteringProcessorNames();     // ['dynamics.compressor', 'eq.parametric', ...]
masteringPairProcessorNames(); // ['match.abCrossfade', ...]
```

プリセットを使った一括マスタリングと、ブロック単位のストリーミング版も
利用できます。WASMでは `masteringChain` / `StreamingMasteringChain` の
config が**ネスト形式**、`masterAudio` の overrides のみ **フラットな
ドット記法キー**（Node / Python の overrides と同じ）になります:

```typescript
// プリセット適用（一括）とストリーミング版
import { masterAudio, masteringPresetNames, StreamingMasteringChain } from '@libraz/libsonare';
masteringPresetNames(); // ['pop', 'edm', 'acoustic', 'hipHop', 'aiMusic', 'speech', 'streaming', 'youtube', 'broadcast', 'podcast', 'audiobook', 'cinema', 'jpop', 'ambient', 'lofi', 'classical', 'drumAndBass', 'techno', 'metal', 'trap', 'rnb', 'jazz', 'kpop', 'trance', 'gameOst']
const out = masterAudio(samples, sampleRate, 'aiMusic', { 'loudness.targetLufs': -13 });

const chain = new StreamingMasteringChain({ eq: { tiltDb: 0.5 } });
chain.prepare(48000, 512, 1);
const block = chain.processMono(new Float32Array(512));
chain.delete();
```

**ミキシング**

```typescript
import { mixStereo, mixingScenePresetJson, mixingScenePresetNames } from '@libraz/libsonare';

mixingScenePresetNames(); // ['vocalReverbSend', ...]
const sceneJson = mixingScenePresetJson('vocalReverbSend');

const mix = mixStereo([vocalL, musicL], [vocalR, musicR], sampleRate, {
  faderDb: [-3, -12],
  pan: [0, -0.2],
  width: [1, 0.9],
});
// { left, right, meters }
```

**ヘッドレス DAW プロジェクト**

```typescript
import { Project } from '@libraz/libsonare';

// WASM は `new Project()` で構築。Node ネイティブは `Project.create()` を使います。
// それ以外のメソッド面は同一です。
const project = new Project();
project.setSampleRate(48000);

// 楽曲上の位置は PPQ（4分音符）。
const { clipId } = project.addMidiClip(0, 4);          // { trackId, clipId }
project.setMidiEvents(clipId, [
  Project.midiNoteOn(0, 0, 0, 60, 100),                // ppq, group, channel, note, velocity
  Project.midiNoteOff(1, 0, 0, 60),
]);

const json = project.toJson();                         // 決定論的・ビルド内でバイト安定
const smf = project.exportSmf();                       // Uint8Array — Standard MIDI File
const midi2 = project.exportClipFile();                // Uint8Array — MIDI 2.0 Clip File（ロスレス）

const { hasTimeline, diagnostics } = project.compile();
const audio = project.bounce({ numChannels: 2 });      // インターリーブ Float32Array

project.delete();                                      // Node ネイティブ: project.destroy()
```

### Python

`pip install libsonare` で提供されるホイールは **WAV/MP3のみ対応** です
（librosa / pydub / soundfile と同じ慣習）。M4A/AAC/FLAC/OGG を扱う場合は、
事前に外部 `ffmpeg` で変換するか、FFmpegをリンクしたソースビルドを行ってください:

```bash
SONARE_FFMPEG=1 pip install libsonare --no-binary libsonare
# システム側のFFmpeg dev libsが必要:
# brew install ffmpeg / apt install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev
```

```python
import libsonare

audio = libsonare.Audio.from_file("song.mp3")
print(f"BPM: {audio.detect_bpm()}, Key: {audio.detect_key()}")

# 詳細なキー判定オプションは明示指定時のみ有効です。デフォルト動作は維持されます。
key_with_options = audio.detect_key(
    use_hpss=True,
    loudness_weighted=True,
    high_pass_hz=80.0,
)

acoustic = audio.detect_acoustic()  # ブラインドRT60/EDT。C50/C80/D50 は NaN
ir_params = libsonare.analyze_impulse_response(ir_samples, sample_rate=sr)

# ダウンビート・拍子・コードの追加機能（すべて明示指定時のみ有効）
downbeats = audio.detect_downbeats()              # 小節頭の時刻（秒）
time_signature = audio.analyze().time_signature   # 例: 4/4
chords = audio.detect_chords(
    use_hmm=True,             # Viterbi/HMM による時間方向の平滑化
    detect_inversions=True,   # 検出したベース音からスラッシュコードを判定
    use_key_context=True,     # キー内のコードを優先
    chroma_method="nnls",     # 通常のSTFTクロマの代わりにNNLSクロマを使用
)

# テンポグラム / NNLSクロマ / EBU R128 ラウドネス
env = audio.onset_envelope()                     # オンセット強度包絡線
n_frames, tg = libsonare.tempogram(env, sample_rate=sr)
n_frames_ft, ft = libsonare.fourier_tempogram(env, sample_rate=sr)
ratios = libsonare.tempogram_ratio(tg)
pulse = libsonare.plp(env, sample_rate=sr)

nf, chroma = audio.nnls_chroma()                 # (n_frames, 12 x n_frames の行優先配列)

loud = audio.lufs()  # integrated_lufs / momentary_lufs / short_term_lufs / loudness_range を返す
mom = audio.momentary_lufs()                     # ブロック単位の時系列
short = audio.short_term_lufs()

# librosa互換のスペクトル特徴・分解・エフェクト
contrast = libsonare.spectral_contrast(samples, sample_rate=sr)  # (n_bands+1) x n_frames
poly = libsonare.poly_features(samples, sample_rate=sr)          # (order+1) x n_frames
crossings = libsonare.zero_crossings(samples)                    # ゼロ交差インデックス
tuning = libsonare.estimate_tuning(samples, sample_rate=sr)      # 偏差（ビン単位）
offset = libsonare.pitch_tuning(frequencies)

w, h = libsonare.decompose(spectrogram, n_features, n_frames, 8)  # NMF 因子
filtered = libsonare.nn_filter(spectrogram, n_features, n_frames)

remixed = libsonare.remix(samples, [0, sr, 2 * sr, 3 * sr], sample_rate=sr)
faster = libsonare.phase_vocoder(samples, sample_rate=sr, rate=1.5)
hpss = libsonare.hpss_with_residual(samples, sample_rate=sr)      # harmonic/percussive/residual

multi = libsonare.lufs_interleaved(interleaved, channels=2, sample_rate=sr)
lra = libsonare.ebur128_loudness_range(samples, sample_rate=sr)   # EBU R128 LRA (LU)

# マスタリング — 返り値は MasteringResult(samples, sample_rate,
# input_lufs, output_lufs, applied_gain_db, latency_samples)
result = audio.mastering(target_lufs=-14.0, ceiling_db=-1.0)
print(f"{result.input_lufs:.1f} LUFS → {result.output_lufs:.1f} LUFS "
      f"(gain {result.applied_gain_db:+.2f} dB)")

# 単一プロセッサ / リファレンス参照解析
compressed = libsonare.mastering_process(
    "dynamics.compressor", samples, sample_rate=44100,
    params={"thresholdDb": -24, "ratio": 1.5},
)
loudness_json = libsonare.mastering_pair_analyze(
    "match.referenceLoudness", source, reference, sample_rate=44100,
)

# 利用可能なプロセッサ名を取得
libsonare.mastering_processor_names()       # ['dynamics.compressor', ...]
libsonare.mastering_pair_processor_names()  # ['match.abCrossfade', ...]

# プリセットを使った一括マスタリング + ストリーミング
libsonare.mastering_preset_names()  # ['pop', 'edm', 'acoustic', 'hipHop', 'aiMusic', 'speech', 'streaming', 'youtube', 'broadcast', 'podcast', 'audiobook', 'cinema', 'jpop', 'ambient', 'lofi', 'classical', 'drumAndBass', 'techno', 'metal', 'trap', 'rnb', 'jazz', 'kpop', 'trance', 'gameOst']
result = libsonare.master_audio(samples, sample_rate=sr, preset='aiMusic',
                                 overrides={'loudness.targetLufs': -13})

with libsonare.StreamingMasteringChain({'eq.tilt.tiltDb': 0.5}) as chain:
    chain.prepare(44100, 512, 1)
    out = chain.process_mono([0.0] * 512)

# ミキシングプリセットとオフラインステレオレンダリング
libsonare.mixing_scene_preset_names()  # ['vocalReverbSend', ...]
scene_json = libsonare.mixing_scene_preset_json("vocalReverbSend")
mix = libsonare.mix_stereo(
    [(vocal_l, vocal_r), (music_l, music_r)],
    sample_rate=sr,
    fader_db=[-3.0, -12.0],
    pan=[0.0, -0.2],
    width=[1.0, 0.9],
)

# ヘッドレス DAW プロジェクト（オーディオ + MIDI アレンジ。PPQ = 4分音符）
with libsonare.Project() as project:
    project.set_sample_rate(48000)
    track_id, clip_id = project.add_midi_clip(0.0, 4.0)
    project.set_midi_events(clip_id, [
        libsonare.Project.midi_note_on(0.0, 0, 0, 60, 100),
        libsonare.Project.midi_note_off(1.0, 0, 0, 60),
    ])
    json_str = project.to_json()           # 決定論的・ビルド内でバイト安定
    smf = project.export_smf()             # bytes — Standard MIDI File
    result = project.compile()             # has_timeline / messages / diagnostics
    audio = project.bounce(num_channels=2) # (frames, channels) の float32 ndarray
```

### Python CLI

```bash
pip install libsonare

# 解析
sonare analyze song.mp3
# > Estimated BPM : 161.00 BPM  (conf 75.0%)
# > Estimated Key : C major  (conf 100.0%)

sonare bpm song.mp3 --json

# 拡張解析（C++ CLI と同等）
sonare acoustic room.wav --json          # ブラインドRT60/EDT（--ir でIRベースの明瞭度指標）
sonare estimate-room room.wav --json     # ブラインド推定: 体積／寸法／吸音率／DRR ＋ 信頼度
sonare synthesize-rir --length 7 --width 5 --height 3 -o rir.wav   # 形状からRIRを合成
sonare room-morph dry.wav --length 12 --width 9 --wet 0.6 -o morphed.wav  # 目標ルームへモーフィング
sonare lufs song.wav --series            # EBU R128 integrated/momentary/short-term
sonare rhythm song.wav --json
sonare dynamics song.wav --json
sonare timbre song.wav --json
sonare tempogram song.wav --json
sonare nnls-chroma song.wav --json

# マスタリング
sonare mastering song.wav -o mastered.wav --target-lufs -14
sonare mastering-processor song.wav --processor dynamics.compressor \
    --params thresholdDb=-24,ratio=1.5 -o compressed.wav
sonare mastering-pair-analyze source.wav --reference reference.wav \
    --analysis match.referenceLoudness
sonare mastering-processors                 # 利用可能なプロセッサ一覧

# ミキシング
sonare mixing-presets
sonare mixing-preset --preset vocalReverbSend
sonare mix input.wav -o mix.wav --fader-db -3 --pan 0.1 --pan-mode stereo-pan --width 1.1

# ヘッドレス DAW プロジェクト（アレンジ / MIDI）
sonare project new -o song.json                          # 空のプロジェクトを作成
sonare project validate --in song.json                   # プロジェクトJSONを往復／検証
sonare project compile --in song.json                    # コンパイル + 診断を表示
sonare project bounce --in song.json -o mix.wav          # オフラインでWAVにレンダリング
sonare project export-smf --in song.json -o song.mid     # テンポマップ + MIDIクリップ → SMF
sonare project import-smf --smf song.mid -o song.json     # SMF → 新規プロジェクトJSON
sonare project export-midi2 --in song.json -o song.midi2 # → MIDI 2.0 Clip File（ロスレス）
sonare project import-midi2 --midi2 song.midi2 -o song.json
```

### C++

```cpp
#include <sonare/sonare.h>           // 解析 + 特徴量 + エフェクト
#include <sonare/mastering/master.h> // マスタリングチェイン & プロセッサ

auto audio = sonare::Audio::from_file("music.mp3");
auto result = sonare::MusicAnalyzer(audio).analyze();
std::cout << "BPM: " << result.bpm
          << ", Key: " << result.key.to_string() << std::endl;
```

## 機能

### 解析（librosa互換）

| 音楽                  | スペクトル / ピッチ  | ストリーミング       |
|-----------------------|----------------------|----------------------|
| BPM / テンポ          | STFT / iSTFT         | リアルタイム解析     |
| キー検出              | メルスペクトログラム | 逐次BPM              |
| ビート / ダウンビート | MFCC                 | 逐次キー             |
| 拍子 / メーター       | クロマ / NNLSクロマ  | オンセットイベント   |
| コード(HMM/転回形)    | CQT / VQT            |                      |
| セクション検出        | テンポグラム / PLP   |                      |
| 音色 / ダイナミクス   | スペクトル特徴量     |                      |
| RT60 / EDT / C50      | 音響特性解析         |                      |
| ピッチ (YIN/pYIN)     | オンセット検出       |                      |
| ラウドネス(EBU R128)  | オンセット包絡線     |                      |

### マスタリング（66個のDSPプロセッサ）

| ダイナミクス                | EQ                          | マルチバンド / ステレオ                  |
|-----------------------------|-----------------------------|------------------------------------------|
| コンプレッサー              | パラメトリック / グラフィック | マルチバンドコンプレッサー / EQ / リミッター |
| リミッター / ブリックウォール| リニアフェーズ / ミニマムフェーズ | Linkwitz-Riley クロスオーバー（位相補償）|
| エキスパンダー / ゲート     | ダイナミックEQ              | ステレオイメージャー / M-S / Haas        |
| ディエッサー                | パッシブ / ステップ式EQ     | フェーズアライン / モノメーカー / 互換   |
| トランジェントシェイパー    | チルト / シェルビング       |                                          |

| サチュレーション / リペア              | マキシマイザー / マッチ                  | ビルディングブロック          |
|---------------------------------------|-----------------------------------------|-------------------------------|
| チューブ (Dempwolf 12AX7) / テープ    | トゥルーピークリミッター (ITU-R BS.1770-4) | ポリフェーズFIRオーバーサンプラ |
| トランス / エキサイター / ビットクラッシャー | ラウドネスオプティマイザ (LUFSターゲット) | ADAAアンチエイリアスシェイピング |
| デクリック / デクリップ / デクラックル | アダプティブリリース                    | Vicanek matched-Z バイクァッド |
| デノイズ / ディリバーブ / デハム      | リファレンスEQ / ラウドネス / スペクトラム | パーティションドコンボルバー   |

リペア系は意図的に古典的DSPに限定しています。`denoise_classical` は明示的な
ノイズ推定に基づく spectral subtraction、MMSE-STSA、LogMMSE を扱い、
DNN復元、音源分離、対話型スペクトル修復はスコープ外です。

EQ の位相モードは既存互換を維持します。Zero Latency は RBJ バイクァッドを
既定のまま使い、Natural Phase は Vicanek matched-Z IIR に解決します。
高域シェルフで Vicanek の端点ゲイン誤差が固定許容値を超える場合は RBJ に
フォールバックします。

マスタリングはデフォルトでビルドされます（`BUILD_MASTERING=ON`）。
`cmake -DBUILD_MASTERING=OFF` で解析専用ビルド（バイナリを小さく）にもできます。

### ミキシング / ルーティング

| チャンネルストリップ       | ルーティング / シーンAPI      | メーター / QA                 |
|----------------------------|-------------------------------|-------------------------------|
| 入力トリム / フェーダー / 極性 | センドとFXバス             | ピーク / RMS / トゥルーピーク |
| バランス / ステレオ / デュアルパン | バスインサートとグラフPDC | 相関 / モノ幅                 |
| 幅とゲインオートメーション | C / Node / Python / WASM / CLI| Golden hash とRTテスト        |
| インサートホスティング / サイドチェイン | 永続シーンミキサー | 処理中の無アロケーション検証 |

ミキシングはデフォルトでビルドされます（`BUILD_MIXING=ON`）。
インサートホスティングにはマスタリングのプロセッサインターフェースを利用します。
解析／マスタリング専用ビルドにする場合は `cmake -DBUILD_MIXING=OFF` を指定してください。

### ヘッドレス DAW / アレンジメント

| アレンジメントモデル          | MIDI とファイル入出力           | コンパイル & レンダリング      |
|-------------------------------|---------------------------------|--------------------------------|
| オーディオ/MIDI/AUX トラック・クリップ | MIDI 1.0 + MIDI 2.0 シーケンス | レンダリング可能タイムラインへコンパイル |
| split / trim / move、undo / redo | SMF の入出力                | 構造化されたコンパイル診断     |
| オートテンポ・スナップ・ワープ | MIDI 2.0 Clip File（ロスレス）  | 決定論的なオフラインバウンス   |
| プログラム/バンク、クリップ別MIDI-FX | トラック別MIDIデスティネーション | C / Node / Python / WASM / CLI |

アレンジメントランタイムはヘッドレスのコアのみです。UI・デバイス設定・
プラグインホスト実装は含みません（[非目標](#非目標non-goals)参照）。プロジェクト状態は
決定論的でバイト安定なJSONにシリアライズされ、`bounce` は同一プロジェクト・同一オプションなら
ビルド内でビット一致します。

## パフォーマンス

解析はネイティブC++で動作し、効果のある箇所（HPSSメディアンフィルタ、
`analyze()` のフルパイプライン）でマルチスレッドを使用します。ベンチマーク用素材では、
HPSSやpYINなどの反復アルゴリズムとフルパイプラインはlibrosaの相当処理より明確に高速です。
一方、FFT律速の単発特徴量（STFT、Mel、MFCC）はおおむね同等です。
WebAssemblyはシングルスレッドのため、マルチスレッドによる高速化は適用されません。

マスタリングDSPでは、ITU仕様準拠のポリフェーズオーバーサンプリング、
ADAA（積分による反エイリアス）、ホットパスでのSIMD対応Eigen線形代数を採用しています。

測定方法と特徴量ごとの数値は、
[ベンチマーク](https://libsonare.libraz.net/ja/docs/benchmarks)を参照してください。

## librosa互換性

デフォルトパラメータはlibrosaに揃えています:
- サンプルレート: 22050 Hz
- n_fft: 2048, hop_length: 512, n_mels: 128
- fmin: 0.0, fmax: sr/2

## 対応音声フォーマット

| フォーマット | デフォルト¹ | FFmpegあり² | WASM (`@libraz/libsonare`) |
|--------------|-------------|-------------|----------------------------|
| WAV (PCM 16/24/32, float32) | ✅ | ✅ | n/a (sampleを直接渡す) |
| MP3 | ✅ | ✅ | n/a |
| M4A / AAC / FLAC / OGG / Opus / WMA / ... | ❌（明示エラー） | ✅ | n/a (Web Audio APIを使用) |

¹ **デフォルト**: PyPIホイール（`pip install libsonare`）と、FFmpeg dev libsが
無い環境でのソースビルド。PyPIホイールはこのモードに固定されており、
ユーザの `libavformat` の有無に左右されません。

² **FFmpegあり**: FFmpegをリンクしたソースビルド。CMakeはpkg-config経由で
自動検出します（`-DSONARE_WITH_FFMPEG=AUTO` がデフォルト）。
`-DSONARE_WITH_FFMPEG=ON`/`OFF` で強制できます。Python:
`SONARE_FFMPEG=1 pip install libsonare --no-binary libsonare`。
Nodeネイティブ: `SONARE_FFMPEG=1 yarn build`。

WASMは設計上ファイルデコーダを同梱しません。
Web Audio APIなどでデコードした `Float32Array` を渡してください。

## ソースからビルド

```bash
# ネイティブ（FFmpeg自動検出。マスタリングとミキシングはデフォルトでON）
make build && make test

# 解析専用（バイナリを小さくしたいとき）
cmake -B build -DBUILD_MASTERING=OFF -DBUILD_MIXING=OFF && cmake --build build

# WebAssembly（マスタリング同梱）
make wasm

# リリースビルド（最適化）
make release
```

## ドキュメント

ドキュメントとブラウザ完結デモ: **[libsonare.libraz.net](https://libsonare.libraz.net/ja/)**（[デモ](https://libsonare.libraz.net/ja/demos)）。

**まず読む**
- [イントロダクション](https://libsonare.libraz.net/ja/docs/introduction) · [はじめに](https://libsonare.libraz.net/ja/docs/getting-started) · [インストール](https://libsonare.libraz.net/ja/docs/installation) · [使用例](https://libsonare.libraz.net/ja/docs/examples)

**利用環境別 API**
- [ブラウザ / WASM](https://libsonare.libraz.net/ja/docs/wasm) · [JavaScript](https://libsonare.libraz.net/ja/docs/js-api) · [Python](https://libsonare.libraz.net/ja/docs/python-api) · [Node.js ネイティブ](https://libsonare.libraz.net/ja/docs/native-bindings) · [C++](https://libsonare.libraz.net/ja/docs/cpp-api) · [CLI](https://libsonare.libraz.net/ja/docs/cli)

**作りたいもの別**
- [マスタリングプロセッサ](https://libsonare.libraz.net/ja/docs/mastering-processors) · [ミキシングエンジン](https://libsonare.libraz.net/ja/docs/mixing) · [編集 DSP](https://libsonare.libraz.net/ja/docs/editing-dsp) · [リアルタイムとストリーミング](https://libsonare.libraz.net/ja/docs/realtime-streaming) · [ルーム音響解析](https://libsonare.libraz.net/ja/docs/acoustic-analysis)

**詳しく知る**
- [アーキテクチャ](https://libsonare.libraz.net/ja/docs/architecture) · [librosa互換性](https://libsonare.libraz.net/ja/docs/librosa-compatibility) · [ベンチマーク](https://libsonare.libraz.net/ja/docs/benchmarks) · [用語集](https://libsonare.libraz.net/ja/docs/glossary)

## 非目標（Non-goals）

libsonare は意図的に次の機能を含めません。

- **プラグイン級の楽器／クリエイティブエフェクト** — Tone.js、プラグインホスト、DAW を使ってください
- **内蔵音声合成**（オシレーター、サンプラー、楽器 DSP）— スコープ外です
- **リアルタイム I/O 抽象化**（PortAudio/JACK ラッパー）— 呼び出し側が I/O を扱います
- **完全な DAW アプリケーションワークフロー**（UI、デバイス設定、プラグインホスト実装）—
  呼び出し側がその層を提供し、libsonare は headless arrangement/runtime core に集中します
- **深層学習モデル**（同梱するモデル重みなし、推論ランタイムなし）— 依存ゼロと Apache-2.0 の純度を保つためです

この境界により、libsonare は **解析 + マスタリング + ミキサーDSP + headless arrangement runtime** に集中し、
依存ゼロの性質を維持できます。

## ライセンス

[Apache-2.0](LICENSE)
