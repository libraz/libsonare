/* Every string the page shows, in both languages, and the triage tree.
 *
 * The page is read by whoever is listening, and that is not always the person
 * who wrote the harness. So nothing user-facing is built from a source key or
 * an engine name alone: those stay as identifiers beside a sentence.
 *
 * Two languages, one table. A key missing from `ja` falls through to `en`
 * rather than rendering as its own key — a half-translated page is readable and
 * a page full of `feedback.send` is not.
 */

'use strict';

const STRINGS = {
  en: {
    'app.title': 'audition',

    'view.bank': 'bank',
    'view.listen': 'listen',
    'view.aria': 'view',

    'head.voice': 'voice',
    'head.lang': 'language',
    'head.options': 'options',
    'head.help': 'keys',

    'opt.playback': 'playback',
    'opt.matchLoudness': 'match the loudness of the versions',
    'opt.restart': 'restart from the top when I switch',
    'opt.blind': 'hide which version is which',
    'opt.about': 'about this set',
    'opt.close': 'close',

    'keys.title': 'keyboard',
    'keys.playPause': 'play / pause',
    'keys.take': 'previous / next take',
    'keys.version': 'pick a version',
    'keys.swap': 'model ⇄ reference',
    'keys.loop': 'loop',
    'keys.match': 'match loudness',
    'keys.restart': 'restart on switch',
    'keys.blind': 'blind',
    'keys.reveal': 'reveal / reshuffle',
    'keys.drag': 'Drag across the waveform to loop a region; a click seeks.',
    'keys.bankMove': 'move through the rows',
    'keys.bankEnds': 'first / last',
    'keys.bankOpen': 'listen to this voice',
    'keys.bankFind': 'find',

    'role.model': 'libsonare',
    'role.reference': 'reference',
    'role.other': 'versions',
    'role.model.long': 'libsonare — what the library produces',
    'role.reference.long': 'reference — what it is being compared against',

    'subj.slot': 'slot',
    'subj.gm': 'GM {n}',
    'subj.program': 'program {n}',
    'subj.bank': 'bank {n}',
    'subj.kit': 'kit {n}',
    'subj.channel10': 'channel 10',
    'subj.patch': 'patch {name}',
    'subj.noPatch': 'patch not reported',
    'subj.struck': 'struck',
    'subj.reference': 'reference',
    'subj.target': 'target: {what}',
    'subj.none': 'none captured yet',
    'subj.declined': 'none — {names} carries {carries} at this address',
    'subj.declinedBare': 'none, by policy',
    'src.module': 'the module itself',
    'src.dedicated': 'a dedicated instrument',
    'src.library': 'a sample library',
    'src.unclassified': 'kind not classified',
    'want.instrument': 'a modern recording',
    'want.machine': 'the module',
    'room.none': 'no room in it',
    'room.present': 'carries a room',
    'room.dry': 'effects switched off',
    'room.undeclared': 'room not declared',
    'prov.offTarget': 'not what this slot is aimed at',
    'prov.unclassified': 'the capture does not say what answered it',

    'ver.hint': 'Versions of the same take, to be chosen between — the line above says what the selected one is meant to sound like.',
    'ver.hintBlind': 'Names hidden. Whichever version you leave a take on is your vote for that take.',

    'now.hidden': 'hidden',
    'now.copy': 'copy what I hear',
    'now.copied': 'copied',
    'now.shown': 'shown above',
    'now.copyTitle':
      'Set, take, version, playhead, which strike it is in, and the link that reproduces it',
    'now.swap': 'compare with the reference',
    'now.swapBack': 'back to libsonare',

    'transport.play': 'play',
    'transport.pause': 'pause',
    'transport.loop': 'loop',
    'transport.clearRegion': 'whole take',

    'wave.caption': 'waveform — every version drawn in its own colour, the one sounding solid',
    'wave.captionSolo': 'waveform',
    'spec.caption': 'spectrogram of the version sounding — log frequency',
    'spec.decoding': 'decoding…',
    'spec.failed': 'could not load: {msg}',

    'takes.title': 'takes',
    'takes.hasReference': 'has a reference',

    'level.rms': 'rms {db} dBFS',
    'level.gain': 'gain {db} dB applied',

    'blind.pickEach': 'pick a version on each take',
    'blind.preferred': 'preferred',
    'blind.record': 'record this result',
    'blind.decided': 'picked on {n} take(s)',
    'blind.result': 'blind listening, {n} take(s): {tally}',

    'fb.title': 'tell me what you hear',
    'fb.intro': 'Two choices at a time, and “not sure” is an answer. Send at any point.',
    'fb.notSure': 'not sure',
    'fb.back': 'back',
    'fb.restart': 'start over',
    'fb.comment': 'anything else, in your own words',
    'fb.commentPlaceholder': 'optional — what you actually heard',
    'fb.attach': 'attach what is sounding right now',
    'fb.attachWhat': 'take, version, playhead, which strike, and the options in force',
    'fb.prefer': 'keep this one',
    'fb.preferTitle': 'Record this version as the one to keep, with what is sounding and whatever is in the box below',
    'fb.send': 'send',
    'fb.sending': 'sending…',
    'fb.sent': 'sent',
    'fb.failed': 'could not send: {msg}',
    'fb.undo': 'undo the last one',
    'fb.undone': 'removed',
    'fb.recent': 'what has been said about this voice',
    'fb.none': 'Nothing yet.',
    'fb.needSomething': 'Answer something or write a line first.',
    'fb.where': 'Saved to {path}',

    'grade.ok': 'fine',
    'grade.acceptable': 'liveable',
    'grade.wrongInstrument': 'wrong instrument',
    'grade.broken': 'barely sounds',
    'grade.off': 'something off',
    'grade.unsure': 'not sure',

    'bank.voices': 'voices',
    'bank.withOracle': 'with a reference',
    'bank.unwritten': 'unwritten settings',
    'bank.rendered': 'rendered',
    'bank.method': 'synthesis method',
    'bank.any': 'any',
    'bank.physical': 'physical model',
    'bank.fm': 'FM',
    'bank.classic': 'subtractive / additive',
    'bank.filter': 'filter',
    'bank.fHasOracle': 'has a reference',
    'bank.fNeedsOracle': 'needs one',
    'bank.fUnwritten': 'unwritten',
    'bank.fRendered': 'rendered',
    'bank.find': 'find',
    'bank.findPlaceholder': 'name, engine, patch  ( / )',
    'bank.sort': 'sort',
    'bank.sortAddress': 'address',
    'bank.sortStage': 'stage, highest first',
    'bank.sortName': 'name',
    'bank.colProg': 'prog',
    'bank.colVoice': 'voice',
    'bank.colEngine': 'engine',
    'bank.colStage': 'stage',
    'bank.colOracle': 'reference',
    'bank.colNext': 'next',
    'bank.noOracle': 'none',
    'bank.notReported': 'not reported',
    'bank.nUnwritten': '{n} unwritten',
    'bank.nothingMatches': 'Nothing matches.',
    'bank.notGenerated':
      'No bank view generated. Run `make voice-status-refresh` (it needs a -DBUILD_TUNING=ON build) and reload.',
    'bank.ladder': 'the ladder',
    'bank.methodLegend': 'method',
    'bank.selectOne':
      'Select a voice for its coverage and where it sits against the references’ own spread.',
    'bank.measurement': 'measurement',
    'bank.oracle': 'reference',
    'bank.notCaptured': 'not captured',
    'bank.coverage': 'coverage',
    'bank.coverageOf': '{gated} of {canonical} gated',
    'bank.excused': '{n} excused: {list}',
    'bank.gaps': 'gaps',
    'bank.agreement': 'agreement',
    'bank.agreementOf': '{inside} of {total} inside the spread',
    'bank.noSpread': 'no spread',
    'bank.unjudgeable': '{n} dimension(s) unjudgeable — one reference timbre',
    'bank.next': 'next',
    'bank.listen': 'listen →',
    'bank.timbres': '{timbres} timbre(s), {rows} rows, gate {gate}',
    'bank.gateNotRecorded': 'not recorded',
    'bank.stageOf': 'stage {n} — {name}',

    'stage.0': 'untouched',
    'stage.1': 'voiced',
    'stage.2': 'measured',
    'stage.3': 'covered',
    'stage.4': 'agreeing',
    'stage.5': 'settled',
    'stage.0.of': 'no deliberate patch: a famN family fallback on subtractive',
    'stage.1.of': 'a deliberate engine and patch answer it',
    'stage.2.of': 'two or more reference timbres, a profile, a current gate',
    'stage.3.of': 'every canonical dimension gated, or excused with a reason',
    'stage.4.of': 'most gated dimensions sit inside the reference spread',
    'stage.5.of': 'no structural residual, and the musical take signed off',

    'empty.noRenders':
      'No renders found. Generate a set with tools/voicematch/make_audition.py — --model-only needs no plugin — then reload.',
    'empty.loadFailed': 'could not load the renders: {msg}',
  },

  ja: {
    'app.title': '試聴',

    'view.bank': '一覧',
    'view.listen': '試聴',
    'view.aria': '表示',

    'head.voice': '音色',
    'head.lang': '言語',
    'head.options': '設定',
    'head.help': 'キー',

    'opt.playback': '再生',
    'opt.matchLoudness': 'バージョン間の音量を揃える',
    'opt.restart': '切り替えたら頭から鳴らし直す',
    'opt.blind': 'どれがどれか隠す（ブラインド）',
    'opt.about': 'このページについて',
    'opt.close': '閉じる',

    'keys.title': 'キーボード',
    'keys.playPause': '再生 / 一時停止',
    'keys.take': '前 / 次のテイク',
    'keys.version': 'バージョンを選ぶ',
    'keys.swap': 'libsonare ⇄ リファレンス',
    'keys.loop': 'ループ',
    'keys.match': '音量を揃える',
    'keys.restart': '切替時に頭出し',
    'keys.blind': 'ブラインド',
    'keys.reveal': '正体を見る / 並べ直す',
    'keys.drag': '波形をドラッグするとその範囲をループします。クリックで頭出し。',
    'keys.bankMove': '行を移動',
    'keys.bankEnds': '先頭 / 末尾',
    'keys.bankOpen': 'この音色を試聴',
    'keys.bankFind': '検索',

    'role.model': 'libsonare',
    'role.reference': 'リファレンス',
    'role.other': 'バージョン',
    'role.model.long': 'libsonare — このライブラリが出している音',
    'role.reference.long': 'リファレンス — 目標にしている音',

    'subj.slot': 'スロット',
    'subj.gm': 'GM {n}',
    'subj.program': 'プログラム {n}',
    'subj.bank': 'バンク {n}',
    'subj.kit': 'キット {n}',
    'subj.channel10': 'ch 10',
    'subj.patch': 'パッチ {name}',
    'subj.noPatch': 'パッチ未報告',
    'subj.struck': '打っているもの',
    'subj.reference': 'リファレンス',
    'subj.target': '目標: {what}',
    'subj.none': 'まだ採取されていません',
    'subj.declined': 'なし — この番地の {names} には {carries} が入っている',
    'subj.declinedBare': '方針によりなし',
    'src.module': 'モジュール実機',
    'src.dedicated': '専用音源',
    'src.library': 'サンプルライブラリ',
    'src.unclassified': '種別が未分類',
    'want.instrument': '実楽器の録音',
    'want.machine': 'モジュール実機',
    'room.none': '残響なし',
    'room.present': '残響を含む',
    'room.dry': 'エフェクト全停止',
    'room.undeclared': '残響は未申告',
    'prov.offTarget': 'このスロットが目指している音源ではない',
    'prov.unclassified': 'キャプチャに音源の種別が書かれていない',

    'ver.hint': '同じテイクの別バージョンです。聴き比べて良いものを選んでください — 選んだものが何を狙った設定かは上の行が説明します。',
    'ver.hintBlind': '名前を伏せています。各テイクで最後に選んだものが、そのテイクの一票になります。',

    'now.hidden': '伏せ中',
    'now.copy': 'いま聴いている条件をコピー',
    'now.copied': 'コピーしました',
    'now.shown': '上に表示しました',
    'now.copyTitle':
      '音色・テイク・バージョン・再生位置・何打目か、そして同じ状態を開くリンク',
    'now.swap': 'リファレンスと聴き比べる',
    'now.swapBack': 'libsonare に戻る',

    'transport.play': '再生',
    'transport.pause': '一時停止',
    'transport.loop': 'ループ',
    'transport.clearRegion': '全体に戻す',

    'wave.caption': '波形 — バージョンごとに色分け、鳴っているものが濃い線',
    'wave.captionSolo': '波形',
    'spec.caption': '鳴っているバージョンのスペクトログラム（周波数は対数）',
    'spec.decoding': '読み込み中…',
    'spec.failed': '読み込めませんでした: {msg}',

    'takes.title': 'テイク',
    'takes.hasReference': 'リファレンスあり',

    'level.rms': 'RMS {db} dBFS',
    'level.gain': '{db} dB 補正',

    'blind.pickEach': 'テイクごとに好きな方を選んでください',
    'blind.preferred': '選ばれた回数',
    'blind.record': 'この結果を記録する',
    'blind.decided': '{n} テイクで選択済み',
    'blind.result': 'ブラインド試聴、{n} テイク: {tally}',

    'fb.title': '聞こえたことを教えてください',
    'fb.intro': '2択で少しずつ絞ります。「わからない」も答えです。途中で送信できます。',
    'fb.notSure': 'わからない',
    'fb.back': 'ひとつ戻る',
    'fb.restart': '最初から',
    'fb.comment': 'そのほか、ことばで',
    'fb.commentPlaceholder': '任意 — 実際に聞こえたこと',
    'fb.attach': 'いま鳴っている状態を添える',
    'fb.attachWhat': 'テイク・バージョン・再生位置・何打目か・設定',
    'fb.prefer': 'これを推す',
    'fb.preferTitle': 'いま鳴っているバージョンを「残すべきもの」として記録します。下の欄に書いたことも一緒に送ります',
    'fb.send': '送信',
    'fb.sending': '送信中…',
    'fb.sent': '送信しました',
    'fb.failed': '送信できませんでした: {msg}',
    'fb.undo': '直前の送信を取り消す',
    'fb.undone': '取り消しました',
    'fb.recent': 'この音色について送られたこと',
    'fb.none': 'まだありません。',
    'fb.needSomething': 'どれかを選ぶか、ひとこと書いてから送ってください。',
    'fb.where': '保存先 {path}',

    'grade.ok': 'これでいい',
    'grade.acceptable': '許容できる',
    'grade.wrongInstrument': '別の楽器',
    'grade.broken': 'ほぼ鳴らない',
    'grade.off': '気になる',
    'grade.unsure': 'わからない',

    'bank.voices': '音色',
    'bank.withOracle': 'リファレンスあり',
    'bank.unwritten': '未採用の設定',
    'bank.rendered': 'レンダー済み',
    'bank.method': '合成方式',
    'bank.any': 'すべて',
    'bank.physical': '物理モデル',
    'bank.fm': 'FM',
    'bank.classic': '減算 / 加算',
    'bank.filter': '絞り込み',
    'bank.fHasOracle': 'リファレンスあり',
    'bank.fNeedsOracle': 'リファレンスなし',
    'bank.fUnwritten': '未採用あり',
    'bank.fRendered': 'レンダー済み',
    'bank.find': '検索',
    'bank.findPlaceholder': '名前・エンジン・パッチ  ( / )',
    'bank.sort': '並び順',
    'bank.sortAddress': 'アドレス順',
    'bank.sortStage': '進み具合（高い順）',
    'bank.sortName': '名前順',
    'bank.colProg': '番号',
    'bank.colVoice': '音色',
    'bank.colEngine': 'エンジン',
    'bank.colStage': '進み具合',
    'bank.colOracle': 'リファレンス',
    'bank.colNext': '次の一手',
    'bank.noOracle': 'なし',
    'bank.notReported': '報告なし',
    'bank.nUnwritten': '未採用 {n} 件',
    'bank.nothingMatches': '該当なし。',
    'bank.notGenerated':
      '一覧が生成されていません。`make voice-status-refresh`（-DBUILD_TUNING=ON のビルドが要ります）を実行してから再読み込みしてください。',
    'bank.ladder': '進み方',
    'bank.methodLegend': '合成方式',
    'bank.selectOne': '音色を選ぶと、測定の範囲とリファレンス自身のばらつきとの関係が出ます。',
    'bank.measurement': '測定',
    'bank.oracle': 'リファレンス',
    'bank.notCaptured': '未取得',
    'bank.coverage': '測定範囲',
    'bank.coverageOf': '{canonical} 項目中 {gated} 項目',
    'bank.excused': '{n} 件除外: {list}',
    'bank.gaps': '未測定',
    'bank.agreement': '一致',
    'bank.agreementOf': '{total} 項目中 {inside} 項目がばらつきの内側',
    'bank.noSpread': 'ばらつきなし',
    'bank.unjudgeable': 'リファレンスが1音色のみで {n} 項目が判定不能',
    'bank.next': '次の一手',
    'bank.listen': '試聴する →',
    'bank.timbres': '音色 {timbres} 件・行 {rows}・ゲート {gate}',
    'bank.gateNotRecorded': '記録なし',
    'bank.stageOf': '進み具合 {n} — {name}',

    'stage.0': '未着手',
    'stage.1': '音付け済み',
    'stage.2': '測定済み',
    'stage.3': '範囲を満たす',
    'stage.4': 'おおむね一致',
    'stage.5': '完了',
    'stage.0.of': '意図したパッチがなく、famN 系の減算フォールバックのまま',
    'stage.1.of': '意図したエンジンとパッチが割り当てられている',
    'stage.2.of': 'リファレンス音色2件以上・プロファイル・現行ゲートがある',
    'stage.3.of': '正準項目がすべてゲート済み、または理由つきで除外されている',
    'stage.4.of': 'ゲート済み項目の大半がリファレンスのばらつきの内側にある',
    'stage.5.of': '構造的な残差がなく、音楽テイクでも承認済み',

    'empty.noRenders':
      'レンダー結果が見つかりません。tools/voicematch/make_audition.py で生成してから再読み込みしてください（--model-only ならプラグインは要りません）。',
    'empty.loadFailed': '読み込めませんでした: {msg}',
  },
};

/* The triage tree.
 *
 * Wording is what a listener would say, never what a parameter is called. The
 * second fork is "is this even the right instrument", because that is the size
 * of error the bank still has, and a question about an attack transient asks
 * the listener to translate on our behalf — a mistranslation arrives as a
 * confident, wrong, machine-readable tag.
 *
 * Each node offers exactly two substantive answers and a third that is "not
 * sure", which is a real answer and is recorded as one: a narrowing that stops
 * early still says where it stopped.
 *
 * THE SECOND NODE IS A VERDICT AND IS SEPARATE FROM THE DIAGNOSIS. "It is
 * recognisably the instrument and I would still change it" is the state most of
 * the bank is actually in, and with only "fine" and "wrong" to choose from it
 * had to be filed as one or the other — which is the difference between a voice
 * worth a release and a voice that is a defect. It comes out in `grade`, beside
 * the finer tag, so the two questions can be answered separately later.
 *
 * `broken` is a defect rather than a voicing complaint: a voice whose note dies
 * to nothing has a mechanism missing, not a constant mis-set, and nothing else
 * on this page can say so.
 *
 * The tag is what anything mechanical reads; the question text never is. Adding
 * a fork is one entry here.
 */
const TREE = {
  start: 'off',
  nodes: {
    off: {
      q: { en: 'Compared with the reference, how is it?',
        ja: 'リファレンスと比べて、どうですか？' },
      // Most voices have no captured reference, and asking a listener to
      // compare against one that is not on the page is a question they can only
      // answer wrongly. The judgement is the same either way — what a violin
      // sounds like is in the ear as readily as on the other button — so only
      // the wording moves.
      qSolo: { en: 'How does this sound to you?',
        ja: 'この音、どう聞こえますか？' },
      a: [
        { tag: 'off', to: 'grade',
          en: 'Something bothers me', ja: '気になるところがある' },
        { tag: 'ok', leaf: true,
          en: 'This is fine as it is', ja: 'これでいい' },
      ],
      unsure: 'off/unsure',
    },
    grade: {
      q: { en: 'Does it hold up as the instrument it is meant to be?',
        ja: 'その楽器として成立していますか？' },
      a: [
        { tag: 'acceptable', to: 'when',
          en: 'It is the instrument — not the reference, but I could live with it',
          ja: '楽器には聞こえる（リファレンスとは違うが許容できる）' },
        { tag: 'wrong-instrument', to: 'broken',
          en: 'No — it sounds like something else',
          ja: '別の楽器・楽器でない音に聞こえる' },
      ],
      unsure: 'off/unsure',
    },
    broken: {
      q: { en: 'Is it sounding at all?', ja: 'その音、ちゃんと鳴っていますか？' },
      a: [
        { tag: 'broken', leaf: true,
          en: 'Barely, or it dies away to nothing',
          ja: 'ほとんど鳴らない・途中で消える' },
        { tag: 'wrong-instrument', to: 'alien',
          en: 'It sounds, it is just the wrong sound', ja: '鳴ってはいる' },
      ],
      unsure: 'wrong-instrument/unsure',
    },
    alien: {
      q: { en: 'Which is it closer to?', ja: 'どちらに近いですか？' },
      a: [
        { tag: 'wrong-instrument/synthetic', leaf: true,
          en: 'Synthetic, machine-like', ja: '機械っぽい・合成っぽい' },
        { tag: 'wrong-instrument/other', leaf: true,
          en: 'An instrument, but the wrong one', ja: '楽器ではあるが別の楽器' },
      ],
      unsure: 'wrong-instrument/unsure',
    },
    when: {
      q: { en: 'What draws your attention — the moment it starts, or while it sounds?',
        ja: '気になるのは、音が出た瞬間ですか、鳴っているあいだですか？' },
      a: [
        { tag: 'onset', to: 'onset',
          en: 'The moment it starts', ja: '出た瞬間' },
        { tag: 'body', to: 'body',
          en: 'While it sounds', ja: '鳴っているあいだ' },
      ],
      unsure: 'acceptable/unsure',
    },
    onset: {
      q: { en: 'How does the start feel?', ja: '出だしはどう感じますか？' },
      a: [
        { tag: 'onset/hard', leaf: true,
          en: 'Too hard, too sudden', ja: 'きつい・硬い' },
        { tag: 'onset/soft', leaf: true,
          en: 'Too soft, slow to speak', ja: 'もたつく・ぼやける' },
      ],
      unsure: 'onset/unsure',
    },
    body: {
      q: { en: 'Is it how the note dies away, or its colour?',
        ja: '気になるのは、音の消え方ですか、音の色あいですか？' },
      a: [
        { tag: 'tail', to: 'tail', en: 'How it dies away', ja: '消え方' },
        { tag: 'tone', to: 'tone', en: 'Its colour', ja: '色あい' },
      ],
      unsure: 'body/unsure',
    },
    tail: {
      q: { en: 'How does it die away?', ja: 'どんな消え方ですか？' },
      a: [
        { tag: 'tail/long', leaf: true,
          en: 'Hangs on too long', ja: '長く残りすぎる' },
        { tag: 'tail/short', leaf: true,
          en: 'Dies too quickly', ja: 'すぐ消えてしまう' },
      ],
      unsure: 'tail/unsure',
    },
    tone: {
      q: { en: 'Which way is the colour off?', ja: '色あいはどちら寄りですか？' },
      a: [
        { tag: 'tone/bright', leaf: true,
          en: 'Too bright, too harsh', ja: '明るすぎる・キンキンする' },
        { tag: 'tone/dark', leaf: true,
          en: 'Too dull, too dark', ja: 'こもる・暗い' },
      ],
      unsure: 'tone/unsure',
    },
  },
};

const LANG_KEY = 'audition:lang';
const LANGS = ['en', 'ja'];

let lang = pickLang();
const listeners = new Set();

function pickLang() {
  const saved = localStorage.getItem(LANG_KEY);
  if (LANGS.includes(saved)) return saved;
  // The browser decides the first visit. Anything declaring Japanese gets
  // Japanese; everything else gets English, which is the repository's language.
  const want = (navigator.languages || [navigator.language || 'en']).join(',');
  return /\bja\b/i.test(want) ? 'ja' : 'en';
}

/// The string for a key, with `{name}` placeholders filled from `vars`.
export function t(key, vars) {
  const table = STRINGS[lang] || STRINGS.en;
  const raw = (key in table) ? table[key] : STRINGS.en[key];
  if (raw === undefined) return key;
  if (!vars) return raw;
  return raw.replace(/\{(\w+)\}/g, (m, name) =>
    (name in vars ? String(vars[name]) : m));
}

/// A phrase carried in the tree rather than in the string table.
export const phrase = (obj) => (obj && (obj[lang] || obj.en)) || '';

export const currentLang = () => lang;
export const languages = () => LANGS.slice();
export const tree = () => TREE;

export function setLang(next) {
  if (!LANGS.includes(next) || next === lang) return;
  lang = next;
  localStorage.setItem(LANG_KEY, lang);
  document.documentElement.lang = lang;
  document.body.classList.toggle('lang-ja', lang === 'ja');
  applyStatic();
  for (const fn of listeners) fn(lang);
}

/// Re-render whatever the caller draws itself, whenever the language changes.
export function onLang(fn) { listeners.add(fn); }

/* The markup carries its own keys, so a static label is translated where it is
 * written rather than in a list somewhere else that has to be kept in step:
 * `data-i18n` for the text, `data-i18n-title` / `-placeholder` / `-aria` for the
 * attributes that are also read aloud. */
export function applyStatic(root) {
  const where = root || document;
  for (const node of where.querySelectorAll('[data-i18n]')) {
    node.textContent = t(node.dataset.i18n);
  }
  for (const node of where.querySelectorAll('[data-i18n-title]')) {
    node.title = t(node.dataset.i18nTitle);
  }
  for (const node of where.querySelectorAll('[data-i18n-placeholder]')) {
    node.placeholder = t(node.dataset.i18nPlaceholder);
  }
  for (const node of where.querySelectorAll('[data-i18n-aria]')) {
    node.setAttribute('aria-label', t(node.dataset.i18nAria));
  }
  document.title = t('app.title');
}

export function initLang() {
  document.documentElement.lang = lang;
  document.body.classList.toggle('lang-ja', lang === 'ja');
  applyStatic();
}
