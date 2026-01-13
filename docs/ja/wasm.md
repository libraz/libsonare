# WebAssembly ガイド

## 概要

libsonare は WebAssembly にコンパイルでき、サーバーサイド処理なしで Web ブラウザ内で直接オーディオ解析を行えます。

## インストール

### npm/yarn

```bash
npm install @libraz/sonare
# または
yarn add @libraz/sonare
```

### CDN

```html
<script type="module">
  import createSonare from 'https://unpkg.com/@libraz/sonare/dist/sonare.js';
</script>
```

## 基本的な使い方

### ES Modules

```typescript
import { Sonare } from '@libraz/sonare';

async function analyzeAudio() {
  const sonare = await Sonare.create();

  // AudioContext からオーディオデータを取得
  const audioCtx = new AudioContext();
  const response = await fetch('music.mp3');
  const arrayBuffer = await response.arrayBuffer();
  const audioBuffer = await audioCtx.decodeAudioData(arrayBuffer);

  // モノラルサンプルを取得
  const samples = audioBuffer.getChannelData(0);
  const sampleRate = audioBuffer.sampleRate;

  // BPM 検出
  const bpm = sonare.detectBpm(samples, sampleRate);
  console.log(`BPM: ${bpm}`);

  // キー検出
  const key = sonare.detectKey(samples, sampleRate);
  console.log(`キー: ${key.root} ${key.mode}`);
}
```

### CommonJS

```javascript
const { Sonare } = require('@libraz/sonare');

async function main() {
  const sonare = await Sonare.create();
  // ... 上記と同じ
}
```

## TypeScript 型

```typescript
interface Key {
  root: PitchClass;   // 0-11 (C=0, B=11)
  mode: Mode;         // 0=Major, 1=Minor
  confidence: number; // 0.0-1.0
}

enum PitchClass {
  C = 0, Cs = 1, D = 2, Ds = 3, E = 4, F = 5,
  Fs = 6, G = 7, Gs = 8, A = 9, As = 10, B = 11
}

enum Mode {
  Major = 0,
  Minor = 1
}

interface AnalysisResult {
  bpm: number;
  key: Key;
  timeSignature: TimeSignature;
  beats: Beat[];
  chords: Chord[];
  sections: Section[];
}
```

## API リファレンス

### Sonare クラス

```typescript
class Sonare {
  // 初期化
  static async create(): Promise<Sonare>;

  // BPM 検出
  detectBpm(samples: Float32Array, sampleRate: number): number;

  // キー検出
  detectKey(samples: Float32Array, sampleRate: number): Key;

  // ビート検出
  detectBeats(samples: Float32Array, sampleRate: number): number[];

  // オンセット検出
  detectOnsets(samples: Float32Array, sampleRate: number): number[];

  // フル解析
  analyze(samples: Float32Array, sampleRate: number): AnalysisResult;

  // バージョン情報
  version(): string;
}
```

## オーディオファイルの操作

### ファイル入力

```typescript
async function analyzeFile(file: File) {
  const sonare = await Sonare.create();
  const audioCtx = new AudioContext();

  const arrayBuffer = await file.arrayBuffer();
  const audioBuffer = await audioCtx.decodeAudioData(arrayBuffer);
  const samples = audioBuffer.getChannelData(0);

  const bpm = sonare.detectBpm(samples, audioBuffer.sampleRate);
  return bpm;
}

// ファイル入力での使用
const input = document.querySelector('input[type="file"]');
input.addEventListener('change', async (e) => {
  const file = e.target.files[0];
  const bpm = await analyzeFile(file);
  console.log(`BPM: ${bpm}`);
});
```

### マイク入力

```typescript
async function analyzeFromMicrophone() {
  const sonare = await Sonare.create();
  const audioCtx = new AudioContext();

  const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
  const source = audioCtx.createMediaStreamSource(stream);
  const analyser = audioCtx.createScriptProcessor(4096, 1, 1);

  const samples: number[] = [];

  analyser.onaudioprocess = (e) => {
    const input = e.inputBuffer.getChannelData(0);
    samples.push(...input);

    // 5秒ごとにオーディオを解析
    if (samples.length >= audioCtx.sampleRate * 5) {
      const float32 = new Float32Array(samples);
      const bpm = sonare.detectBpm(float32, audioCtx.sampleRate);
      console.log(`現在の BPM: ${bpm}`);
      samples.length = 0; // バッファをクリア
    }
  };

  source.connect(analyser);
  analyser.connect(audioCtx.destination);
}
```

## パフォーマンスのヒント

### より高速な解析のためのリサンプリング

BPM 検出では、より高速な処理のために 22050Hz にダウンサンプリングできます:

```typescript
async function fastBpmDetection(samples: Float32Array, sampleRate: number) {
  const sonare = await Sonare.create();

  // 必要に応じて 22050Hz にダウンサンプリング
  let processedSamples = samples;
  let processedSampleRate = sampleRate;

  if (sampleRate > 22050) {
    // 簡易ダウンサンプリング (本番では適切なリサンプリングを使用)
    const ratio = sampleRate / 22050;
    const newLength = Math.floor(samples.length / ratio);
    processedSamples = new Float32Array(newLength);
    for (let i = 0; i < newLength; i++) {
      processedSamples[i] = samples[Math.floor(i * ratio)];
    }
    processedSampleRate = 22050;
  }

  return sonare.detectBpm(processedSamples, processedSampleRate);
}
```

### Web Workers

重い解析には、メインスレッドをブロックしないよう Web Workers を使用:

```typescript
// worker.ts
import { Sonare } from '@libraz/sonare';

let sonare: Sonare;

self.onmessage = async (e) => {
  if (!sonare) {
    sonare = await Sonare.create();
  }

  const { samples, sampleRate, action } = e.data;

  switch (action) {
    case 'bpm':
      const bpm = sonare.detectBpm(samples, sampleRate);
      self.postMessage({ type: 'bpm', result: bpm });
      break;
    case 'analyze':
      const result = sonare.analyze(samples, sampleRate);
      self.postMessage({ type: 'analyze', result });
      break;
  }
};
```

```typescript
// main.ts
const worker = new Worker(new URL('./worker.ts', import.meta.url));

worker.onmessage = (e) => {
  console.log('結果:', e.data);
};

// 解析用にオーディオを送信
worker.postMessage({
  action: 'analyze',
  samples: audioBuffer.getChannelData(0),
  sampleRate: audioBuffer.sampleRate
});
```

## メモリ管理

### 自動クリーンアップ

Sonare ラッパーは自動的にメモリを管理します。ただし、大きなオーディオファイルの場合、オーディオデータを明示的に管理したい場合があります:

```typescript
async function analyzeWithCleanup(url: string) {
  const sonare = await Sonare.create();
  const audioCtx = new AudioContext();

  const response = await fetch(url);
  const arrayBuffer = await response.arrayBuffer();
  const audioBuffer = await audioCtx.decodeAudioData(arrayBuffer);

  // サンプルを取得 (コピーが作成される)
  const samples = audioBuffer.getChannelData(0);

  // 解析
  const result = sonare.analyze(samples, audioBuffer.sampleRate);

  // AudioBuffer はガベージコレクションされる
  // Float32Array (samples) もガベージコレクションされる

  return result;
}
```

## ブラウザ互換性

| ブラウザ | サポート |
|---------|---------|
| Chrome | 57+ |
| Firefox | 52+ |
| Safari | 11+ |
| Edge | 16+ |

要件:
- WebAssembly サポート
- Web Audio API
- ES2017+ (async/await)

## バンドルサイズ

- `sonare.js`: ~34KB (gzip: ~12KB)
- `sonare.wasm`: ~228KB (gzip: ~80KB)

合計: ~262KB (~92KB gzip 圧縮時)

## トラブルシューティング

### "Out of memory" エラー

非常に長いオーディオファイルの場合、WASM メモリを増やします:

```javascript
// 内部で処理されますが、メモリ使用量を監視できます
console.log('メモリ使用量:', performance.memory?.usedJSHeapSize);
```

### AudioContext が許可されない

モダンブラウザでは AudioContext 作成前にユーザー操作が必要です:

```typescript
document.addEventListener('click', async () => {
  const audioCtx = new AudioContext();
  await audioCtx.resume();
  // これで audioCtx を使用できます
});
```

### クロスオリジンの問題

他のドメインからオーディオを読み込む場合、CORS ヘッダーが設定されていることを確認:

```typescript
const response = await fetch(url, {
  mode: 'cors',
  credentials: 'omit'
});
```
