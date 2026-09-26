/* The take list's own words, in Japanese.
 *
 * A take's label, its one-line note and its group heading are written in
 * `phrases.py` and BAKED INTO EVERY MANIFEST at render time, so they arrived on
 * the page as English however the page was set — the one column of the listening
 * surface that the language toggle did not reach, and the column that says what
 * the phrase is for.
 *
 * Keyed by the English string rather than by the take's id, because an id does
 * not identify the words: `single-mid` is "held four seconds" on a struck voice,
 * "plucked, let ring" on a plucked one and "let ring" on a bell. The id is the
 * address; the sentence is the thing being translated.
 *
 * Resolved here rather than written into the manifests, so the hundred and
 * eighty-odd pages already rendered answer in Japanese without one of them being
 * rendered again — the same reason the slot and its provenance are resolved per
 * request in `serve.py`.
 *
 * A string whose Japanese is the same as its English (a velocity list, a row of
 * note names) still carries an entry. The rule the test holds is that every
 * string the harness can produce is IN the table, and an entry that happens to
 * be identical is how that rule stays total rather than needing a list of
 * exceptions.
 */

'use strict';

import { currentLang } from './i18n.js';

/// Group headings — the question a run of takes is asking.
const GROUPS = {
  'one note at a time': '1音ずつ',
  'one at a time': '1発ずつ',
  'notes together': '同時に鳴らす',
  'between notes': '音と音のあいだ',
  'at speed': '速く動かす',
  'the mechanism': '機構の音',
  'the pedal': 'ペダル',
  'the mute group': 'ミュートグループ',
  'the kit as a kit': 'キット全体',
  'the long tail': '長い余韻',
  'a phrase': 'フレーズ',
  'a real line': '実際の音楽',
};

/* Take labels. The harpsichord set writes its dashes as hyphens and every other
 * set as em dashes, and both spellings are in manifests already rendered, so
 * both are keys. */
const LABELS = {
  'Single note — C4, mf': '単音 — C4、mf',
  'Single note - C4, mf': '単音 — C4、mf',
  'Single note — held four seconds': '単音 — 4秒保持',
  'Single note — held six seconds': '単音 — 6秒保持',
  'Single note — plucked, let ring': '単音 — 弾いて、鳴らしたまま',
  'Single strike — let ring': '単打 — 鳴りきるまで',
  'One trigger — heard whole': '1回の発音 — 最後まで聴く',
  'Dynamics — C4 at five velocities': '強弱 — C4 をベロシティ5段階で',
  'Dynamics - C4 at five velocities': '強弱 — C4 をベロシティ5段階で',
  'Dynamics — five velocities': '強弱 — ベロシティ5段階',
  'Register — A0 to C8': '音域 — A0 から C8 まで',
  'Register - the whole compass': '音域 — 全音域',
  'Register — across the compass': '音域 — 全音域を渡る',
  'Treble - the top note held four seconds': '高音 — 最高音を4秒保持',
  'Swell — one note from a whisper and back': 'スウェル — 1音を最弱から最強、また戻す',
  'Chord — C major triad held': '和音 — ハ長調の三和音を保持',
  'Chord - C major triad held': '和音 — ハ長調の三和音を保持',
  'Chord — a major triad held': '和音 — 長三和音を保持',
  'Chord — strummed, not struck': '和音 — 同時ではなくストロークで',
  'Arpeggio — overlapping': 'アルペジオ — 重ねて',
  'Arpeggio — overlapping, no pedal': 'アルペジオ — 重ねて、ペダルなし',
  'Arpeggio — overlapping, all ringing': 'アルペジオ — 重ねて、すべて鳴らしたまま',
  'Arpeggio - overlapping, held past each other':
    'アルペジオ — 次の音が出ても前の音を保持',
  'Two bars together — a third': '音板2枚 — 3度で同時に',
  'Legato — a scale, slurred': 'レガート — 音階をスラーで',
  'Repeated notes — separated': '同音連打 — 1音ずつ切って',
  'Leaps — across the compass': '跳躍 — 音域をまたいで',
  'Repeated note — eight strikes on a ringing string': '連打 — 鳴っている弦を8回打つ',
  'Repeated note — re-plucked while ringing': '連打 — 鳴っている弦を弾き直す',
  'Roll — sixteen strokes on one bar': 'ロール — 同じ音板を16打',
  'Retriggered — before it has finished': '再発音 — 鳴り終わる前に',
  'Trill - C4 to D4, sixteen notes': 'トリル — C4–D4 を16音',
  'Staccato - eight short notes': 'スタッカート — 短い音を8つ',
  'Staccato — damped strokes': 'スタッカート — 手で止める奏法',
  'Pedal — staccato notes under a held pedal': 'ペダル — 踏んだまま短い音を並べる',
  'Hi-hat — open, choked, open, pedal':
    'ハイハット — オープン、チョーク、オープン、ペダル',
  'Hi-hat — sixteenths at 120': 'ハイハット — 120 で16分',
  'Snare — flams and a roll': 'スネア — フラムとロール',
  'Toms — a descending fill': 'タム — 下降フィル',
  'Hand percussion — shakers, scrapers, cuica, vibraslap':
    'ハンドパーカッション — シェイカー、ギロ、クイーカ、ビブラスラップ',
  'Cymbals — the plates, let ring': 'シンバル — クラッシュ系を鳴らしきる',
  'Cymbals — rides, bell and splash': 'シンバル — ライド、ベル、スプラッシュ',
  'Groove — a bar of eights': 'グルーヴ — 8分音符1小節',
  'Phrase — melody over a bass line': 'フレーズ — 低音の上に旋律',
  'Phrase — melody over a bass line, pedalled': 'フレーズ — 低音の上に旋律、ペダルあり',
  'Phrase — a line over open strings': 'フレーズ — 開放弦の上を動く旋律',
  'Phrase — a line with the bars left ringing': 'フレーズ — 音板を鳴らしたままの旋律',
  'Phrase - two voices': 'フレーズ — 2声',
  // The musical takes. A work's Japanese title is the one it is catalogued
  // under here, not a translation of the English wording.
  'WTC I — Prelude in C, opening': '平均律クラヴィーア曲集 第1巻 — ハ長調 前奏曲、冒頭',
  'WTC I — Fugue in C minor, subject and answer':
    '平均律クラヴィーア曲集 第1巻 — ハ短調 フーガ、主題と応答',
  'Lute Suite BWV 996 — Praeludium, opening': 'リュート組曲 BWV 996 — 前奏曲、冒頭',
  'Cello Suite No. 1 — Prelude, opening': '無伴奏チェロ組曲 第1番 — 前奏曲、冒頭',
  'Ich ruf zu dir, BWV 639 — opening':
    '《われ汝に呼ばわる、主イエス・キリストよ》BWV 639 — 冒頭',
};

/// The line under a label: what the take is actually made of.
const SUBS = {
  'attack, free decay, damper': 'アタック、自由減衰、ダンパー',
  'pluck, free decay, damper on release': '弾弦、自由減衰、離鍵でダンプ',
  'strike, free decay, damper on release': '打弦、自由減衰、離鍵でダンプ',
  'the pluck, then nothing feeding it': '弾いた瞬間だけ、あとは何も与えない',
  'the whole ring, well past where any metric stops looking':
    '計測が見るのをやめたずっと先まで、鳴りきるまで',
  'the vibrato, if there is one, and how it starts':
    'ビブラートの有無と、その入り方',
  'held two seconds, then left to finish': '2秒保持し、そのまま鳴り終わらせる',
  'the top of the compass has to keep sounding': '最高音が鳴り続けられるか',
  'vel 16 / 40 / 72 / 100 / 127': 'vel 16 / 40 / 72 / 100 / 127',
  'vel 16 / 40 / 72 / 100 / 127, and they should barely differ':
    'vel 16 / 40 / 72 / 100 / 127 — ほとんど差が出ないはず',
  'A0 C2 C4 C6 C8, vel 96': 'A0 C2 C4 C6 C8、vel 96',
  'F1 C2 C3 C4 C5 C6 F6, vel 96': 'F1 C2 C3 C4 C5 C6 F6、vel 96',
  'low, low-mid, mid, high-mid, high, vel 96': '低・中低・中・中高・高、vel 96',
  'low, low-mid, mid, high-mid, high, vel 100': '低・中低・中・中高・高、vel 100',
  'expression from 16 to 127 and back over 6 s':
    'エクスプレッションを 16→127→16 と 6 秒で動かす',
  'C3 E3 G3, vel 88, 5 s': 'C3 E3 G3、vel 88、5 秒',
  'C3 E3 G3, vel 88, 6 s': 'C3 E3 G3、vel 88、6 秒',
  'root, third, fifth at vel 88, 6 s': '根音・3度・5度を vel 88、6 秒',
  'five strings 25 ms apart, let ring': '5弦を 25 ms ずつずらして、鳴らしたまま',
  'struck at once, let ring': '同時に打って、鳴らしたまま',
  'C3 E3 G3 C4 E4 G4 C5, each held past the next':
    'C3 E3 G3 C4 E4 G4 C5 — それぞれ次の音より長く保持',
  'an octave and a half, each note held past the next':
    '1オクターブ半 — それぞれ次の音より長く保持',
  'an octave and a half, nothing damped': '1オクターブ半 — 一切ダンプしない',
  'overlapping note-ons, no gap anywhere':
    'ノートオンを重ね、どこにも切れ目を作らない',
  'the same pitch eight times, each articulated':
    '同じ音を8回、1音ずつ発音し直す',
  'low, high, low, slurred': '低・高・低をスラーで',
  'C4 vel 100, 190 ms apart': 'C4 vel 100、190 ms 間隔',
  'vel 100, 190 ms apart': 'vel 100、190 ms 間隔',
  'vel 100, 160 ms apart, eight times': 'vel 100、160 ms 間隔で8回',
  'six triggers 400 ms apart': '400 ms 間隔で6回',
  '80 ms apart, alternating accents': '80 ms 間隔、アクセントを交互に',
  'each key released before the next, 90 ms apart':
    '次を押す前に離鍵、90 ms 間隔',
  'eight short notes, each stopped by the release':
    '短い音を8つ、いずれも離鍵で止める',
  'the jack and the damper are the sound between the notes':
    '音と音のあいだに鳴るのはジャックとダンパー',
  'CC64 down at 0.2 s, up at 7.0 s': 'CC64 を 0.2 s で踏み、7.0 s で離す',
  '46 open / 42 closed / 46 open / 44 pedal':
    '46 オープン / 42 クローズ / 46 オープン / 44 ペダル',
  '42 closed, accented on the beat': '42 クローズ、拍にアクセント',
  'grace note 28 ms ahead, then a 12-stroke roll':
    '28 ms 前に装飾音、続けて12打のロール',
  "43 41 50 48 47 45, the capture's measured pitch order, high to low":
    '43 41 50 48 47 45 — キャプチャで実測した音高順、高い方から',
  '69 cabasa, 70 maracas, 73/74 guiro, 78/79 cuica, 58 vibraslap':
    '69 カバサ、70 マラカス、73/74 ギロ、78/79 クイーカ、58 ビブラスラップ',
  '49 crash, 57 crash 2, 52 china, ten seconds':
    '49 クラッシュ、57 クラッシュ2、52 チャイナ、10 秒',
  '51 ride, 59 ride 2, 53 bell, 55 splash':
    '51 ライド、59 ライド2、53 ベル、55 スプラッシュ',
  'kick, snare, closed hat, 100 bpm':
    'キック、スネア、クローズドハット、100 bpm',
  'two hands, nothing held by a pedal': '両手、ペダルでは何も保持しない',
  'with the pedal changed on each bass note': '低音が変わるたびにペダルを踏み替える',
  'a right hand over a walking bass, no pedal anywhere':
    '歩く低音の上に右手、ペダルは一切なし',
  'a melody with the low notes left ringing under it':
    '低音を鳴らしたまま、その上に旋律',
  'nothing damped, so every note is heard against the last four':
    '一切ダンプしないので、どの音も直前4音の上で鳴る',
  // The musical takes' notes, which say what the passage is there to catch.
  'broken chords held under each other: the bloom of overlapping decays, and whether the next entry has room':
    '分散和音を重ねて保持 — 減衰が重なって膨らむ様子と、次の入りに場所が残るか',
  'two entries of one subject in different registers: whether the ring of the first is still audible under the second':
    '同じ主題を別の音域で2回 — 1回目の響きが2回目の下でまだ聴こえるか',
  'a plucked line against a held bass: how long a pluck lasts under the next one, and whether the bass survives it':
    '保持した低音に対する弾弦の旋律 — 弾いた音が次の音の下でどれだけ保つか、低音が生き残るか',
  'one line across a wide compass, mostly stepwise: where a bowed or blown voice changes character as it climbs':
    '広い音域を主に順次進行で1本の線 — 擦弦・吹奏の音色が上行のどこで変わるか',
  'a sustained line over a moving inner voice and a pedal: register balance across three parts that never stop':
    '動く内声とペダル音の上に保持された線 — 途切れない3声の音域バランス',
};

/* A musical take is transposed by whole octaves into the program's compass and
 * says so at the end of its own note, so the suffix is taken off, the sentence
 * translated, and the suffix put back in words. */
const OCTAVE = /^(.*) \(([-+]\d+) octaves?\)$/;

function withOctave(text) {
  const m = text.match(OCTAVE);
  if (!m) return null;
  const base = SUBS[m[1]];
  if (base === undefined) return null;
  const n = Number(m[2]);
  return `${base}（${Math.abs(n)}オクターブ${n > 0 ? '上' : '下'}）`;
}

/* The Japanese for one of the strings a manifest carries, or the string itself.
 *
 * Falling through to the English is the whole behaviour for `en`, and it is also
 * what a take added to `phrases.py` without a translation gets: an English line
 * in a Japanese list is legible, and the test says so before it ships. */
export function takeText(text) {
  if (!text || currentLang() !== 'ja') return text || '';
  const hit = GROUPS[text] ?? LABELS[text] ?? SUBS[text] ?? withOctave(text);
  return hit ?? text;
}
