#include "midi/synth/piano_voice_calibration.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/piano_voice.h"
#include "midi/synth/pitch.h"
#include "util/constants.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

using sonare::constants::kMidiA4;
using sonare::constants::kSemitonesPerOctave;

namespace piano_detail {

/// Mezzo-forte reference velocity (0..1) the felt-hammer laws are anchored at.
SONARE_TUNABLE_SCOPED("piano_voice", kHammerMfVel, 0.6f);
/// Felt hysteresis coefficient: how much stiffer the hammer's loading curve is
/// than its unloading curve (see the contact solver's `ham_mu_`).
SONARE_TUNABLE_SCOPED("piano_voice", kHammerHysteresis, 0.229431f);
/// Additional felt-stiffness cutoff octaves per unit velocity above mf, per
/// unit hammer_dynamics: a compressed felt patch passes more of the pulse top.
SONARE_TUNABLE_SCOPED("piano_voice", kHammerDynBrightOct, 1.5f);
/// Felt-stiffness lowpass on the injected force, expressed as cycles of its
/// corner per hammer contact -- the corner is this over the reference contact
/// time, not a frequency. What a lossy contact passes is set by how long it
/// lasts, and the contact solver above already knows that for every note, so
/// the register grading comes out of it instead of being written down twice.
///
/// It had been a flat 2400 Hz. In the middle of the keyboard that is the ninth
/// harmonic and the filter is barely in the way; at C7 it is the fundamental
/// times 1.15, so the excitation carried nothing above the note itself and the
/// top of the keyboard rendered as a sine. Held to the contact instead, the
/// same value anchors C4 exactly where it was fitted and reaches 7 kHz at C7,
/// which is most of what the measurement asks for there -- and it darkens the
/// deep bass, where a big soft hammer dwells on the string for milliseconds,
/// rather than handing A0 the same 2.4 kHz burst as the treble.
///
/// The velocity term stays at zero: three octaves of climb had been laid on a
/// contact solver that already derives its own velocity response from the Hertz
/// laws, and the sum came out around four times the measured spread -- a C4's
/// partial stack ran from 15 dB below the reference at pianissimo to 8 dB above
/// it at fortissimo, where the reference barely moves. The mechanism is real,
/// so the climb stays fittable and a patch can still reach it through
/// hammer_dynamics; it is the magnitude that measured as voicing, not physics.
SONARE_TUNABLE_SCOPED("piano_voice", kFeltCutoffContactCycles, 4.615f);
SONARE_TUNABLE_SCOPED("piano_voice", kFeltCutoffVelOct, 0.0f);
/// Semitones the patch's reference contact time takes to DOUBLE as the note
/// descends. A grand's contact spans a far narrower range than its periods do —
/// well under a millisecond in the treble against a few in the bass — so this
/// is measured in tens of semitones, not in one octave. At 13.12 it doubled
/// every 1.09 octaves, which is very nearly holding the contact to a fixed
/// fraction of the period, and it handed A0 a 13.9 ms contact: a force pulse
/// whose first null falls at 72 Hz. The string was then driven only where the
/// soundboard cannot radiate it, so the whole bass note had to be carried by
/// the hammer knock and the scrub noise instead — silencing either one took an
/// A0 attack apart, which is what a string with no excitation of its own looks
/// like from the outside.
///
/// Only the bottom of the keyboard is governed by this: from C4 up the period
/// floor below is the binding term, so the fitted mid and treble do not move
/// with it at all (verified — notes 60 and above render identically).
SONARE_TUNABLE_SCOPED("piano_voice", kContactKeytrackSemis, 42.0f);
/// Hammer-contact floor in fundamental PERIODS, anchored at C4 and graded
/// per octave (signed: it shrinks into the bass, grows into the treble). The
/// contact duration is THE felt-vs-nail cue — a contact much shorter than half
/// a period injects a spike-like pulse that reads as a fingernail pluck, and in
/// the treble it additionally leaves the overtones louder than the fundamental
/// (a plucked-string spectrum).
///
/// This grading is what starves the top of the keyboard, and it is left alone
/// anyway. At 0.61 it hands C6 1.7 periods and everything above it the 2.0
/// ceiling below, and a contact lasting two periods is a force pulse whose
/// first spectral null falls at HALF the fundamental -- the string is driven
/// almost nowhere but at f0 and the top of the keyboard renders very nearly as
/// a sine, h2 measuring 14 dB under the reference at C6 and 24 under it at C7.
/// Measured contact spans about half a period at C4 against about one at C7,
/// which is 0.17 per octave, and at 0.17 the h2/h1 ladder recovers 17 dB.
///
/// It also makes the treble 4 to 9 dB louder in RMS -- C7 goes from 11 dB over
/// the reference to 16 -- and that is not a sharper transient being read by a
/// peak meter, because it is the same in RMS over the first second. The ladder
/// score cannot see it: it normalises each note by its own median, so a voice
/// that gets its partials by getting louder scores as one that got its partials.
/// Shortening the contact raises the radiated level because the injection is
/// normalised on the mezzo-forte PEAK FORCE rather than on the blow's impulse,
/// so a shorter, taller pulse arrives with the same peak and more of its energy
/// inside the string's band.
///
/// Fixing that normalisation had looked like the way in and measures as not
/// being one. Scored across the keyboard on level, envelope and sustained
/// colour together, the momentum anchor (kInjImpulseNorm = 1) is worse than
/// the peak-force anchor at every contact grading tried, and the literature
/// grading itself — half a period at C4, 0.17 per octave — is worse than both
/// values here whichever anchor carries it. What the fit wants is a contact
/// half the length at C4 and graded three times as steeply, and it wants it
/// for the partials: the ladder and the body improve while the crest factor
/// gives back about a decibel. The register profile of hammer-string contact
/// is not the only thing these two numbers are carrying, and until whatever
/// else they stand in for is named, they are calibration and not measurement.
SONARE_TUNABLE_SCOPED("piano_voice", kContactPeriodsAtC4, 0.18f);
SONARE_TUNABLE_SCOPED("piano_voice", kContactPeriodsPerOct, 0.5f);
SONARE_TUNABLE_SCOPED("piano_voice", kContactPeriodsMax, 2.0f);
/// What the injection's calibration is anchored on: the mezzo-forte blow's PEAK
/// force (0) or the momentum it delivers (1).
///
/// The peak anchor is not a free choice of units, it is a register tilt nobody
/// asked for. The mezzo-forte stiffness is solved so the blow lasts the
/// register's reference contact, `k = (c_p/tau)^(p+1)`, and the peak force of
/// that bounce works out at `f_peak = (c_p/tau) * (0.5(p+1))^(p/(p+1))` — it
/// goes as 1/tau exactly. Dividing by it therefore multiplies the injection by
/// tau. Meanwhile the pulse itself carries a FIXED impulse whatever the felt
/// does: a free bounce reverses a unit mass's unit velocity, so the integral of
/// the force is 2 regardless of stiffness or duration. The normalization is the
/// only thing that breaks that invariance, and it breaks it by a factor of two
/// across the keyboard — C4's mezzo-forte contact is 92 samples against C7's 46.
///
/// The cost is not the tilt itself but that the tilt is welded to the contact
/// GRADING, which is the one control the partial ladder answers to. Regrading
/// the contact to what the instrument measures moves the level by an amount
/// nothing chose, so the ladder can only be bought by spending level and the
/// register compensation that exists for the job (kInjTiltDbOct) never gets to
/// own it. Anchored on the impulse instead, contact duration sets the spectrum
/// and only the spectrum: a shorter contact still radiates more, because more
/// of the same momentum lands inside the string's band rather than below its
/// fundamental, and that part is real and is supposed to be there.
///
/// The anchor is C4, so the middle of the keyboard is unmoved at either setting
/// and only the ends are re-levelled.
SONARE_TUNABLE_SCOPED("piano_voice", kInjImpulseNorm, 0.0f);
/// Ceiling on ONE blow's contact, in fundamental periods, and never below the
/// floor above: the two are the same physical statement — the string's own
/// reflection returns while the felt is still loaded and decides when the
/// hammer leaves — so in the treble, where the floor is the binding one, they
/// meet.
///
/// The free bounce on its own has no ceiling: contact goes as v^-((p-1)/(p+1)),
/// so a soft treble blow dwells for nearly four periods, which puts the force
/// pulse's first null below the fundamental and all but erases the note. A
/// pianissimo C7 rendered 34 dB under the reference where the same note at
/// fortissimo sat 12 dB under it, and the pp->ff level swing came out at +48 dB
/// against a measured +25 — the model's soft treble simply vanished, which is
/// the register a melody is played in.
///
/// The ceiling is imposed by stiffening the felt for that blow, which is the
/// same statement in the solver's terms. The mezzo-forte calibration the
/// injection normalizes against is left at the unstiffened value, so the
/// velocity level curve still comes out of the dynamics rather than out of the
/// normalization. Through the middle of the keyboard the free bounce never
/// reaches the ceiling and nothing changes: every note at or below C4 renders
/// the same as it did without one.
///
/// The value is fitted, and one period is where the fit landed — which is the
/// round trip the reflection makes, so the number reads as the mechanism rather
/// than as a coincidence. Tightening it further starts shortening mid-register
/// contacts, which is a different change wearing this one's clothes.
SONARE_TUNABLE_SCOPED("piano_voice", kContactPeriodsPerBlowMax, 1.0f);
/// What the voice puts out, scaling the injected force and the noise/knock
/// paths together so the balance between them does not move with it.
///
/// This voice is built up from physical calibration — felt stiffness, contact
/// duration, string admittance, radiation — and none of those steps knows what
/// the result should measure. Left unnormalized it came out 16 dB under the
/// rest of the GM bank: a C4 at velocity 100 peaked at -32 dBFS where the
/// harpsichord and the nylon guitar, which are struck-and-decaying in the same
/// way, both sat near -15.5, and the same 16 dB separated it from a concert
/// grand recorded dry. The gap is flat across velocity, so it is a level, not
/// a curve. A piano that quiet is buried by everything it plays with.
///
/// The bank-balance knob is the family's own `gain`, which stays where it is:
/// the defect is here, in a voice whose output was never anchored to anything,
/// and it is worth 16 dB more than the family gain can even express (that field
/// clamps at 4).
SONARE_TUNABLE_SCOPED("piano_voice", kOutputLevel, 5.7f);
/// Where the aftersound taper starts, in octaves above C4. Read on the note's
/// own partial rows, three concert grands hold a flat -4.5 to -6.3 dB/s from A0
/// to C5 and steepen past it. The broadband envelope this was first taken from
/// reports no trend anywhere, because above the middle it stops describing the
/// string at all — see `_late_bed` in the capture definition.
SONARE_TUNABLE_SCOPED("piano_voice", kTrebleDecayKneeOct, 1.25f);
/// Halvings of the aftersound stage per octave past that knee, applied to the
/// slow stage only — the prompt rate is the polarization/unison coupling's, and
/// has its own register profile below. The same three put C7 and C8 at -13 dB/s
/// against -6 in the middle, a factor of two across two octaves; at this the
/// worst note's error falls from 6.0 to 1.7 times their own disagreement, and
/// the late on-partial level from 34 dB over the reference at C8 to 4.
SONARE_TUNABLE_SCOPED("piano_voice", kTrebleDecayOct, 1.4f);
/// Where that narrowing bottoms out, in octaves above C4. The partials are flat
/// again past C7 (-13.4 dB/s there against -12.8 at C8), so the taper stops
/// instead of running on and leaving the top octave a click.
SONARE_TUNABLE_SCOPED("piano_voice", kTrebleDecayFloorOct, 3.0f);
/// Register profile of the prompt-vs-aftersound contrast. The double decay is
/// strongest in the trichord mid-range (vertical polarization + unison
/// coupling drain the bridge fast, then the decohered residue rings): the
/// wound bass strings have no unison partner and ring at essentially the
/// aftersound rate, and the capped treble is so short-lived the two stages
/// merge. Gaussian in octaves from C4; at the edges the effective prompt rate
/// relaxes toward the aftersound rate.
///
/// It had been six-tenths of an octave wide, against a whole keyboard, and
/// pinned to a centre it never earned (see kTwoStageCenterOct). Together those
/// two put a deep narrow trough of prompt decay on C4 and left the notes a
/// fifth either side with almost none: measured on the model, the level lost by
/// 1.2 s ran 2.2 dB at C3, 22.8 at C4 and 11.9 at C5 — a twenty-decibel swing
/// inside one octave, which the instrument does not have. No per-note shape
/// metric can see it, because they are all normalised per note; it shows up as
/// a keyboard level profile with a hole in it, and in the crest factor, which
/// ran 9.6 dB off across the keyboard.
///
/// Widening had been rejected before, and for a real reason: it bought the
/// level by spending brightness. The reason is the prompt stage itself —
/// it is applied by subtracting a share of the summed bridge signal, which is a
/// low-partial-weighted quantity, so more prompt decay necessarily drains the
/// bottom of the spectrum and leaves a brighter residue. A real string's fast
/// polarization does not do that.
///
/// That objection was a statement about the drain's bandwidth, not about the
/// width, and kTwoStageDrainPartials is what answers it: band-limit the
/// subtraction to the register the mechanism was derived for and the trade
/// disappears. With the drain limited and the centre free, the profile widens
/// to something the size of the instrument and the keyboard's crest error falls
/// from 9.6 dB to 6.0 while the sustained band profile improves alongside it
/// instead of paying for it.
SONARE_TUNABLE_SCOPED("piano_voice", kTwoStageWidthOct, 2.0f);
/// Where that profile peaks, in octaves from C4. Zero is the identity and is
/// where it had always been, but only because C4 is the pivot every other
/// keytrack in this file measures from — the contrast was given the same origin
/// rather than its own, and a shared origin is not a measurement. The reference
/// puts the peak somewhere else entirely: the prompt-vs-aftersound gap is
/// weakest at C4 (-7.2 dB/s against -5.1) and sharpest at F#4 and C5 (-31 and
/// -41 against -1.9 and -2.9), so the Gaussian's crest was sitting on the one
/// note where the instrument has almost no double decay at all.
///
/// Fitted across the keyboard it lands two octaves up, around C6, which is
/// further than the per-note rates alone suggest and is not a contradiction of
/// them: the profile also has to carry the treble's own short decay, so the
/// peak sits where the fast stage is needed most rather than where the contrast
/// is sharpest. It is one of the largest single terms in the voice, and it is
/// overloaded in exactly that way — half of what it carries is a property of
/// frequency rather than of register, and kBridgeHfDrain is the half that has
/// been taken out of it and stated directly.
SONARE_TUNABLE_SCOPED("piano_voice", kTwoStageCenterOct, 2.4f);
/// Treble taper cap (octaves above C4): the decay/darkening keytracks stop
/// steepening past here — an uncapped taper leaves the top octave with a
/// sub-100 ms husk of a note.
SONARE_TUNABLE_SCOPED("piano_voice", kTrebleTaperOctCap, 1.5f);
/// Treble loop darkening (effective-brightness drop per octave above C4): the
/// treble string's upper partials decay much faster than its fundamental, so
/// the loop lowpass closes toward the top even for a bright patch voicing.
/// Kept gentle: over-closing leaves a 3-4 partial flageolet instead of a
/// piano treble.
SONARE_TUNABLE_SCOPED("piano_voice", kTrebleBrightPerOct, 0.06f);
/// Effective-brightness drop per octave BELOW C4 (wound-string mid-partial
/// damping; see bright_eff). The h1 decay is unaffected — the loop-lowpass
/// loss at the fundamental is compensated (lp_comp), so this only shortens
/// the upper partials.
SONARE_TUNABLE_SCOPED("piano_voice", kBassDarkPerOct, 0.06f);
/// String-to-string inharmonicity spread inside a unison (fractional jitter
/// on the dispersion allpass): real unison strings never share an exact B, so
/// each partial's unison beat runs at its own rate. Identical coefficients
/// make every partial null in lockstep — an audibly artificial hollow dip.
///
/// The size of the spread is bounded from above by roughness, and the bound
/// bites long before the ear notices the lockstep. A partial's frequency
/// carries the stiffness as k^2, so a spread that is inaudible at the
/// fundamental separates a high partial's three copies by tens of hertz —
/// which is the rate the ear hears as roughness rather than as pitch. Measured
/// per critical band on a sustained C4 against three concert grands, every
/// band holding several partials above 2.5 kHz fluctuated 14 to 17 dB more
/// than the instrument's, in one case eighteen times what the three of them
/// disagree by, while nothing below h10 was out at all. Two percent lands
/// those bands within 2 dB; three percent is worse than two, so this is a real
/// optimum and not a ceiling. Real unison strings are the same gauge cut to
/// the same length and do not differ by five percent in anything.
SONARE_TUNABLE_SCOPED("piano_voice", kUnisonStiffJitter, 0.02f);
/// Under the soft pedal the action rides a softer, less-grooved felt patch that
/// compresses far less under a hard blow, so the velocity dynamics is scaled
/// down there (this also preserves the una-corda high-frequency softening).
SONARE_TUNABLE_SCOPED("piano_voice", kUnaCordaDynScale, 0.4f);
/// Uneven unison strike: hammer crowning and string leveling never deliver
/// equal energy to a bichord/trichord's strings. Equal amplitudes make the
/// unison beats cancel to full-depth nulls (an audible slow chorus wobble);
/// the uneven strike keeps them as shallow ripple and seeds the aftersound.
SONARE_TUNABLE_SCOPED("piano_voice", kUnisonStrikeUneven, 0.15f);
/// Uneven bridge coupling across the unison (Weinreich): each string meets
/// the bridge at a slightly different impedance, so the antisymmetric normal
/// mode — whose string motions cancel at an ideal bridge — still radiates,
/// at roughly this fraction of the symmetric mode. This is what makes the
/// aftersound AUDIBLE without detuning the unison out of the locked regime:
/// with equal radiation the slow mode is silent, and compensating with deep
/// detune buys the second stage at the cost of a chorus-like beating
/// fundamental no tuned piano has.
///
/// It had been past that warning rather than short of it, which is worth
/// stating because the paragraph above already described what was happening.
/// Above one the antisymmetric mode radiates louder than the symmetric one, so
/// the two components arrive near equal and the beat between them reaches full
/// depth. Measured over a sustained C5 against three concert grands, whose
/// fundamentals wobble 5.2 to 11.6 dB, this voice wobbled 21.1.
///
/// The setting is a trade and the losing side is named here rather than left
/// to be rediscovered. Bringing it down costs the bass: C1, E1 and F#2 land
/// 0.3 to 1.0 dB shallower than the least-beating of the three instruments,
/// against ten decibels recovered at C5. Silencing the slow mode outright also
/// costs C5 five decibels of its two-second aftersound, which is why this is
/// half and not zero.
SONARE_TUNABLE_SCOPED("piano_voice", kUnisonRadSpread, 0.5f);
/// Felt impact noise: level relative to the hammer amplitude, exponential
/// decay time, and hard stop of the burst. The noise passes the same
/// velocity-driven felt-stiffness lowpass as the pulse, so soft strikes thud
/// and hard strikes click.
SONARE_TUNABLE_SCOPED("piano_voice", kStrikeNoiseGain, 0.75f);
SONARE_TUNABLE_SCOPED("piano_voice", kStrikeNoiseTauMs, 8.0f);
SONARE_TUNABLE_SCOPED("piano_voice", kStrikeNoiseMaxMs, 30.0f);
/// The impact noise radiates through a darker path than the string pulse: a
/// felt hammer lands as a 0.5-2 kHz thud, not a pick click — the noise gets
/// its own lowpass at this fraction of the felt-stiffness cutoff.
///
/// Widening this to chase a thin partial stack is the trap on this path. It
/// works, on paper: a broadband floor lifts the measured level at every partial
/// frequency, so the partial-stack figure improves steadily as it opens. What
/// the partial ladder cannot see is that none of that lift is tonal — carried
/// far enough it takes the spectral centroid to more than three times the
/// reference's, and the note is audibly hissy long before the ladder complains.
/// Read the two together or this constant will fit itself into noise.
SONARE_TUNABLE_SCOPED("piano_voice", kStrikeNoiseCutoffScale, 0.487539f);
/// Halvings of the noise cutoff per octave below C4 (see noise_cutoff).
///
/// Zero, because the instrument grades the other way and a bass hammer being
/// bigger and softer says nothing about what the blow radiates. Measured on
/// three concert grands, the BRIGHTEST attack on the keyboard is the bass:
/// 810 Hz of spectral centroid over the first 50 ms at C2 against 581 at C4.
/// Graded at a half this voice read 337 there — under half the instrument's,
/// and 16 to 24 dB short at 500 Hz and 1 kHz, which is the band a blow is
/// heard through. Ungraded it reaches 663 at C2 and lands on the instrument at
/// C3, and it costs nothing above C4 by construction.
///
/// Nineteen knobs were swept for connectivity to that band before this one was
/// touched; four of them (kStrikeNoiseDirect, kNoiseTrebleTaperOct,
/// kInjTiltDbOct, kContactPeriodsAtC4) do not move it at all. The neighbouring
/// cutoff scale reaches it too and is the wrong lever: it opens the noise
/// across the whole keyboard and buys the brightness as hiss, which is the
/// trap its own commentary describes. This one costs no tonality anywhere —
/// the sustained 2-8 kHz figure reads the same on both sides of the change.
SONARE_TUNABLE_SCOPED("piano_voice", kNoiseCutoffBassOct, 0.0f);
/// Third noise pole, placed this factor above the main cutoff: the felt
/// noise keeps its passband but falls off a cliff past it — the reference
/// attack holds energy to a few kHz then drops ~37 dB into the next octave,
/// a shape two poles cannot make (their tail is what read as a jack click).
SONARE_TUNABLE_SCOPED("piano_voice", kNoiseSteepRatio, 4.0f);
/// Finite hammer-head width: the felt contacts several percent of the string
/// length, and that footprint lowpasses the injected force (partials whose
/// half-wavelength fits inside the footprint cancel). Caps the pulse content
/// at about this multiple of the fundamental — without it the bass/tenor
/// overtones around h6-h12 come out plectrum-hard (a honky, dry midrange no
/// felt hammer produces). The strike-noise path is NOT capped: the scrub
/// noise is what carries the airy top sheen.
SONARE_TUNABLE_SCOPED("piano_voice", kHammerWidthHarmonics, 2.69125f);
/// Share of the impact noise injected into the strings themselves: the felt
/// scrub and the wave re-striking the hammer during contact excite the
/// string broadband. This is what seeds the HIGH partials (h10+) — the
/// smooth force pulse alone rolls off ~18 dB/oct past ~2/contact, leaving
/// the mid/bass sustain with no top: a bright strike into a dull ring reads
/// as a fingernail pluck. Random phase, so it does not re-cohere the loop.
SONARE_TUNABLE_SCOPED("piano_voice", kStrikeNoiseInject, 0.298027f);
/// Corner of the high shelf `attack_hf_dynamics` tilts the injected scrub with. Inert
/// while a patch leaves that field at zero.
SONARE_TUNABLE_SCOPED("piano_voice", kScrubHfShelfHz, 3680.0f);
/// The injection tapers above C4 (halvings per octave): the treble hammer
/// rests on the string for around a full period, shorting high-frequency
/// string motion at the contact point — broadband seeding there rings the
/// overtones into a harpsichord jangle instead of a piano treble.
SONARE_TUNABLE_SCOPED("piano_voice", kInjectTrebleTaperOct, 0.654102f);
/// ...and grows below C4 (doublings per octave): the massive bass hammer's
/// felt scrub and re-strike chatter seed the dense partial cloud a wound
/// string radiates (absent it, the bass is a clean plucked stack).
SONARE_TUNABLE_SCOPED("piano_voice", kInjectBassBoostOct, 1.23607f);
/// The impact-noise LEVEL also tapers above C4: the treble hammer is a few
/// grams of hard felt on a short string — its scrub is faint next to the
/// tone, where the same level against a fast-dying treble note reads as a
/// pick scratch riding every onset.
SONARE_TUNABLE_SCOPED("piano_voice", kNoiseTrebleTaperOct, 0.435016f);
/// Hammer-knock radiation (through the soundboard) relative to the string
/// injection, and its growth per octave BELOW C4: the wide wound bass
/// strings take a massive hammer whose impact drives the board directly —
/// on a reference grand the low-register attack peaks in the 60-250 Hz
/// thump, not in the string partials. Without that boom the exposed bass
/// harmonic stack reads as a harpsichord register.
/// Impact-knock level.
///
/// Judged against the note's own body rather than against anything absolute:
/// how far the low-band envelope peak stands over its own 30-100 ms. Three
/// concert grands stand 1.0 to 3.8 dB over it from C1 to C5; this voice stood
/// 4.3 to 6.1, and every note's envelope topped out in the first ten
/// milliseconds where the instrument's low band tops out 20 to 170 ms in and
/// then holds. A blow that stands too far over what follows it is heard as a
/// hit rather than as weight, whatever its spectrum.
///
/// The level cannot come down on its own: at this band it takes C2's broadband
/// rise from the instrument's 17 ms to 107. It comes down together with
/// kKnockThudHz moving up, and the pair is what keeps the rise while the
/// proportion corrects.
SONARE_TUNABLE_SCOPED("piano_voice", kKnockGain, 1.6f);
/// Extra velocity exponent on the knock, ON TOP of the one the blow force
/// already carries. Zero leaves the knock scaling exactly as the force does.
///
/// The force peak goes as v^(2p/(p+1)) and so does the string injection, so on
/// paper the two track and no exponent is needed. What they do not share is
/// what happens to that energy afterwards: a soft blow has a longer, duller
/// contact that puts less into the partials still sounding at 30 ms, while the
/// knock is a lowpassed copy of the same force and keeps all of its. The share
/// therefore climbs as the blow gets softer, which is the opposite of a piano.
/// Measured as the peak over the note's own 30-100 ms body against three
/// concert grands: at C4 they stand 4.6 to 5.7 dB over it at v24 and 3.8 to
/// 5.4 at v120 — flat — where this voice stood 11.4 and 5.1. Silencing the
/// knock at v24 took 7.7 dB out of that and at v88 only 1.7, which is the
/// measurement saying the excess is the knock's and that it is an exponent
/// rather than a level: halving the gain lands v24 inside the span and drops
/// v88 out the bottom.
///
/// It is also what an action does. A pianissimo is tone without knock: the key
/// is released gently, the shank and the whole action carry less structure-borne
/// impact into the board, and the tone that remains is the string's. The path
/// this scales is that one, not the string.
///
/// The value comes from a second measurement that shares no method with the one
/// above: the softest-to-hardest level range over the whole note grid, which the
/// exponent is not aimed at and which no other knob in this voice moves. Against
/// the reference it reads 2.84 dB out at zero and 1.49 at this value, or 3.1 and
/// 1.6 times the disagreement between the three instruments. Nothing else in the
/// gate shifts by a tenth of a decibel across the sweep, which is what a
/// mechanism that was missing looks like rather than a value that was mis-set.
/// Both measurements are taken on peaks, well clear of the corpus's own floor.
SONARE_TUNABLE_SCOPED("piano_voice", kKnockVelExp, 0.4f);
/// The knock radiates only the impact THUD: the hammer/action/board contact
/// pumps a fixed low band regardless of the note (a treble strike lands as a
/// quiet thock, not a burst at the string's own pitch). Radiating the raw
/// pulse instead puts note-frequency energy straight into the board and
/// sympathetic modes, whose stretch-detuned ring then beats against the
/// string fundamental — an audible onset notch in the treble.
///
/// The corner is where the thud stops, not where it sits, and at a third of a
/// kilohertz it stopped below the band a blow is heard through. Measured over
/// the first 50 ms against three concert grands, C2 was 16 dB short at 500 Hz
/// and 24 at 1 kHz while carrying a large excess below both — a bass attack
/// that is felt and not heard, which is the same shape a missing mid-band
/// always takes. Here C2's 500 Hz hole closes to within 2 dB of the
/// instrument's own reading.
SONARE_TUNABLE_SCOPED("piano_voice", kKnockThudHz, 1400.0f);
/// Halvings of the thud frequency per octave below C4 (see thud_hz).
///
/// Zero, and the grading is gone rather than reduced, because what it graded
/// away is the only path by which the blow reaches the air before the string
/// has built up. Every halving per octave puts C1's thud three octaves down:
/// at 2.8 the corner is one hertz, at 0.7 it is eighty, and at neither does the
/// knock reach the output at all in the bottom octave.
///
/// What that costs is the whole shape of a bass attack. Measured against three
/// concert grands, the instrument reaches its peak 23 ms after a C1 is struck,
/// 18 ms after a C2 and 20 ms after a C3, and has already fallen 2.5-3.6 dB by
/// 200 ms. Graded, this voice took 140, 78 and 61 ms and was still RISING at
/// 200 ms -- a swell rather than a blow -- and its peak stood only 4.3 dB over
/// the note at 0.3 s where the instrument's stands 11.0. Ungraded it reaches
/// 22, 21 and 20 ms with a 5.3 to 11.0 dB peak, which is the instrument to
/// within a few decibels on both counts, and it moves nothing at C4 and above,
/// where the string is fast enough not to need the help.
///
/// Nothing else reaches it. The board bank, the late field, unison detune, the
/// two-stage decay, the contact-time controls and the radiation corner were each
/// switched off or swept in turn; removing the board bank makes the rise WORSE
/// (321 ms at C1), and no other lever moved it by more than a few tens of ms.
SONARE_TUNABLE_SCOPED("piano_voice", kKnockThudBassOct, 0.0f);
/// Radiation bloom: the string radiates only through the board, whose modes
/// take time to ring up — the tone swells over tens of ms in the bass and a
/// few ms in the treble, while a plucked string (or a harpsichord jack) is
/// loudest at the very first cycle. One-pole rise time constant at C4 (ms)
/// and its per-octave keytrack (bass slower, treble faster). The knock/thud
/// path is NOT bloomed — the impact is the first thing heard.
SONARE_TUNABLE_SCOPED("piano_voice", kBloomTauMsC4, 4.6604f);
SONARE_TUNABLE_SCOPED("piano_voice", kBloomTauOct, 0.9f);
/// String yield under the blow (fraction of the hammer's speed the strike
/// point recedes at, at mf peak force). Keytracked DOWN toward the treble:
/// the treble hammer outweighs its short string many times over, so the
/// string barely loads the bounce (a clean full-period dwell); the wound
/// bass strings are massive and swing away under the light-relative hammer,
/// stretching the contact and softening the transfer.
SONARE_TUNABLE_SCOPED("piano_voice", kStringYield, 1.28f);
/// Register level compensation on the injected force (dB per octave from C4):
/// the bass chatter re-feeds its strings while the treble's near-period dwell
/// couples weakly into the fundamental, tilting the raw physical levels
/// bass-heavy by ~10 dB/oct against the reference.
SONARE_TUNABLE_SCOPED("piano_voice", kInjTiltDbOct, 1.5f);
/// How far from C4 that compensation keeps acting, in octaves, before it holds
/// flat. Both mechanisms it answers to are strongest at the ENDS of the
/// keyboard, so a span this narrow stops the correction exactly where its
/// subject is worst: past C5.25 every remaining octave gets the same fixed
/// 4.4 dB however far the raw level has drifted by then. Measured against the
/// reference the model runs 8 dB quiet at D1 and 10 dB loud at F#6 with this
/// pinned, and no lever inside the injection can reach either end.
SONARE_TUNABLE_SCOPED("piano_voice", kInjTiltOctSpan, 1.25f);
SONARE_TUNABLE_SCOPED("piano_voice", kYieldTrebleOct, 2.0f);
/// How the knock grows into the bass (doublings per octave below C4). The
/// heavy bass hammer really does rock the board harder, but the knock has no
/// highpass of its own — only the soundboard radiation below it — so growth
/// here lands mostly under 60 Hz, where nothing is heard and everything is
/// displaced. At 1.3 an A0 attack measured 43 dB over the reference in
/// 20-60 Hz while sitting 15-20 dB UNDER it everywhere above 200 Hz: a note
/// that is felt and not heard. Backing it off is worth more than it costs
/// right up to the point where 60-200 Hz starts thinning, which is where this
/// sits.
///
/// How far it can back off is set by whether the STRING carries the bass. While
/// the footprint cap was pinned by a numeric floor (see kOnePoleAlphaFloor) and
/// the contact ran three times too long, it did not: silencing the knock cost an
/// A0 attack every decibel it had below 200 Hz, because there was nothing else
/// down there. With the pulse reaching the register it drives, half the knock
/// comes out and the note keeps its weight.
SONARE_TUNABLE_SCOPED("piano_voice", kKnockBassBoostOct, 0.3f);
/// ...and DOES shrink above C4, but by a third of what a ratio reading asks
/// for, and for a reason the ratio reading gets wrong.
///
/// The trap first, because it is still there. Measured against the tone, the
/// reference's treble attack sits 23-26 dB under broadband where a C4 sits 16
/// under, and reading that as a taper is wrong: what radiates down there is
/// not the hammer, it is the instrument -- the same body, struck through the
/// bridge, at a level the string's pitch has little say in -- so the ratio
/// moves because the tone above it changes and not because the body does.
///
/// What the ratio reading missed is that there are two windows and they do not
/// agree. Measured on the reference over 0.4-1.4 s, where the note's own
/// fundamental has left the band, the 60-250 Hz level runs -39.6 dB at C4,
/// -37.8 at C5, -38.7 at C6, -37.7 at C7: flat to two decibels across three
/// octaves, which is what a fixed mass struck by a fixed blow gives, and which
/// is why this knob was held at zero. The first fifty milliseconds are NOT
/// flat. The same measurement reads -34.2, -44.7, -47.3 and -49.2 there --
/// fifteen decibels of fall across the same three octaves, agreed on by all
/// three instruments to within eight -- because the attack window still holds
/// the strike transient, whose spectrum the contact time and the felt do shape
/// with pitch. The sustain argument is sound and applies to the sustain; this
/// knob acts on the attack.
///
/// Held flat, every treble note began with a low thump the instrument does not
/// have: 19 dB over at C6 in that band, and 24 dB over relative to the note
/// itself, which is what masks a note into sounding cloudy. Graded here all six
/// notes from C2 to C7 land within 2 dB.
///
/// Two is still too steep and the earlier reading of it stands: at 2.0 the
/// model fell 31 dB across that span and the top two octaves arrived with no
/// body at all. Fifteen decibels over three octaves is five per octave, and
/// this is that slope. What the earlier round tested was zero and two, with
/// nothing between them; the value the measurement points at was never swept.
SONARE_TUNABLE_SCOPED("piano_voice", kKnockTrebleTaperOct, 1.4f);
/// Size of the blow handed to the shared board's case network at note-on, per
/// unit of the same velocity term the knock carries. Zero renders exactly as a
/// build without the path.
///
/// What the second window measures is not a spectrum at all. A grand goes on
/// radiating 60-250 Hz at a level its pitch barely moves, which is what a fixed
/// mass struck by a fixed blow gives and is not something the strings can
/// produce; the ratio climbs only because the treble string's own radiation
/// falls away underneath it. So the blow reaches the structure ungraded, and
/// the register dependence is a consequence rather than a setting.
///
/// Fitted on the sustained window over the whole keyboard, this takes that
/// error from 2.1 times the three references' own spread to 0.8 — inside it —
/// and its signed median from -15.1 dB to -0.2. It is the point where that
/// median crosses zero, which is where a note-independent quantity should be
/// fitted. The whole path is skipped when the case network is off.
///
/// The blow it is quoted against was for a long time not in the product: the
/// path carried only the knock's EXTRA velocity exponent and not the force that
/// exponent sits on top of, so a rim radiated 5.6 dB from a pianissimo to a
/// fortissimo where the blow moves 26. At this level that is inaudible and it
/// is a wall at any other — raised far enough to matter, the flat path floors
/// the soft end and the keyboard's pp-to-ff range collapses 13.7 dB, which
/// reads as a level fault in whatever was raised.
///
/// It is NOT the answer to the top octave's level, and the attempt is recorded
/// because it looked like one. C8's first fifty milliseconds sit 9.2 dB under
/// where its own keyboard puts it, 7.2 times what the three references disagree
/// by and the largest error in the voice; raising this to 0.28 closes it exactly
/// and takes C7 with it. What that costs is invisible to the eleven gated
/// dimensions, which all hold, and plain in the harmonic-to-non-harmonic
/// balance: C4 and C5's attack goes from 2 dB of the reference to 12, because
/// the blow is broadband and the middle of the keyboard did not need any. The
/// pair with a shorter kCaseT60S holds THAT and destroys the sustained field
/// instead — C8's sustain from 23 dB too harmonic to 69 — which is the decay
/// that constant is derived from doing its job. Two radiators are sharing one
/// network: a blow into the rim is over in a fraction of a second and the low
/// field it feeds rings for four, and no single level and decay is both.
SONARE_TUNABLE_SCOPED("piano_voice", kCaseStrikeGain, 0.02f);
/// The same blow into the board bank, which answers over a fraction of a
/// second where the case network answers over four. Quoted the same way as
/// kCaseStrikeGain and independent of it; zero renders exactly as a build
/// without the path.
///
/// Which of the two a blow belongs in is a question about timescale, and the
/// top octave is the only register that can answer it, because it is the only
/// one whose own note is shorter than either structure. See kCaseStrikeGain
/// for what a blow of this size does when it is spent into the long one.
///
/// Fitted on the two readings that say the top octave is too quiet, which no
/// value of the long path could satisfy together. C8's LEVEL -- the loudest
/// its body gets, against its own keyboard median -- goes from 6.2 times the
/// three references' spread to 0.5, taking the whole register dimension from
/// 1.4 to 1.3; and C8's fall to 40 dB under its own peak goes from 1.29 s to
/// 0.52 against references at 0.43 to 0.50. Everything else in the gate is
/// flat or better, and the bass and the middle of the keyboard are unmoved on
/// every window of the harmonic-to-non-harmonic balance, which is what the
/// short path buys and the long one cannot.
///
/// What it does not reach is that balance AT C8, which goes 11 dB more
/// non-harmonic than the reference. Read with the level above rather than on
/// its own: the ratio was near-perfect while both of its halves were 9 dB
/// short, so filling one breaks it. The other half is the note, and
/// kModalLevel at 0.4 puts the ratio back within 1.5 dB -- at a cost to the
/// sustained windows, which is where that thread continues.
SONARE_TUNABLE_SCOPED("piano_voice", kBoardStrikeGain, 0.12f);
/// How the board's share of the blow GROWS into the treble (doublings per
/// octave above C4). Zero is flat, which is what a blow into a structure would
/// be if the string took the same share of it everywhere.
///
/// It does not. What reaches the bridge is the momentum the string did not
/// accept, and a short stiff string accepts very little -- so the structural
/// share is the complement of an admittance that collapses at the top, and it
/// rises. Measured as each side's first fifty milliseconds of 40-320 Hz against
/// its own C4, the three references sit flat within their own spread from C5 to
/// C7 and come back up about 6 dB at C8; this voice falls monotonically and
/// arrives 15 dB short there. A flat blow sized for C8 fills the flat part too,
/// by 6 dB, which is what makes this a grading rather than a level.
///
/// One doubling per octave is where C8 lands with C5 to C7 held: at that value
/// no note between C2 and C7 moves past its own reference spread on either the
/// level or the low-band reading, and flat at the same C8 level puts 8 to 12 dB
/// of low band on the notes that already had the right amount.
SONARE_TUNABLE_SCOPED("piano_voice", kBoardStrikeTrebleOct, 1.0f);
/// The hammer-width harmonic cap keytracks from C4, signed doublings per
/// octave on each side: in HARMONIC number the felt footprint's cap follows
/// both the footprint's span of the string and how the contact dwell scales
/// against the period, so neither side is forced brighter or darker a
/// priori — the reference ladders decide the sign per register.
///
/// The bass branch is the one with a first-principles answer, because in HERTZ
/// the footprint cap is c / 2w — the transverse wave speed over twice the
/// contact width — and neither of those follows the pitch. On a grand's own
/// scale the speed falls about 2.5x from C4 to A0 and the felt widens about 2x,
/// so the corner drops ~0.7 doublings per octave while f0 drops a full one, and
/// the cap in harmonic number therefore RISES into the bass. It had been fitted
/// negative, taking A0's corner to 0.88 Hz, but only a numeric floor made that
/// survivable and the fit could never see the register it was describing.
SONARE_TUNABLE_SCOPED("piano_voice", kWidthBassOct, 0.3f);
SONARE_TUNABLE_SCOPED("piano_voice", kWidthTrebleOct, 0.81966f);
/// The footprint cap also rides the dynamics-gated felt compression, and the
/// sign of that is not obvious: compressing felt stiffens it, which passes more
/// of the pulse's top end, but it also flattens the crown and WIDENS the contact
/// patch, and a wider patch cancels LOWER partials. Fitted as a free exponent on
/// the compression factor, the reference declines to choose -- from +1 to -0.5
/// it trades the h2-h7 velocity spread against the h8-h16 one and the level
/// swing almost exactly one for one, with the total unchanged. It stays tied to
/// the stiffness factor because nothing measured says otherwise.
/// Strike-point keytrack (doublings per octave below C4) applied to the
/// patch's strike_position fraction. A grand's strike ratio travels from about
/// a twelfth of the speaking length in the middle to an eighth in the bass —
/// half a doubling across the whole bottom of the keyboard, not per octave.
/// Read per octave it carried A0's hammer out to a THIRD of the string, which
/// parks the strike comb's first peak at 46 Hz and scatters its nulls through
/// the register the note is heard in; the h8 notch it exists to place ended up
/// at h3.
SONARE_TUNABLE_SCOPED("piano_voice", kStrikePosBassOct, 0.18f);
/// The comb below is a full `1 - z^-D`, so its nulls are infinitely deep, which
/// no string has: the agraffe does not reflect perfectly, the round trip costs
/// the string's own losses, and the hammer is a patch rather than a point.
/// Attenuating the returning tap to bottom the nulls out at a finite depth was
/// measured and kept 0.6 dB of A0's ladder at a reflection of 0.5 -- a real
/// mechanism with almost no authority here, because what actually flattens the
/// reference's bass ladder is not the comb. Left as a plain difference rather
/// than as a knob that would cost a multiply per sample to do nothing.
/// Longitudinal ("phantom partial") mode bank: the first mode's frequency at
/// C4 and how it climbs per octave, the bank's level and its taper above C4,
/// and the first mode's ring-down.
///
/// A string's longitudinal modes sit at c_L / 2L, so their frequency is set by
/// the speaking length alone. Taking c_L in steel and a grand's own scale — two
/// metres at A0 through about half a metre at C4 — that is 1.3 kHz at the
/// bottom and near 4.9 kHz by the middle, a climb of 0.6 doublings per octave.
/// It is well under one because a real scale foreshortens the bass instead of
/// doubling the length every octave, which is also why the frequency has to be
/// keytracked in its own right and cannot be a multiple of f0. Fitting the
/// climb freely lands within a few per cent of the scale-length figure and
/// measurably worse either side of it, so the scale is what it is set from.
///
/// The level tapers above C4 to nothing: the treble strings are short and stiff
/// and their longitudinal modes are far above anything audible. In the bass it
/// is the opposite — with the bank absent the model had NO energy at all
/// between 200 Hz and 3 kHz in an A0 attack, 16 dB under the reference, and the
/// only lever with any authority there was the hammer knock. Fitting that
/// instead drove the same attack 43 dB OVER the reference below 60 Hz, because
/// a lowpassed force is all the knock can radiate. A bass note that is felt and
/// not heard is what a missing mode bank looks like from the fitter's side.
SONARE_TUNABLE_SCOPED("piano_voice", kLongFirstHzC4, 4900.0f);
SONARE_TUNABLE_SCOPED("piano_voice", kLongFirstOct, 0.6f);
/// The level is what the bank contributes, and it is large because the drive is
/// a squared slope: two derivatives' worth of scaling sit between the string
/// signal and this number, so it carries no meaning as a ratio.
///
/// Fitting it is a two-window problem. Against the attack it wants to be large:
/// A0 has no other source at all for 0.8-3 kHz, and every dB here is a dB of
/// the growl that tells the ear a low note came from a piano. Against the
/// sustain it wants to be small, because the bank keeps ringing and the
/// sustained centroid is already 12% over the reference. Pushed further the
/// 3-12 kHz octaves overshoot and the centroid runs away, which is the
/// scrub-noise trade over again.
///
/// It had been at eight thousand, which is on the far side of that overshoot
/// rather than at it. The drive is highpassed at kLongDriveHpHz and then
/// SQUARED, so its products land at twice the band it is driven from; measured
/// over the first 150 ms against three concert grands, the 5.7-11.3 kHz octave
/// stood 12 to 27 dB over the instrument on every note from C2 to C6. At five
/// hundred that octave lands within 3 dB across the keyboard, and it is where
/// listening stops calling the strike metallic -- one round of the same audition
/// rejected the old value by ear on that word alone.
///
/// Lowering the drive corner does NOT substitute for lowering the level, which
/// is worth recording because the arithmetic suggests it should: squaring a
/// wider band adds low products without removing the high ones, and the 8 kHz
/// excess measured slightly WORSE at every corner from 2.5 kHz down. The corner
/// stays where the scale length puts it.
SONARE_TUNABLE_SCOPED("piano_voice", kLongLevel, 500.0f);
SONARE_TUNABLE_SCOPED("piano_voice", kLongTrebleTaperOct, 1.5f);
SONARE_TUNABLE_SCOPED("piano_voice", kLongT60S, 0.35f);
/// Band limit on the slope operator the drive passes through before it is
/// squared.
///
/// The tension follows the string's SLOPE, and a transverse partial's slope
/// grows in proportion to its order — the slope operator is a differentiator.
/// A one-pole highpass IS that differentiator below its corner and flattens
/// above it, so this is where the +6 dB/octave stops rather than a filter
/// corner in the usual sense; it keeps the squaring from amplifying the top of
/// the band into hiss.
///
/// It decides the envelope, not the spectrum. The slope is carried by the upper
/// transverse partials, which die first, so a high limit concentrates the bank
/// in the attack where a phantom partial belongs. Measured: at 300 Hz the bank
/// costs 4.4 points of sustained centroid for 1.6 dB of attack; here it buys
/// 3.3 dB for 3.9 points; above about 8 kHz there is too little drive left to
/// reach at any level, which reads as an inert knob rather than as a small one.
/// A plain sample difference is the far end of the same axis and is inert for
/// the same reason: it is 38 dB down at 100 Hz, which takes the drive away in
/// exactly the register the bank exists for.
SONARE_TUNABLE_SCOPED("piano_voice", kLongDriveHpHz, 4000.0f);
/// Soundboard radiation highpass (fourth order; see kRadiationHpSections).
/// The board radiates poorly
/// below its first body modes, so a piano's low fundamentals barely reach
/// the air — the pitch is carried as virtual pitch by the upper partials.
/// Passing the raw string fundamental instead makes the note read as a
/// literally vibrating string (a guitar with its Helmholtz-supported lows),
/// an octave darker than a piano radiates.
SONARE_TUNABLE_SCOPED("piano_voice", kRadiationHpHz, 60.8f);
/// Bridge-hill radiation emphasis (RBJ peaking biquad). The bridge/board
/// mobility of a grand peaks broadly around 1-2 kHz (the "bridge hill"),
/// lifting whichever partials land in that fixed band: the bass h9-h12
/// partial crown, the mid-register presence, and the treble's h2-h3 body all
/// radiate from this resonance, not from the strings. Without it every
/// register reads mid-heavy and boxed-in regardless of the hammer spectrum.
SONARE_TUNABLE_SCOPED("piano_voice", kBridgeHillHz, 1856.4375f);
SONARE_TUNABLE_SCOPED("piano_voice", kBridgeHillGainDb, 15.863776f);
SONARE_TUNABLE_SCOPED("piano_voice", kBridgeHillQ, 2.40983f);

/// How far up the partial series the prompt-decay drain is allowed to reach,
/// in multiples of the fundamental. 0 leaves it broadband, which is where it
/// has always been.
///
/// The drain is the model of one physical thing: the vertical polarization
/// couples into the bridge and loses its energy there while the horizontal one
/// barely couples and rings on. That coupling is a property of the bridge
/// admittance around the fundamental and the first few partials. Applied
/// broadband it also drains everything above them, and what that costs grows
/// with the note, because the drain acts once per round trip and a high note
/// makes thousands of those a second. Measured at C8 the second partial falls
/// 103 dB/s faster than the fundamental where the reference holds the two
/// within a few decibels of each other for the whole note, and 56 of those
/// 103 are this: switching the drain off halves the excess on its own.
///
/// Band-limiting it does not switch the double decay off — the fundamental and
/// the low partials, which is where it was measured and where a listener hears
/// it, keep exactly the drain they had. It stops the mechanism reaching a part
/// of the spectrum it was never derived for.
///
/// Four partials is where the whole-keyboard fit puts it. Its own contribution
/// is modest, but it is what unblocks kTwoStageWidthOct: while the drain ran
/// broadband, every decibel of prompt decay came out of the bottom of the
/// spectrum, so the profile could not be widened without brightening the
/// sustain. Limited, the two move together.
SONARE_TUNABLE_SCOPED("piano_voice", kTwoStageDrainPartials, 4.0f);
/// The drain's second band, quoted where the instrument quotes it. 0 leaves
/// the drain exactly the band-limited one above.
///
/// The band limit is measured in partials of the note being played, and a
/// bridge does not know which note is driving it. Both statements can be true
/// because they describe different halves of one curve, and the half the
/// note-relative corner cannot reach is the one that matters most to a
/// listener: measured against a dry concert grand, pooled by ABSOLUTE
/// frequency over ten notes and three velocities, the reference's first half
/// second falls at 34 dB/s around 2 kHz and 31 dB/s around 3 kHz against 10
/// and 12 in the model, while its aftersound over the same partials agrees to
/// within 3 dB/s everywhere. The sustain is right and the attack is not, and
/// the missing loss is nine decibels of upper spectrum sitting through the
/// whole early body of every note -- which is what "muddy" is a description of.
///
/// A note-relative corner at four partials puts that band above the drain at
/// every pitch: a C4 stops draining at a kilohertz, a C2 at 262 Hz. So the
/// second band is taken from a one-pole at a FIXED corner and added back at
/// its own weight, which makes the drain a shelf in absolute frequency.
///
/// The weight is divided by the note's own fundamental because the drain acts
/// once per traversal while the target is quoted per second, and a note makes
/// f0 traversals a second. Without that division a weight fitted at C4 is four
/// times too strong at C6 and four times too weak at C2 -- the same register
/// runaway kLoopDampRateNorm exists to remove from the loop lowpass, arriving
/// by the same route. kBridgeHfRefHz is the note the raw weight is quoted at.
///
/// The corner lands inside the bridge hill's band (kBridgeHillHz, fitted
/// independently and from a different measurement), which is what it should do
/// if both are describing the same mobility peak: the band where the bridge
/// moves most is the band where the string loses most.
///
/// The shape is one-sided, and the same measurement asks for a lower lobe it
/// does not have. Below 120 Hz the reference's first half second falls fifteen
/// decibels a second faster than the model's, and a second fixed-corner band
/// closes that to one and a half -- while costing the spectrogram comparison
/// and the off-partial residue at every weight tried, because the model's bass
/// is already too quiet and the lower lobe's only action there is to make it
/// quieter. It is a correct mechanism whose paired defect is elsewhere, so it
/// is not here.
SONARE_TUNABLE_SCOPED("piano_voice", kBridgeHfDrain, 6.0f);
SONARE_TUNABLE_SCOPED("piano_voice", kBridgeHfHz, 1600.0f);
SONARE_TUNABLE_SCOPED("piano_voice", kBridgeHfRefHz, 261.6256f);
/// Ceiling on the second band's share of the loop gain, as a fraction. The
/// weight is a multiplier on a difference of two loop gains, so nothing in its
/// own units says how large it may become before the coherent component is
/// removed inside a single traversal; this bounds it in the units that do.
SONARE_TUNABLE_SCOPED("piano_voice", kBridgeHfDrainMax, 0.5f);
/// Design the loop's loss filter from the decay it has to produce rather than
/// from the patch's tone knob. 0 keeps the tone-derived coefficient.
///
/// The upper partials' decay is the one thing about this loop that nothing
/// states: the pole is picked for brightness, its loss at the partials is
/// whatever falls out, and that loss is charged once per traversal. What a
/// listener hears is therefore proportional to the note's own frequency, and by
/// the top of the keyboard it has run away — C8's second partial dies 103 dB/s
/// faster than its fundamental against a reference that holds the two together
/// for the whole note. The excitation is not the problem there: measured in the
/// first 20 ms that partial arrives within 4 dB of the reference and is then
/// taken apart by the loop.
SONARE_TUNABLE_SCOPED("piano_voice", kLoopSolveHf, 0.0f);
/// Where the upper-partial decay target is quoted, in Hz, and how many times
/// faster than the fundamental the partial sitting there decays.
///
/// Quoted at a fixed frequency because that is how string damping is measured,
/// and because the alternative does not work: a target quoted at the octave
/// asks a one-pole to tell apart two frequencies a third of a percent of the
/// sample rate apart in the bass, which it cannot, and the pole that finally
/// does is a brick wall four octaves up.
SONARE_TUNABLE_SCOPED("piano_voice", kLoopHfQuoteHz, 4000.0f);
SONARE_TUNABLE_SCOPED("piano_voice", kLoopHfDecayRatio, 3.0f);
/// Where the modal bank takes over from the waveguide (see the bank's own
/// commentary in the header for why there is a second synthesis path at all).
/// Two MIDI notes: below the first the loop runs alone, above the second the
/// loop is skipped entirely, and between them the two are smoothstep-blended.
///
/// The default band is where the loop stops being able to hold its own
/// filters, which is a structural fact about the delay length rather than a
/// preference: the Lagrange-3 interpolator's magnitude droop, charged once per
/// traversal, costs the second partial 0.5 dB/s at C6, 8 dB/s at C7 and 331
/// dB/s at C8, and the dispersion cascade has already been faded out by note 98
/// for want of loop to sit in. Measured against a dry concert grand the bank
/// takes C8's mean partial-ladder error from 17.0 dB to 8.9 and C7's from 15.7
/// to 12.6, at an unchanged held level.
///
/// The PARTIAL-LADDER metric prefers the bank a long way further down -- it
/// wins at every note from 72 up, by 2 to 6 dB, at a held-level cost under 1.5.
/// Do not follow it. The ladder is read on h2..h5 and is blind to what the bank
/// does above them: with no loop lowpass, and a per-partial damping that is
/// gentle by design, the modal sustain runs hot wherever a note has enough
/// partials to notice -- at F#5 the 8-16 kHz band goes from 29 dB under the
/// reference to 44 OVER it the moment the bank takes the note. In the top
/// octave there are four or five partials in total and the effect does not
/// arise; that is the whole reason the two metrics disagree.
///
/// So the crossover is left where the argument is structural rather than where
/// either metric points, and kModalDampPow measures best at its default over
/// exactly this band. The loop's per-traversal loss running away below note 92
/// is a real defect, but it is a different one with its own lever
/// (kLoopSolveHf), and it is not this bank's to fix.
SONARE_TUNABLE_SCOPED("piano_voice", kModalCrossNoteLo, 92.0f);
SONARE_TUNABLE_SCOPED("piano_voice", kModalCrossNoteHi, 98.0f);
/// How the modal bank grades a partial's decay: t60(f) = t60_slow * (f/f0)^-p.
///
/// This is the quantity the waveguide could not state. A power law in the
/// partial's own frequency ratio, because that is the shape string damping
/// measures as -- 0 gives every partial the fundamental's decay, 0.5 is the
/// air-loss-dominated t60 ~ 1/sqrt(f), 1 is t60 ~ 1/f. It replaces the loop
/// filter rather than joining it: above the crossover there is no loop and no
/// per-traversal loss to correct, so kLoopSolveHf and the brightness taper do
/// not reach these notes.
SONARE_TUNABLE_SCOPED("piano_voice", kModalDampPow, 0.25f);
/// Trim on the modal bank's output. The mode amplitudes are derived so the two
/// paths agree by construction (header), which makes 1 the meaningful default
/// and any departure from it a measurement of how well that derivation holds.
///
/// The derivation holds. Rendering the same keyboard twice with the crossover
/// forced fully to one path and then fully to the other, the bank sits a flat
/// 12.5 to 12.9 dB under the loop at the fundamental at every note from C5 to
/// C8 -- which is this trim and nothing else, so what it states is not a
/// correction but an attenuation the top octave carries for no physical reason.
SONARE_TUNABLE_SCOPED("piano_voice", kModalLevel, 0.2f);
/// How much of a struck partial's amplitude the bank leaves for the aftersound
/// once the prompt stage has run (see PianoVoice::modal_prompt_). 1 is the
/// identity: no prompt stage, which is what the bank had.
///
/// The RATE of that stage needs no constant -- it is the gap between the loop's
/// own two t60s, which start() has already computed for this note and which the
/// bank was throwing away. Only the split between the two stages is new, and it
/// cannot be taken from the loop: there the split emerges from how fast the
/// unison decoheres rather than being written down anywhere.
///
/// What the references ask for is severe. Three concert grands put C8's knee
/// 33 dB under its own peak and reach it in half a second; this voice's C8 fell
/// at 4.9 dB/s against their 65 to 68 and put its knee at 3.7 s, so the top two
/// notes outlasted every reference by a factor of two and the comparison was
/// refused rather than scored -- seven of sixty rows dropped, all of them at C7
/// and C8, on the three dimensions that describe a decay.
///
/// Below about 0.075 every one of those rows scores against all three
/// references, and below about 0.04 the aftersound starts to overshoot; this
/// sits in the middle of what is left rather than at either edge, where a
/// change elsewhere would push rows back out of the comparison. At it the body
/// under the note improves from 0.8 to 0.4 times the references' own spread,
/// the aftersound from 1.4 to 1.2, and the keyboard below C6 renders bit for
/// bit as it did.
///
/// One phrase take reaches this register and it moves both ways, for the reason
/// kTrebleDecayOct already records from the other direction: the A0-to-C8
/// sweep's tail is measured after the last note, which is C8. Its 2.5-10 kHz
/// share falls from 16 dB under the tail to 38, and its level from 76 dB under
/// the take's peak to 87 against references at 61 to 68 -- C8's over-long ring
/// had been standing in for a tail this voice does not otherwise have. Its
/// SLOPE improves over the same change, from -23.9 dB/s to -15.3 against the
/// references' -7.6 to -9.2. What the voice does have there is the case
/// network: switching kCaseLevel off takes that tail to -99.6 dB and -71.4
/// dB/s, while the sympathetic bank, the board and the reverb each leave it
/// unmoved to the digit. No other take moves by so much as a tenth of a
/// decibel.
SONARE_TUNABLE_SCOPED("piano_voice", kModalResidue, 0.06f);
/// Traversal-rate normalization of the loop lowpass (see its use in start()).
/// 0 leaves the raw per-traversal coefficient, where upper-partial damping
/// grows with the fundamental and the top two octaves lose their partial stack
/// entirely. 1 removes the register dependence completely, making the loop's
/// high-frequency loss a function of absolute frequency alone, and is the
/// default because the dependence is an artefact of the model's structure
/// rather than a property of a string. Genuine register grading of the voicing
/// belongs to kTrebleBrightPerOct / kBassDarkPerOct, which still apply. The
/// reference frequency is the note left untouched, and sits at the bottom of
/// the keyboard so the correction only ever opens the loop, never closes it.
SONARE_TUNABLE_SCOPED("piano_voice", kLoopDampRateNorm, 1.0f);
SONARE_TUNABLE_SCOPED("piano_voice", kLoopDampRefHz, 27.5f);

/// How much longer the damper takes on a quiet string, per unit of MIDI
/// velocity below the anchor. Damper felt is the same viscoelastic material as
/// hammer felt and loses energy the same way: the loss rises with how far the
/// string drives it, so a string the felt meets gently is damped gently. The
/// measured corpus carries this plainly and in one direction on all three
/// instruments in it -- a note struck at 24 takes between two and three times
/// as long to fall 40 dB after note-off as the same note struck at 120, where a
/// linear damper would take exactly as long. Fitted on the concert grand
/// (-0.0083 per velocity unit); the other two sit either side of it.
///
/// The anchor is the corpus's loudest point rather than the middle of the
/// range, because that is where the damper t60 below was fitted: at velocity
/// 120 this factor is exactly one and the voice renders as it did before.
///
/// It is the string's amplitude when the damper lands that the felt actually
/// responds to, and velocity is standing in for it. The two part company for a
/// note released long after it was struck, which by then is far quieter than
/// its velocity says -- a staccato and a note held through its whole decay get
/// the same damper here, where the real instrument damps the staccato harder.
/// The corpus cannot separate them: every note in it is held for the same eight
/// seconds, so the amplitude-at-release axis is unmeasured and a term for it
/// would be fitted to nothing.
SONARE_TUNABLE_SCOPED("piano_voice", kDamperVelSlope, 0.0083f);
SONARE_TUNABLE_SCOPED("piano_voice", kDamperVelAnchor, 120.0f);
/// Ceiling on that factor, so a pianissimo note still stops. With the fitted
/// slope the softest playable note reaches 2.7, so this never binds; it is here
/// to keep a hand-edited or swept slope from removing the damper altogether.
SONARE_TUNABLE_SCOPED("piano_voice", kDamperVelScaleMax, 4.0f);

/// Stiff-string inharmonicity B, fitted to a measured concert-grand corpus
/// (see piano_inharmonicity_b). Two branches meeting at the bass break: above
/// it a plain-wire scale grows B by ~2.8x per octave, and below it B turns
/// around and climbs back into the deep bass, because a wound bass string is
/// a heavy core the scale is too short for. The turnaround is the shape a
/// single exponential cannot express, and it is worth ~7x at the bottom note.
/// Where the dispersion cascade stops fitting the waveguide loop, and where it
/// is gone. The stiff-string law is realized by four allpass stages sitting
/// INSIDE the loop, so their phase delay comes out of the same round trip the
/// string's period has to fit in — and the period runs out first: at C8 it is
/// 11.5 samples against roughly 6.4 the cascade wants. Past that point the
/// cascade no longer realizes the law and only costs the loop, and the cost is
/// the whole note. Measured note by note it is invisible up to C7 and then
/// takes over: the ring to -40 dB at the top note is 0.75 s with the cascade
/// off and 0.30 s with it on, against about 1.5 s on the reference, while C6
/// and below are bit-identical either way.
///
/// Nothing is given up by fading it out. Inharmonicity is only audible through
/// the partials it displaces, and across the top octave the reference's own
/// partials sit 54 to 58 dB under the fundamental — there is no ladder up there
/// for a stretch to stretch, and the fundamental it is being paid for with is
/// the entire note.
SONARE_TUNABLE_SCOPED("piano_voice", kDispersionFadeNoteLo, 98.0f);
SONARE_TUNABLE_SCOPED("piano_voice", kDispersionFadeNoteHi, 108.0f);
SONARE_TUNABLE_SCOPED("piano_voice", kInharmBreakNote, 36.0f);
SONARE_TUNABLE_SCOPED("piano_voice", kInharmBAtA4, 7.718e-4f);
SONARE_TUNABLE_SCOPED("piano_voice", kInharmTrebleBeta, 0.086636f);
SONARE_TUNABLE_SCOPED("piano_voice", kInharmBassBeta, 0.064666f);

/// Railsback stretch, fitted to the same corpus (see piano_stretch_cents).
/// Two power-law branches about the A4 anchor: the treble rises as roughly
/// the fourth power of the distance in octaves, the bass falls close to
/// linearly. A tuner does not set these from the octave's second partial
/// alone -- doing so predicts about a seventh of the measured bass stretch --
/// so the curve is measured rather than derived from kInharm*.
SONARE_TUNABLE_SCOPED("piano_voice", kStretchBassCents, 2.1333f);
SONARE_TUNABLE_SCOPED("piano_voice", kStretchBassPower, 1.0756f);
SONARE_TUNABLE_SCOPED("piano_voice", kStretchTrebleCents, 0.5010f);
SONARE_TUNABLE_SCOPED("piano_voice", kStretchTreblePower, 4.0427f);

}  // namespace piano_detail

namespace {
/// Keyboard bounds the stretch and inharmonicity curves are fitted over; a note
/// outside is held at the edge, since a fourth-power stretch at note 127 asks for
/// nearly three semitones of detune.
constexpr float kLowestPianoNote = 12.0f;
constexpr float kHighestPianoNote = 108.0f;
}  // namespace

using namespace piano_detail;

float piano_inharmonicity_b(uint8_t note) noexcept {
  const float n = std::clamp(static_cast<float>(note & 0x7Fu), kLowestPianoNote, kHighestPianoNote);
  // Plain-wire branch: B grows steadily toward the top of the keyboard.
  const float treble = kInharmBAtA4 * std::exp(kInharmTrebleBeta * (n - kMidiA4));
  if (n >= kInharmBreakNote) return treble;
  // Wound-string branch below the bass break, anchored on the plain-wire value
  // at the break so the two meet without a step.
  const float at_break = kInharmBAtA4 * std::exp(kInharmTrebleBeta * (kInharmBreakNote - kMidiA4));
  return at_break * std::exp(kInharmBassBeta * (kInharmBreakNote - n));
}

int piano_unison_strings(uint8_t note) noexcept {
  const int n = static_cast<int>(note & 0x7Fu);
  if (n <= 29) return 1;  // A0..F1: single wound string.
  if (n <= 47) return 2;  // F#1..B2: wound bichords.
  return 3;               // tenor break up: plain trichords.
}

float piano_stretch_cents(uint8_t note) noexcept {
  // Two power-law branches meeting at zero on the A4 anchor. The curve is
  // asymmetric -- a real keyboard runs about ten cents flat at the bottom and
  // fifty sharp at the top -- so an odd function about A4 cannot fit it.
  const float n = std::clamp(static_cast<float>(note & 0x7Fu), kLowestPianoNote, kHighestPianoNote);
  const float octaves = (n - kMidiA4) / kSemitonesPerOctave;
  if (octaves > 0.0f) return kStretchTrebleCents * std::pow(octaves, kStretchTreblePower);
  if (octaves < 0.0f) return -kStretchBassCents * std::pow(-octaves, kStretchBassPower);
  return 0.0f;
}

}  // namespace sonare::midi::synth
