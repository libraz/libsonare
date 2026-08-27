#pragma once

#include "midi/synth/gm_fallback_families.h"

namespace sonare::midi::synth::detail {

/// GS variation tones for the model floor. A variation hangs under a capital
/// tone at a Bank Select MSB (CC#0) number and is the SAME instrument voiced
/// differently, so each patch here starts as a copy of its capital and changes
/// only what the variation's name claims is different. Anything a copy does not
/// change is inherited, which is what keeps a variation in step with its capital
/// when the capital is re-voiced.
///
/// Roland's tone-list suffixes decode as: `w` a wide (stereo-spread) voicing,
/// `d` a detuned/darkened one, `o` the key-off release noise, `v` a
/// velocity-switched mix. The suffix, not a guess at the sample, is what each
/// delta below implements.
///
/// Runs LAST of the configure_* passes: every variation derives from a capital
/// tone the earlier passes have already built (or from a family patch, for a
/// capital that has no override of its own), so the deltas here read as "this
/// variation minus its capital" rather than restating a whole voice.
SONARE_TUNED_CONSTEXPR void configure_variation_programs(ProgramOverrides& o) noexcept {
  // The table this builds is constant-initialised, so the family patches have
  // to come from the builder rather than from the `family_patches()` accessor,
  // whose function-local static exists only once the process is running.
  const std::array<NativeSynthPatch, 16> fam = build_family_patches();

  // --- Piano (capital: program 0, the family-0 waveguide grand) -------------
  // Piano 1w: the same grand miked wide — the bass/treble halves pulled further
  // apart across the stereo field, with a touch more unison spread so the wider
  // image is carried by the strings rather than by a pan alone.
  // Written as factors on the capital rather than as absolute values: the grand
  // is the one voice in this table whose own numbers are fitted against a
  // reference, and a variation stated absolutely would drift away from it every
  // time it is refitted. A factor keeps "wider than the capital" true whatever
  // the capital becomes.
  o.piano_wide = fam[0];
  o.piano_wide.stereo_spread = 0.55f;
  o.piano_wide.piano.detune_cents = fam[0].piano.detune_cents * 2.2f;

  // Piano 1d (GM2: "dark"): the lid down. A softer, longer felt contact loses
  // the upper partials at the hammer instead of filtering them off afterwards,
  // which is what separates a mellow piano from a muffled one.
  o.piano_dark = fam[0];
  o.piano_dark.piano.brightness = fam[0].piano.brightness * 0.69f;
  o.piano_dark.piano.hammer_contact_ms = fam[0].piano.hammer_contact_ms * 1.7f;
  o.piano_dark.piano.soundboard = fam[0].piano.soundboard * 0.85f;

  // Piano 2w / 3w / 4w: the same wide miking on programs 1-3. Each derives from
  // its own capital rather than from the grand -- sharing one wide patch across
  // four capitals would have made a variation quieter, duller or in tune than
  // the tone it hangs under, which is not what `w` means.
  o.bright_piano_wide = o.bright_piano;
  o.bright_piano_wide.stereo_spread = 0.55f;
  o.bright_piano_wide.piano.detune_cents = o.bright_piano.piano.detune_cents * 2.2f;
  o.electric_grand_wide = o.electric_grand;
  o.electric_grand_wide.stereo_spread = 0.45f;
  o.electric_grand_wide.piano.detune_cents = o.electric_grand.piano.detune_cents * 2.2f;
  // The honky-tonk's unisons are already far apart, so widening it is the pan
  // alone: another factor of two on a beat this deep is a different instrument,
  // not a wider one.
  o.honky_tonk_wide = o.honky_tonk;
  o.honky_tonk_wide.stereo_spread = 0.55f;

  // --- Electric piano (capital: programs 4-5, one FM voice) ----------------
  // Detuned EP 1 / 2: two tine assemblies a few cents apart. The beat between
  // them is the effect, so it is voiced as per-voice pitch drift plus a stereo
  // spread that puts the two halves either side, not as a static offset (which
  // would only retune the instrument).
  o.e_piano_detuned_1 = o.e_piano;
  o.e_piano_detuned_1.drift_cents = 4.5f;
  o.e_piano_detuned_1.drift_rate_hz = 0.45f;
  o.e_piano_detuned_1.stereo_spread = 0.4f;
  o.e_piano_detuned_1.fm.ops[2].detune_cents = 6.0f;  // the tine pair, not the body
  o.e_piano_detuned_2 = o.e_piano_detuned_1;
  o.e_piano_detuned_2.drift_cents = 7.0f;
  o.e_piano_detuned_2.fm.ops[2].detune_cents = 9.0f;

  // E.Piano 1v / 2v: the velocity-switched mix. A Rhodes bar bites into a
  // metallic bark when it is struck hard, which on an FM voice is the tine
  // operator's level following velocity much more steeply than the body's.
  o.e_piano_velocity_1 = o.e_piano;
  o.e_piano_velocity_1.fm.ops[3].vel_to_level = 1.0f;
  o.e_piano_velocity_1.fm.ops[1].vel_to_level = 0.85f;
  o.e_piano_velocity_1.vel_to_cutoff_cents = 2400.0f;
  o.e_piano_velocity_2 = o.e_piano_velocity_1;
  o.e_piano_velocity_2.fm.ops[3].vel_to_level = 0.9f;

  // 60's E.Piano: the reed piano rather than the tine one. A struck steel reed
  // has no long-ringing tine ping over it, so the "ping" operator drops back
  // and the body carries a harder, more odd-harmonic bark that dies faster.
  o.e_piano_sixties = o.e_piano;
  o.e_piano_sixties.fm.ops[1].ratio = 2.0f;
  o.e_piano_sixties.fm.ops[1].level = 1.4f;
  o.e_piano_sixties.fm.ops[3].level = 0.5f;
  o.e_piano_sixties.amp_env.decay_ms = 1800.0f;
  o.e_piano_sixties.fm.ops[0].env.decay_ms = 1800.0f;

  // --- Drawbar organ (capitals: programs 16 and 17) -------------------------
  // Detuned Or.1 / 2: the chorus/vibrato scanner. A tonewheel generator is
  // fixed-pitch, so what moves is the scanner's delay, heard as a slow beat
  // across the drawbars — per-voice drift plus a wider image, and the second
  // organ's darker registration under it. 17's two variations carry its
  // percussion, which is what a detuned or bright percussive organ is.
  o.organ_detuned_1 = o.drawbar_organ;
  o.organ_detuned_1.drift_cents = 6.0f;
  o.organ_detuned_1.drift_rate_hz = 0.7f;
  o.organ_detuned_1.stereo_spread = 0.45f;
  o.organ_detuned_2 = o.percussive_organ;
  o.organ_detuned_2.drift_cents = 8.0f;
  o.organ_detuned_2.drift_rate_hz = 0.7f;
  o.organ_detuned_2.stereo_spread = 0.45f;
  o.organ_detuned_2.additive.drawbars = {8.0f, 8.0f, 6.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.0f, 4.0f};

  // 60's Organ 1: the registration of the decade — the first three drawbars
  // fully out with nothing above them, and the key click hard.
  o.organ_sixties = o.drawbar_organ;
  o.organ_sixties.additive.drawbars = {8.0f, 8.0f, 8.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  o.organ_sixties.additive.key_click = 0.7f;

  // Organ 4: every drawbar out — the full, hooting tutti.
  o.organ_4 = o.drawbar_organ;
  o.organ_4.additive.drawbars = {8.0f, 8.0f, 8.0f, 8.0f, 8.0f, 8.0f, 8.0f, 8.0f, 8.0f};
  o.organ_4.gain = o.drawbar_organ.gain * 0.8f;  // nine partials at full, not five

  // Organ 5: the bright end drawn instead of the fundamental — the thin,
  // upper-partial registration that cuts through a band.
  o.organ_5 = o.percussive_organ;
  o.organ_5.additive.drawbars = {6.0f, 4.0f, 8.0f, 4.0f, 8.0f, 4.0f, 6.0f, 6.0f, 8.0f};
  o.organ_5.additive.key_click = 0.55f;

  // --- Chromatic percussion ------------------------------------------------
  // Vib.w / Marimba w: one bar bank spread across the field. The bars are a
  // row two metres wide in front of the player, so the spread is the whole
  // difference — the voicing is untouched.
  o.vibraphone_wide = o.vibraphone;
  o.vibraphone_wide.stereo_spread = 0.55f;
  o.marimba_wide = o.marimba;
  o.marimba_wide.stereo_spread = 0.5f;

  // Church Bell (program 14 variation 8): a cast bronze bell, not the tuned
  // tube the capital voices. A founder tunes five partials — hum an octave
  // below, prime, the MINOR-third tierce that gives a bell its dark colour,
  // quint, and nominal an octave above the prime — and the strike note is the
  // missing fundamental they imply. The tierce is what a tubular bell does not
  // have, so it carries the variation.
  o.church_bell = o.tubular_bells;
  o.church_bell.modal.num_modes = 7;
  o.church_bell.modal.modes[0] = {0.5f, 0.55f, 1.3f};   // hum
  o.church_bell.modal.modes[1] = {1.0f, 1.0f, 1.0f};    // prime
  o.church_bell.modal.modes[2] = {1.19f, 0.85f, 0.9f};  // tierce (minor third)
  o.church_bell.modal.modes[3] = {1.5f, 0.6f, 0.7f};    // quint
  o.church_bell.modal.modes[4] = {2.0f, 0.5f, 0.8f};    // nominal
  o.church_bell.modal.modes[5] = {2.5f, 0.3f, 0.45f};   // deciem
  o.church_bell.modal.modes[6] = {3.0f, 0.22f, 0.3f};   // undecime
  o.church_bell.modal.decay_s = 20.0f;
  o.church_bell.modal.decay_stretch = 0.6f;
  o.church_bell.modal.strike_brightness = 0.85f;
  // The ring-down is deliberately held to the tubular bell's: this table's
  // longest release is the tail every fallback bounce is padded by
  // (gm_fallback_max_release_ms), so a tower bell's true ring would make every
  // render longer for one tone. The partial series, not the tail, is the
  // variation.
  o.church_bell.modal.release_damp_s = 8.0f;
  o.church_bell.amp_env.release_ms = 6000.0f;
  o.church_bell.gain = 0.55f;

  // Carillon (program 14 variation 9): the same founder's partial series in a
  // small bell — a short, bright ring rather than the tower bell's long swell.
  o.carillon = o.church_bell;
  o.carillon.modal.decay_s = 3.6f;
  o.carillon.modal.decay_stretch = 0.45f;
  o.carillon.modal.strike_brightness = 0.92f;
  o.carillon.modal.release_damp_s = 3.2f;
  o.carillon.amp_env.release_ms = 2800.0f;
  o.carillon.gain = 0.6f;

  // --- Church Organ (capital: program 19, the principal chorus) -------------
  // Church Org.2: the flute registration. Stopped (covered) pipes speak
  // fundamental-dominant with almost no upperwork, so drawing the gedackts and
  // dropping the mixture leaves the round, hollow colour a plenum buries.
  o.church_organ_flutes = o.church_organ;
  o.church_organ_flutes.pipe_organ.rank_count = 3;
  o.church_organ_flutes.pipe_organ.ranks[0] = {0.5f, true, 0.32f, 0.5f, 0.0f, 0.0f};  // 16' bourdon
  o.church_organ_flutes.pipe_organ.ranks[1] = {1.0f, true, 0.45f, 1.0f, 0.0f, 0.15f};  // 8' gedackt
  o.church_organ_flutes.pipe_organ.ranks[2] = {2.0f, false, 0.55f, 0.5f, 0.0f, 0.3f};  // 4' flute
  o.church_organ_flutes.pipe_organ.chiff = 0.24f;
  o.church_organ_flutes.pipe_organ.breath = 0.2f;
  o.church_organ_flutes.pipe_organ.keytrack = 0.3f;

  // Church Org.3: full organ — the plenum with the reed chorus drawn on top.
  // A lingual rank drives the same jet harder and asymmetrically, so the
  // trompette and the 16' bombarde add the brassy snarl that turns a chorus
  // into a tutti. Eight ranks is the per-key maximum (kMaxPipeRanks).
  o.church_organ_full = o.church_organ;
  o.church_organ_full.pipe_organ.rank_count = 8;
  o.church_organ_full.pipe_organ.ranks[6] = {0.5f,  false, 0.7f,
                                             0.45f, 0.72f, 0.4f};                    // 16' bombarde
  o.church_organ_full.pipe_organ.ranks[7] = {1.0f, false, 0.85f, 0.6f, 0.8f, 0.5f};  // 8' trompette
  o.church_organ_full.pipe_organ.breath = 0.42f;
  o.church_organ_full.pipe_organ.chiff = 0.45f;
  o.church_organ_full.gain = o.church_organ.gain * 0.82f;  // eight ranks per key, not six

  // --- Accordion (capital: program 21, on the free-reed voice) --------------
  // Accordion It: the Italian musette. Three reed banks tuned progressively
  // wider apart beat against each other — the wet, shimmering register that is
  // the whole difference from the dry French tuning of the capital.
  o.accordion_italian = o.reed_organ;
  o.accordion_italian.free_reed.detune = 0.46f;
  o.accordion_italian.free_reed.brightness = 0.58f;
  o.accordion_italian.stereo_spread = 0.3f;

  // --- Guitar --------------------------------------------------------------
  // Ukulele (program 24 variation 8): a short nylon string on a small box.
  // The string is the variation — a third of the speaking length rings down
  // far faster and with less decay stretch across the compass.
  o.ukulele = o.nylon_guitar;
  o.ukulele.ks.decay_s = 1.1f;
  o.ukulele.ks.decay_stretch = 0.35f;
  o.ukulele.ks.pick_position = 0.22f;
  o.ukulele.ks.brightness = 0.66f;
  o.ukulele.ks.nail = 0.4f;
  o.ukulele.body_mix = 0.42f;
  o.ukulele.gain = o.nylon_guitar.gain * 0.9f;

  // Nylon Gt.o (program 24 variation 16): the capital plus the key-off noise —
  // the fingers coming off the string and the fret buzz that follows. Roland's
  // `o` suffix is the release noise, the same one the "with key off"
  // harpsichord draws.
  o.nylon_guitar_keyoff = o.nylon_guitar;
  o.nylon_guitar_keyoff.ks.keyoff_noise = 0.42f;

  // 12-str.Gt (program 25 variation 8): six courses, each an octave pair. The
  // octave companion line IS the instrument; the pairs are never in perfect
  // tune, so the second polarization carries the chorus that follows from it.
  o.twelve_string_guitar = fam[3];
  o.twelve_string_guitar.ks.octave_mix = 0.55f;
  o.twelve_string_guitar.ks.polarization = 0.5f;
  o.twelve_string_guitar.ks.body_coupling = 0.45f;
  o.twelve_string_guitar.ks.decay_s = 4.0f;
  o.twelve_string_guitar.stereo_spread = 0.35f;
  o.twelve_string_guitar.gain = fam[3].gain * 0.85f;  // twelve strings share one pluck

  // Mandolin (program 25 variation 16): short doubled steel courses struck near
  // the bridge with a hard plectrum — bright, tight, and beating from the
  // course pairs rather than ringing on like a guitar.
  o.mandolin = fam[3];
  o.mandolin.ks.decay_s = 1.6f;
  o.mandolin.ks.decay_stretch = 0.4f;
  o.mandolin.ks.brightness = 0.72f;
  o.mandolin.ks.pick_position = 0.11f;
  o.mandolin.ks.exc_brightness = 0.92f;
  o.mandolin.ks.nail = 0.9f;
  o.mandolin.ks.polarization = 0.5f;
  o.mandolin.ks.body_coupling = 0.4f;
  o.mandolin.body_mix = 0.45f;

  // --- Strings -------------------------------------------------------------
  // Slow Violin (program 40 variation 8): the same instrument bowed into the
  // note. The bow's own rise, not the amp envelope alone, is what makes a
  // string swell — a slower rise reaches Helmholtz motion late, so the onset
  // is breathy before it is pitched.
  o.violin_slow = o.violin;
  o.violin_slow.bowed_string.attack_ms = 220.0f;
  o.violin_slow.bowed_string.bow_force = 0.48f;
  o.violin_slow.amp_env.attack_ms = 180.0f;
}

}  // namespace sonare::midi::synth::detail
