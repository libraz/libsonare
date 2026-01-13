# JavaScript/TypeScript API リファレンス

libsonare JavaScript/TypeScript インターフェースの完全な API リファレンス。

## 目次

- [インストール](#インストール)
- [初期化](#初期化)
- [コア関数](#コア関数)
- [型とインターフェース](#型とインターフェース)
- [列挙型](#列挙型)
- [エラーハンドリング](#エラーハンドリング)
- [高度な使用法](#高度な使用法)

---

## インストール

### npm/yarn

```bash
npm install @libraz/sonare
# または
yarn add @libraz/sonare
```

### インポート

```typescript
// ES Modules (推奨)
import {
  init,
  detectBpm,
  detectKey,
  detectBeats,
  detectOnsets,
  analyze,
  analyzeWithProgress,
  version
} from '@libraz/sonare';

// CommonJS
const sonare = require('@libraz/sonare');
```

---

## 初期化

解析関数を使用する前に、WASM モジュールを初期化する必要があります。

### `init(options?)`

WASM モジュールを初期化します。解析関数を使用する前に呼び出す必要があります。

```typescript
async function init(options?: InitOptions): Promise<void>

interface InitOptions {
  /**
   * WASM ファイルを見つけるためのカスタム関数。
   * カスタム CDN やバンドラー設定に便利。
   */
  locateFile?: (path: string, prefix: string) => string;
}
```

**例:**

```typescript
import { init, detectBpm } from '@libraz/sonare';

// 基本的な初期化
await init();

// カスタムファイルロケーション
await init({
  locateFile: (path, prefix) => {
    return `/custom/wasm/path/${path}`;
  }
});

// これで解析関数を使用可能
const bpm = detectBpm(samples, sampleRate);
```

### `isInitialized()`

モジュールが初期化済みかどうかを確認します。

```typescript
function isInitialized(): boolean
```

### `version()`

ライブラリのバージョンを取得します。

```typescript
function version(): string  // 例: "1.0.0"
```

---

## コア関数

### `detectBpm(samples, sampleRate)`

オーディオサンプルから BPM (テンポ) を検出します。

```typescript
function detectBpm(samples: Float32Array, sampleRate: number): number
```

**パラメータ:**
- `samples` - モノラルオーディオサンプル (Float32Array、範囲 -1.0 〜 1.0)
- `sampleRate` - サンプルレート (Hz) (例: 44100、48000)

**戻り値:** 検出された BPM (通常 60-200)

**精度:** ±2 BPM

**例:**

```typescript
import { init, detectBpm } from '@libraz/sonare';

await init();

// AudioBuffer からサンプルを取得 (Web Audio API)
const audioCtx = new AudioContext();
const response = await fetch('song.mp3');
const arrayBuffer = await response.arrayBuffer();
const audioBuffer = await audioCtx.decodeAudioData(arrayBuffer);

const samples = audioBuffer.getChannelData(0);  // モノラル
const sampleRate = audioBuffer.sampleRate;

const bpm = detectBpm(samples, sampleRate);
console.log(`BPM: ${bpm}`);  // 例: "BPM: 120"
```

---

### `detectKey(samples, sampleRate)`

オーディオサンプルから音楽キーを検出します。

```typescript
function detectKey(samples: Float32Array, sampleRate: number): Key
```

**パラメータ:**
- `samples` - モノラルオーディオサンプル
- `sampleRate` - サンプルレート (Hz)

**戻り値:** root、mode、confidence、name を含む [`Key`](#key) オブジェクト

**例:**

```typescript
import { init, detectKey, PitchClass, Mode } from '@libraz/sonare';

await init();

const key = detectKey(samples, sampleRate);

console.log(`キー: ${key.name}`);           // "C major"
console.log(`短縮形: ${key.shortName}`);    // "C"
console.log(`ルート: ${key.root}`);         // 0 (PitchClass.C)
console.log(`モード: ${key.mode}`);         // 0 (Mode.Major)
console.log(`信頼度: ${(key.confidence * 100).toFixed(1)}%`);

// 特定のキーをチェック
if (key.root === PitchClass.C && key.mode === Mode.Major) {
  console.log('曲は C メジャーです！');
}
```

---

### `detectBeats(samples, sampleRate)`

オーディオサンプルからビート時刻を検出します。

```typescript
function detectBeats(samples: Float32Array, sampleRate: number): Float32Array
```

**パラメータ:**
- `samples` - モノラルオーディオサンプル
- `sampleRate` - サンプルレート (Hz)

**戻り値:** 秒単位のビート時刻の Float32Array

**例:**

```typescript
import { init, detectBeats } from '@libraz/sonare';

await init();

const beats = detectBeats(samples, sampleRate);

console.log(`${beats.length} 個のビートを検出`);

// 最初の 10 個のビート時刻を表示
for (let i = 0; i < Math.min(10, beats.length); i++) {
  console.log(`ビート ${i + 1}: ${beats[i].toFixed(3)}秒`);
}
```

---

### `detectOnsets(samples, sampleRate)`

オーディオサンプルからオンセット時刻 (音の立ち上がり) を検出します。

```typescript
function detectOnsets(samples: Float32Array, sampleRate: number): Float32Array
```

**パラメータ:**
- `samples` - モノラルオーディオサンプル
- `sampleRate` - サンプルレート (Hz)

**戻り値:** 秒単位のオンセット時刻の Float32Array

---

### `analyze(samples, sampleRate)`

完全な音楽解析を実行します。

```typescript
function analyze(samples: Float32Array, sampleRate: number): AnalysisResult
```

**パラメータ:**
- `samples` - モノラルオーディオサンプル
- `sampleRate` - サンプルレート (Hz)

**戻り値:** BPM、キー、ビート、コード、セクションなどを含む [`AnalysisResult`](#analysisresult)

**例:**

```typescript
import { init, analyze } from '@libraz/sonare';

await init();

const result = analyze(samples, sampleRate);

console.log('=== 音楽解析 ===');
console.log(`BPM: ${result.bpm} (信頼度: ${(result.bpmConfidence * 100).toFixed(0)}%)`);
console.log(`キー: ${result.key.name}`);
console.log(`拍子: ${result.timeSignature.numerator}/${result.timeSignature.denominator}`);

console.log(`\nビート数: ${result.beats.length}`);

console.log('\nコード:');
for (const chord of result.chords) {
  console.log(`  ${chord.name} [${chord.start.toFixed(2)}秒 - ${chord.end.toFixed(2)}秒]`);
}

console.log('\nセクション:');
for (const section of result.sections) {
  console.log(`  ${section.name} [${section.start.toFixed(2)}秒 - ${section.end.toFixed(2)}秒]`);
}

console.log(`\n楽曲形式: ${result.form}`);  // 例: "IABABCO"
```

---

### `analyzeWithProgress(samples, sampleRate, onProgress)`

進捗レポート付きで完全な音楽解析を実行します。

```typescript
function analyzeWithProgress(
  samples: Float32Array,
  sampleRate: number,
  onProgress: ProgressCallback
): AnalysisResult

type ProgressCallback = (progress: number, stage: string) => void;
```

**パラメータ:**
- `samples` - モノラルオーディオサンプル
- `sampleRate` - サンプルレート (Hz)
- `onProgress` - 進捗 (0.0-1.0) とステージ名を受け取るコールバック

**進捗ステージ:**
| ステージ | 説明 | 進捗範囲 |
|---------|------|----------|
| `"bpm"` | BPM 検出 | 0.0 - 0.15 |
| `"key"` | キー検出 | 0.15 - 0.25 |
| `"beats"` | ビートトラッキング | 0.25 - 0.45 |
| `"chords"` | コード認識 | 0.45 - 0.65 |
| `"sections"` | セクション検出 | 0.65 - 0.85 |
| `"timbre"` | 音色解析 | 0.85 - 0.95 |
| `"dynamics"` | ダイナミクス解析 | 0.95 - 1.0 |

**例:**

```typescript
import { init, analyzeWithProgress } from '@libraz/sonare';

await init();

const result = analyzeWithProgress(samples, sampleRate, (progress, stage) => {
  const percent = Math.round(progress * 100);
  console.log(`${stage}: ${percent}%`);

  // UI プログレスバーを更新
  updateProgressBar(percent, stage);
});

console.log(`解析完了: BPM=${result.bpm}`);
```

**React の例:**

```tsx
import { useState } from 'react';
import { init, analyzeWithProgress, AnalysisResult } from '@libraz/sonare';

function AudioAnalyzer() {
  const [progress, setProgress] = useState(0);
  const [stage, setStage] = useState('');
  const [result, setResult] = useState<AnalysisResult | null>(null);

  const handleAnalyze = async (samples: Float32Array, sampleRate: number) => {
    await init();

    const analysisResult = analyzeWithProgress(samples, sampleRate, (p, s) => {
      setProgress(p);
      setStage(s);
    });

    setResult(analysisResult);
  };

  return (
    <div>
      {stage && (
        <div>
          <div>{stage}: {Math.round(progress * 100)}%</div>
          <progress value={progress} max={1} />
        </div>
      )}
      {result && <div>BPM: {result.bpm}</div>}
    </div>
  );
}
```

---

## 型とインターフェース

### Key

検出された音楽キー。

```typescript
interface Key {
  /** ピッチクラス (0-11、C=0、B=11) */
  root: PitchClass;

  /** モード (Major=0、Minor=1) */
  mode: Mode;

  /** 検出信頼度 (0.0 〜 1.0) */
  confidence: number;

  /** フルネーム (例: "C major"、"A minor") */
  name: string;

  /** 短縮名 (例: "C"、"Am") */
  shortName: string;
}
```

### Beat

検出されたビート。

```typescript
interface Beat {
  /** 秒単位の時間 */
  time: number;

  /** ビート強度 (0.0 〜 1.0) */
  strength: number;
}
```

### Chord

検出されたコード。

```typescript
interface Chord {
  /** ルートピッチクラス */
  root: PitchClass;

  /** コードクオリティ */
  quality: ChordQuality;

  /** 開始時間 (秒) */
  start: number;

  /** 終了時間 (秒) */
  end: number;

  /** 検出信頼度 (0.0 〜 1.0) */
  confidence: number;

  /** コード名 (例: "C"、"Am"、"G7") */
  name: string;
}
```

### Section

検出された楽曲セクション。

```typescript
interface Section {
  /** セクションタイプ */
  type: SectionType;

  /** 開始時間 (秒) */
  start: number;

  /** 終了時間 (秒) */
  end: number;

  /** 相対エネルギーレベル (0.0 〜 1.0) */
  energyLevel: number;

  /** 検出信頼度 (0.0 〜 1.0) */
  confidence: number;

  /** セクション名 (例: "Intro"、"Verse 1"、"Chorus") */
  name: string;
}
```

### TimeSignature

検出された拍子。

```typescript
interface TimeSignature {
  /** 小節あたりの拍数 (例: 4) */
  numerator: number;

  /** 拍の単位 (例: 4 = 四分音符) */
  denominator: number;

  /** 検出信頼度 (0.0 〜 1.0) */
  confidence: number;
}
```

### Timbre

音色特性。

```typescript
interface Timbre {
  /** 高周波成分 (0.0 〜 1.0) */
  brightness: number;

  /** 低周波強調 (0.0 〜 1.0) */
  warmth: number;

  /** スペクトル密度 (0.0 〜 1.0) */
  density: number;

  /** 不協和度/非調和度 (0.0 〜 1.0) */
  roughness: number;

  /** スペクトル複雑度 (0.0 〜 1.0) */
  complexity: number;
}
```

### Dynamics

ダイナミクス特性。

```typescript
interface Dynamics {
  /** ピーク対 RMS ダイナミックレンジ (dB) */
  dynamicRangeDb: number;

  /** 短期ラウドネス変動 (dB) */
  loudnessRangeDb: number;

  /** ピーク対 RMS 比 */
  crestFactor: number;

  /** 強くコンプレッションされている場合 true */
  isCompressed: boolean;
}
```

### RhythmFeatures

リズム特性。

```typescript
interface RhythmFeatures {
  /** シンコペーション量 (0.0 〜 1.0) */
  syncopation: number;

  /** グルーブタイプ ("straight"、"shuffle"、"swing") */
  grooveType: string;

  /** リズミック規則性 (0.0 〜 1.0) */
  patternRegularity: number;
}
```

### AnalysisResult

完全な解析結果。

```typescript
interface AnalysisResult {
  /** 検出された BPM */
  bpm: number;

  /** BPM 検出信頼度 (0.0 〜 1.0) */
  bpmConfidence: number;

  /** 検出された音楽キー */
  key: Key;

  /** 検出された拍子 */
  timeSignature: TimeSignature;

  /** 検出されたビートの配列 */
  beats: Beat[];

  /** 検出されたコードの配列 */
  chords: Chord[];

  /** 検出されたセクションの配列 */
  sections: Section[];

  /** 全体的な音色特性 */
  timbre: Timbre;

  /** ダイナミクス特性 */
  dynamics: Dynamics;

  /** リズム特性 */
  rhythm: RhythmFeatures;

  /** 楽曲形式文字列 (例: "IABABCO") */
  form: string;
}
```

---

## 列挙型

### PitchClass

ピッチクラス (半音階)。

```typescript
const PitchClass = {
  C: 0,
  Cs: 1,   // C#/Db
  D: 2,
  Ds: 3,   // D#/Eb
  E: 4,
  F: 5,
  Fs: 6,   // F#/Gb
  G: 7,
  Gs: 8,   // G#/Ab
  A: 9,
  As: 10,  // A#/Bb
  B: 11
} as const;
```

### Mode

音楽モード (メジャー/マイナー)。

```typescript
const Mode = {
  Major: 0,
  Minor: 1
} as const;
```

### ChordQuality

コードクオリティタイプ。

```typescript
const ChordQuality = {
  Major: 0,       // C, D, E...
  Minor: 1,       // Cm, Dm, Em...
  Diminished: 2,  // Cdim, Ddim...
  Augmented: 3,   // Caug, Daug...
  Dominant7: 4,   // C7, D7...
  Major7: 5,      // Cmaj7, Dmaj7...
  Minor7: 6,      // Cm7, Dm7...
  Sus2: 7,        // Csus2, Dsus2...
  Sus4: 8         // Csus4, Dsus4...
} as const;
```

### SectionType

楽曲セクションタイプ。

```typescript
const SectionType = {
  Intro: 0,
  Verse: 1,
  PreChorus: 2,
  Chorus: 3,
  Bridge: 4,
  Instrumental: 5,
  Outro: 6
} as const;
```

---

## エラーハンドリング

モジュールが初期化されていない場合、すべての解析関数はエラーをスローします。

```typescript
import { init, detectBpm, isInitialized } from '@libraz/sonare';

// 初期化を確認
if (!isInitialized()) {
  await init();
}

// try-catch での安全な使用
try {
  const bpm = detectBpm(samples, sampleRate);
  console.log(`BPM: ${bpm}`);
} catch (error) {
  if (error.message.includes('not initialized')) {
    console.error('モジュールが初期化されていません。先に init() を呼び出してください。');
  } else {
    console.error('解析エラー:', error);
  }
}
```

---

## 高度な使用法

### オーディオファイルの処理

```typescript
import { init, analyze } from '@libraz/sonare';

async function analyzeAudioFile(url: string): Promise<AnalysisResult> {
  await init();

  const audioCtx = new AudioContext();
  const response = await fetch(url);
  const arrayBuffer = await response.arrayBuffer();
  const audioBuffer = await audioCtx.decodeAudioData(arrayBuffer);

  // ステレオの場合モノラルに変換
  let samples: Float32Array;
  if (audioBuffer.numberOfChannels === 2) {
    const left = audioBuffer.getChannelData(0);
    const right = audioBuffer.getChannelData(1);
    samples = new Float32Array(left.length);
    for (let i = 0; i < left.length; i++) {
      samples[i] = (left[i] + right[i]) / 2;
    }
  } else {
    samples = audioBuffer.getChannelData(0);
  }

  return analyze(samples, audioBuffer.sampleRate);
}
```

### Web Worker の使用

メインスレッドをブロックしないように解析を Web Worker にオフロードします。

**worker.ts:**

```typescript
import { init, analyze, AnalysisResult } from '@libraz/sonare';

let initialized = false;

self.onmessage = async (e: MessageEvent) => {
  const { samples, sampleRate } = e.data;

  if (!initialized) {
    await init();
    initialized = true;
  }

  try {
    const result = analyze(samples, sampleRate);
    self.postMessage({ success: true, result });
  } catch (error) {
    self.postMessage({ success: false, error: error.message });
  }
};
```

**main.ts:**

```typescript
const worker = new Worker(new URL('./worker.ts', import.meta.url), {
  type: 'module'
});

function analyzeInWorker(samples: Float32Array, sampleRate: number): Promise<AnalysisResult> {
  return new Promise((resolve, reject) => {
    worker.onmessage = (e) => {
      if (e.data.success) {
        resolve(e.data.result);
      } else {
        reject(new Error(e.data.error));
      }
    };

    worker.postMessage({ samples, sampleRate });
  });
}
```

### 高速化のためのダウンサンプリング

BPM 検出では、22050 Hz へのダウンサンプリングで十分であり、高速です。

```typescript
function downsample(samples: Float32Array, srcRate: number, dstRate: number): Float32Array {
  if (srcRate === dstRate) return samples;

  const ratio = srcRate / dstRate;
  const newLength = Math.floor(samples.length / ratio);
  const result = new Float32Array(newLength);

  for (let i = 0; i < newLength; i++) {
    result[i] = samples[Math.floor(i * ratio)];
  }

  return result;
}

// 高速 BPM 検出
const downsampled = downsample(samples, 48000, 22050);
const bpm = detectBpm(downsampled, 22050);
```

---

## バンドルサイズ

| ファイル | サイズ | Gzip |
|---------|--------|------|
| `sonare.js` | ~34 KB | ~12 KB |
| `sonare.wasm` | ~228 KB | ~80 KB |
| **合計** | ~262 KB | ~92 KB |

---

## ブラウザサポート

| ブラウザ | 最小バージョン |
|---------|---------------|
| Chrome | 57+ |
| Firefox | 52+ |
| Safari | 11+ |
| Edge | 16+ |

要件:
- WebAssembly サポート
- ES2017+ (async/await)
- Web Audio API (オーディオデコード用)
