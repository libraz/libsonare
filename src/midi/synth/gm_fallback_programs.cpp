#include "midi/synth/gm_fallback_data.h"

namespace sonare::midi::synth::detail {
namespace {

ProgramOverrides build_program_overrides() noexcept {
  ProgramOverrides o{};
  configure_keyed_programs(o);
  configure_percussion_programs(o);
  configure_physical_programs(o);

  o.e_piano = clamp_synth_patch(o.e_piano);
  o.harpsichord = clamp_synth_patch(o.harpsichord);
  o.harpsichord_octave = clamp_synth_patch(o.harpsichord_octave);
  o.harpsichord_wide = clamp_synth_patch(o.harpsichord_wide);
  o.harpsichord_keyoff = clamp_synth_patch(o.harpsichord_keyoff);
  o.clav = clamp_synth_patch(o.clav);
  o.celesta = clamp_synth_patch(o.celesta);
  o.glockenspiel = clamp_synth_patch(o.glockenspiel);
  o.music_box = clamp_synth_patch(o.music_box);
  o.vibraphone = clamp_synth_patch(o.vibraphone);
  o.marimba = clamp_synth_patch(o.marimba);
  o.xylophone = clamp_synth_patch(o.xylophone);
  o.tubular_bells = clamp_synth_patch(o.tubular_bells);
  o.dulcimer = clamp_synth_patch(o.dulcimer);
  o.nylon_guitar = clamp_synth_patch(o.nylon_guitar);
  o.electric_guitar = clamp_synth_patch(o.electric_guitar);
  o.muted_guitar = clamp_synth_patch(o.muted_guitar);
  o.overdriven = clamp_synth_patch(o.overdriven);
  o.distortion = clamp_synth_patch(o.distortion);
  o.bass_acoustic = clamp_synth_patch(o.bass_acoustic);
  o.bass_fingered = clamp_synth_patch(o.bass_fingered);
  o.bass_picked = clamp_synth_patch(o.bass_picked);
  o.bass_fretless = clamp_synth_patch(o.bass_fretless);
  o.bass_slap = clamp_synth_patch(o.bass_slap);
  o.bass_pop = clamp_synth_patch(o.bass_pop);
  o.harp = clamp_synth_patch(o.harp);
  o.sitar = clamp_synth_patch(o.sitar);
  o.shamisen = clamp_synth_patch(o.shamisen);
  o.koto = clamp_synth_patch(o.koto);
  o.church_organ = clamp_synth_patch(o.church_organ);
  o.reed_organ = clamp_synth_patch(o.reed_organ);
  o.harmonica = clamp_synth_patch(o.harmonica);
  o.bandoneon = clamp_synth_patch(o.bandoneon);
  o.orchestra_hit = clamp_synth_patch(o.orchestra_hit);
  o.tremolo_strings = clamp_synth_patch(o.tremolo_strings);
  o.pizzicato = clamp_synth_patch(o.pizzicato);
  o.timpani = clamp_synth_patch(o.timpani);
  o.choir_aahs = clamp_synth_patch(o.choir_aahs);
  o.voice_oohs = clamp_synth_patch(o.voice_oohs);
  o.synth_voice = clamp_synth_patch(o.synth_voice);
  o.tinkle_bell = clamp_synth_patch(o.tinkle_bell);
  o.agogo = clamp_synth_patch(o.agogo);
  o.steel_drums = clamp_synth_patch(o.steel_drums);
  o.woodblock = clamp_synth_patch(o.woodblock);
  o.taiko = clamp_synth_patch(o.taiko);
  o.melodic_tom = clamp_synth_patch(o.melodic_tom);
  o.synth_drum = clamp_synth_patch(o.synth_drum);
  o.reverse_cymbal = clamp_synth_patch(o.reverse_cymbal);
  o.violin = clamp_synth_patch(o.violin);
  o.viola = clamp_synth_patch(o.viola);
  o.cello = clamp_synth_patch(o.cello);
  o.contrabass = clamp_synth_patch(o.contrabass);
  o.trumpet = clamp_synth_patch(o.trumpet);
  o.trombone = clamp_synth_patch(o.trombone);
  o.tuba = clamp_synth_patch(o.tuba);
  o.muted_trumpet = clamp_synth_patch(o.muted_trumpet);
  o.french_horn = clamp_synth_patch(o.french_horn);
  o.soprano_sax = clamp_synth_patch(o.soprano_sax);
  o.alto_sax = clamp_synth_patch(o.alto_sax);
  o.tenor_sax = clamp_synth_patch(o.tenor_sax);
  o.baritone_sax = clamp_synth_patch(o.baritone_sax);
  o.oboe = clamp_synth_patch(o.oboe);
  o.english_horn = clamp_synth_patch(o.english_horn);
  o.bassoon = clamp_synth_patch(o.bassoon);
  o.clarinet = clamp_synth_patch(o.clarinet);
  o.piccolo = clamp_synth_patch(o.piccolo);
  o.concert_flute = clamp_synth_patch(o.concert_flute);
  o.recorder = clamp_synth_patch(o.recorder);
  o.pan_flute = clamp_synth_patch(o.pan_flute);
  o.blown_bottle = clamp_synth_patch(o.blown_bottle);
  o.shakuhachi = clamp_synth_patch(o.shakuhachi);
  o.tin_whistle = clamp_synth_patch(o.tin_whistle);
  o.ocarina = clamp_synth_patch(o.ocarina);
  return o;
}

}  // namespace

const ProgramOverrides& program_overrides() noexcept {
  static const ProgramOverrides kTable = build_program_overrides();
  return kTable;
}

}  // namespace sonare::midi::synth::detail
