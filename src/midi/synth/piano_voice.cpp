#include "midi/synth/piano_voice.h"

#include <algorithm>
#include <cmath>

#include "midi/synth/pitch.h"
// Only for solve_string_loop_filter(): this voice keeps its own loop (it has an
// in-loop dispersion cascade and a two-stage feedback the shared one does not
// model), but the loss filter it needs is the same one, solved the same way.
#include "midi/synth/string_loop.h"
#include "midi/synth/voice_random.h"
#include "rt/fractional_delay.h"
#include "util/constants.h"
#include "util/dsp_primitives.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kPi;
using sonare::constants::kTwoPi;

/// Mezzo-forte reference velocity (0..1) the felt-hammer laws are anchored at.
SONARE_TUNABLE(kHammerMfVel, 0.6f);
/// Felt hysteresis coefficient: how much stiffer the hammer's loading curve is
/// than its unloading curve (see the contact solver's `ham_mu_`).
SONARE_TUNABLE(kHammerHysteresis, 0.229431f);
/// Additional felt-stiffness cutoff octaves per unit velocity above mf, per
/// unit hammer_dynamics: a compressed felt patch passes more of the pulse top.
SONARE_TUNABLE(kHammerDynBrightOct, 1.5f);
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
SONARE_TUNABLE(kFeltCutoffContactCycles, 4.615f);
SONARE_TUNABLE(kFeltCutoffVelOct, 0.0f);
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
SONARE_TUNABLE(kContactKeytrackSemis, 42.0f);
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
SONARE_TUNABLE(kContactPeriodsAtC4, 0.18f);
SONARE_TUNABLE(kContactPeriodsPerOct, 0.5f);
SONARE_TUNABLE(kContactPeriodsMax, 2.0f);
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
SONARE_TUNABLE(kInjImpulseNorm, 0.0f);
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
SONARE_TUNABLE(kContactPeriodsPerBlowMax, 1.0f);
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
SONARE_TUNABLE(kOutputLevel, 5.7f);
/// Where the aftersound taper starts, in octaves above C4. The taper is not a
/// slope running up from the middle of the keyboard: measured on three concert
/// grands the aftersound t60 has no trend at all from A0 to note 90, sitting
/// between 9 and 50 s across that whole span, and then falls off a cliff — 2.2 s
/// by C7 on two of the three captures and by C8 on all of them. Starting the
/// taper at C4 gave C6 a t60 of 1.6 s against a measured 11, which is a note
/// that dies while the key is still down.
SONARE_TUNABLE(kTrebleDecayKneeOct, 2.5f);
/// Halvings of the aftersound stage per octave past that knee, applied to the
/// slow stage only — the prompt-sound rate is set by the polarization/unison
/// coupling, which has its own register profile below. Steep, because what it
/// describes is a cliff rather than a slope: the measured t60 drops by a factor
/// of six inside the half-octave from note 90 to C7.
SONARE_TUNABLE(kTrebleDecayOct, 5.0f);
/// Where that cliff bottoms out, in octaves above C4. The measurement is flat
/// again past C7 (2.2 s there against 2.3 s at C8), so the taper stops instead
/// of running on and leaving the top octave a click.
SONARE_TUNABLE(kTrebleDecayFloorOct, 3.0f);
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
SONARE_TUNABLE(kTwoStageWidthOct, 2.5f);
/// Where that profile peaks, in octaves from C4. Zero is the identity and is
/// where it had always been, but only because C4 is the pivot every other
/// keytrack in this file measures from — the contrast was given the same origin
/// rather than its own, and a shared origin is not a measurement. The reference
/// puts the peak somewhere else entirely: the prompt-vs-aftersound gap is
/// weakest at C4 (-7.2 dB/s against -5.1) and sharpest at F#4 and C5 (-31 and
/// -41 against -1.9 and -2.9), so the Gaussian's crest was sitting on the one
/// note where the instrument has almost no double decay at all.
///
/// Fitted across the keyboard it lands an octave and a half up, around F#5,
/// which is further than the per-note rates alone suggest and is not a
/// contradiction of them: the profile also has to carry the treble's own short
/// decay, so with the width now the size of the instrument the peak sits where
/// the fast stage is needed most rather than where the contrast is sharpest.
/// It is the second largest single term in the voice — removing it costs 2.0 dB
/// of total error, against 11.8 for the uncombed blow and under 0.6 for
/// everything else.
SONARE_TUNABLE(kTwoStageCenterOct, 1.5f);
/// Treble taper cap (octaves above C4): the decay/darkening keytracks stop
/// steepening past here — an uncapped taper leaves the top octave with a
/// sub-100 ms husk of a note.
SONARE_TUNABLE(kTrebleTaperOctCap, 1.5f);
/// Treble loop darkening (effective-brightness drop per octave above C4): the
/// treble string's upper partials decay much faster than its fundamental, so
/// the loop lowpass closes toward the top even for a bright patch voicing.
/// Kept gentle: over-closing leaves a 3-4 partial flageolet instead of a
/// piano treble.
SONARE_TUNABLE(kTrebleBrightPerOct, 0.06f);
/// Effective-brightness drop per octave BELOW C4 (wound-string mid-partial
/// damping; see bright_eff). The h1 decay is unaffected — the loop-lowpass
/// loss at the fundamental is compensated (lp_comp), so this only shortens
/// the upper partials.
SONARE_TUNABLE(kBassDarkPerOct, 0.15f);
/// String-to-string inharmonicity spread inside a unison (fractional jitter
/// on the dispersion allpass): real unison strings never share an exact B, so
/// each partial's unison beat runs at its own rate. Identical coefficients
/// make every partial null in lockstep — an audibly artificial hollow dip.
SONARE_TUNABLE(kUnisonStiffJitter, 0.05f);
/// Under the soft pedal the action rides a softer, less-grooved felt patch that
/// compresses far less under a hard blow, so the velocity dynamics is scaled
/// down there (this also preserves the una-corda high-frequency softening).
SONARE_TUNABLE(kUnaCordaDynScale, 0.4f);
/// Uneven unison strike: hammer crowning and string leveling never deliver
/// equal energy to a bichord/trichord's strings. Equal amplitudes make the
/// unison beats cancel to full-depth nulls (an audible slow chorus wobble);
/// the uneven strike keeps them as shallow ripple and seeds the aftersound.
SONARE_TUNABLE(kUnisonStrikeUneven, 0.15f);
/// Uneven bridge coupling across the unison (Weinreich): each string meets
/// the bridge at a slightly different impedance, so the antisymmetric normal
/// mode — whose string motions cancel at an ideal bridge — still radiates,
/// at roughly this fraction of the symmetric mode. This is what makes the
/// aftersound AUDIBLE without detuning the unison out of the locked regime:
/// with equal radiation the slow mode is silent, and compensating with deep
/// detune buys the second stage at the cost of a chorus-like beating
/// fundamental no tuned piano has.
SONARE_TUNABLE(kUnisonRadSpread, 0.6f);
/// Felt impact noise: level relative to the hammer amplitude, exponential
/// decay time, and hard stop of the burst. The noise passes the same
/// velocity-driven felt-stiffness lowpass as the pulse, so soft strikes thud
/// and hard strikes click.
SONARE_TUNABLE(kStrikeNoiseGain, 0.6f);
SONARE_TUNABLE(kStrikeNoiseTauMs, 8.0f);
SONARE_TUNABLE(kStrikeNoiseMaxMs, 30.0f);
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
SONARE_TUNABLE(kStrikeNoiseCutoffScale, 0.487539f);
/// Halvings of the noise cutoff per octave below C4 (see noise_cutoff).
SONARE_TUNABLE(kNoiseCutoffBassOct, 0.5f);
/// Third noise pole, placed this factor above the main cutoff: the felt
/// noise keeps its passband but falls off a cliff past it — the reference
/// attack holds energy to a few kHz then drops ~37 dB into the next octave,
/// a shape two poles cannot make (their tail is what read as a jack click).
SONARE_TUNABLE(kNoiseSteepRatio, 4.0f);
/// Lower bound on a one-pole smoothing coefficient. A coefficient of exactly
/// zero never charges, so the pole needs a floor; the floor must sit far below
/// any corner the voice can ask for, or it silently replaces the requested
/// frequency with itself. At 48 kHz this one is 0.076 Hz. The previous 0.01
/// was 76.8 Hz, which is inside the keyboard: it pinned every note from A0 to
/// B2 to the same footprint corner regardless of pitch, so the keytrack below
/// had no authority over the bottom two octaves and could not be fitted there
/// -- the sweeps just saturated. A numeric guard that lands in the audio band
/// stops being a guard and becomes an unfittable filter.
constexpr float kOnePoleAlphaFloor = 1.0e-5f;
/// Finite hammer-head width: the felt contacts several percent of the string
/// length, and that footprint lowpasses the injected force (partials whose
/// half-wavelength fits inside the footprint cancel). Caps the pulse content
/// at about this multiple of the fundamental — without it the bass/tenor
/// overtones around h6-h12 come out plectrum-hard (a honky, dry midrange no
/// felt hammer produces). The strike-noise path is NOT capped: the scrub
/// noise is what carries the airy top sheen.
SONARE_TUNABLE(kHammerWidthHarmonics, 2.69125f);
/// Share of the impact noise injected into the strings themselves: the felt
/// scrub and the wave re-striking the hammer during contact excite the
/// string broadband. This is what seeds the HIGH partials (h10+) — the
/// smooth force pulse alone rolls off ~18 dB/oct past ~2/contact, leaving
/// the mid/bass sustain with no top: a bright strike into a dull ring reads
/// as a fingernail pluck. Random phase, so it does not re-cohere the loop.
SONARE_TUNABLE(kStrikeNoiseInject, 0.298027f);
/// Share of the strike noise that reaches the air directly, alongside the
/// share the board radiates through the knock's thud filter.
///
/// Sending all of it through the thud was a correction for raw noise leaving
/// the 3-12 kHz attack octaves 20-45 dB hot, and it overshot: the thud corner
/// keytracks to 72 Hz at A0, so in the bass the noise is lowpassed an octave
/// below the fundamental and radiates nothing at all. Measured against the
/// reference over the first 50 ms, the attack came out 10-18 dB short above
/// 3 kHz at EVERY register and 20 dB short at 0.8-3 kHz in the bass — a piano
/// whose hammers never land. The sustained partial ladder cannot see this;
/// it is measured after the attack is over.
///
/// The noise is a 30 ms event with an 8 ms decay, so this path touches the
/// attack only. What rings on is the scrub the strings are injected with,
/// which is kStrikeNoiseInject and is not affected by this.
SONARE_TUNABLE(kStrikeNoiseDirect, 0.6f);
/// The injection tapers above C4 (halvings per octave): the treble hammer
/// rests on the string for around a full period, shorting high-frequency
/// string motion at the contact point — broadband seeding there rings the
/// overtones into a harpsichord jangle instead of a piano treble.
SONARE_TUNABLE(kInjectTrebleTaperOct, 0.654102f);
/// ...and grows below C4 (doublings per octave): the massive bass hammer's
/// felt scrub and re-strike chatter seed the dense partial cloud a wound
/// string radiates (absent it, the bass is a clean plucked stack).
SONARE_TUNABLE(kInjectBassBoostOct, 1.23607f);
/// The impact-noise LEVEL also tapers above C4: the treble hammer is a few
/// grams of hard felt on a short string — its scrub is faint next to the
/// tone, where the same level against a fast-dying treble note reads as a
/// pick scratch riding every onset.
SONARE_TUNABLE(kNoiseTrebleTaperOct, 1.08754f);
/// Hammer-knock radiation (through the soundboard) relative to the string
/// injection, and its growth per octave BELOW C4: the wide wound bass
/// strings take a massive hammer whose impact drives the board directly —
/// on a reference grand the low-register attack peaks in the 60-250 Hz
/// thump, not in the string partials. Without that boom the exposed bass
/// harmonic stack reads as a harpsichord register.
SONARE_TUNABLE(kKnockGain, 2.6f);
/// The knock radiates only the impact THUD: the hammer/action/board contact
/// pumps a fixed low band regardless of the note (a treble strike lands as a
/// quiet thock, not a burst at the string's own pitch). Radiating the raw
/// pulse instead puts note-frequency energy straight into the board and
/// sympathetic modes, whose stretch-detuned ring then beats against the
/// string fundamental — an audible onset notch in the treble.
SONARE_TUNABLE(kKnockThudHz, 350.0f);
/// Halvings of the thud frequency per octave below C4 (see thud_hz).
SONARE_TUNABLE(kKnockThudBassOct, 0.7f);
/// Radiation bloom: the string radiates only through the board, whose modes
/// take time to ring up — the tone swells over tens of ms in the bass and a
/// few ms in the treble, while a plucked string (or a harpsichord jack) is
/// loudest at the very first cycle. One-pole rise time constant at C4 (ms)
/// and its per-octave keytrack (bass slower, treble faster). The knock/thud
/// path is NOT bloomed — the impact is the first thing heard.
SONARE_TUNABLE(kBloomTauMsC4, 4.6604f);
SONARE_TUNABLE(kBloomTauOct, 0.9f);
/// String yield under the blow (fraction of the hammer's speed the strike
/// point recedes at, at mf peak force). Keytracked DOWN toward the treble:
/// the treble hammer outweighs its short string many times over, so the
/// string barely loads the bounce (a clean full-period dwell); the wound
/// bass strings are massive and swing away under the light-relative hammer,
/// stretching the contact and softening the transfer.
SONARE_TUNABLE(kStringYield, 0.8f);
/// How far the strike point may be driven aside, as a fraction of THIS blow's
/// peak felt compression. Tension bounds the excursion — the strike point is a
/// sprung wave port, not a free particle — and the bound scales with the blow
/// because the compression does. A safety bound rather than a voicing control,
/// so it stays a plain constant: the fitter has nothing to gain from a limit
/// whose job is to stop the string outrunning the hammer.
constexpr float kYieldExcursionCap = 0.7f;
/// Register level compensation on the injected force (dB per octave from C4):
/// the bass chatter re-feeds its strings while the treble's near-period dwell
/// couples weakly into the fundamental, tilting the raw physical levels
/// bass-heavy by ~10 dB/oct against the reference.
SONARE_TUNABLE(kInjTiltDbOct, 1.5f);
/// How far from C4 that compensation keeps acting, in octaves, before it holds
/// flat. Both mechanisms it answers to are strongest at the ENDS of the
/// keyboard, so a span this narrow stops the correction exactly where its
/// subject is worst: past C5.25 every remaining octave gets the same fixed
/// 4.4 dB however far the raw level has drifted by then. Measured against the
/// reference the model runs 8 dB quiet at D1 and 10 dB loud at F#6 with this
/// pinned, and no lever inside the injection can reach either end.
SONARE_TUNABLE(kInjTiltOctSpan, 1.25f);
SONARE_TUNABLE(kYieldTrebleOct, 2.0f);
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
SONARE_TUNABLE(kKnockBassBoostOct, 0.3f);
/// ...and does NOT shrink above C4, though it reads as if it should: the treble
/// hammer is a few grams, so its own thud must be small, and measured against
/// the tone the reference's treble attack does sit 23-26 dB under broadband
/// where a C4 sits 16 under. That comparison is the trap. What is radiating
/// down there is not the hammer, it is the instrument -- the same body, struck
/// through the bridge, at a level the string's pitch has little say in -- so
/// the ratio moves because the tone above it changes and not because the body
/// does.
///
/// The register profile that claim rests on is the SUSTAIN, not the attack.
/// Measured on the reference over 0.4-1.4 s, where the note's own fundamental
/// has left the band, the 60-250 Hz level runs -39.6 dB at C4, -37.8 at C5,
/// -38.7 at C6, -37.7 at C7: flat to two decibels across three octaves, which
/// is what a fixed mass struck by a fixed blow gives. The first fifty
/// milliseconds are NOT flat -- the same measurement reads -9.6, -19.2, -25.0
/// and -23.1 there -- because the attack window still holds the strike
/// transient, whose spectrum the contact time and the felt do shape with pitch.
/// An earlier revision of this comment quoted an attack profile flat to five
/// decibels; no window reproduces that, and the sustain is where the constant
/// the argument needs actually lives.
///
/// Zero survives the correction anyway. Graded at 2.0 the model fell 31 dB
/// across that span and the top two octaves arrived with no body at all; held
/// flat it is what the whole-keyboard fit settles on with every other body knob
/// free to disagree. The knob stays because a real taper is not zero -- it is
/// just far smaller than a fitted-on-ratios reading makes it look, and what
/// remains of the gap is the board's, not the hammer's.
SONARE_TUNABLE(kKnockTrebleTaperOct, 0.0f);
/// How much of the knock is taken from the blow itself rather than from the
/// wave injected into the string. 0 keeps the injected copy, 1 takes the blow.
///
/// The knock is a STRUCTURE-BORNE path: the hammer landing shakes the action,
/// the frame and the rim, and that reaches the board without passing through
/// the string at all. It was nonetheless being tapped off the injected
/// excitation, which carries the strike-position comb — the near-end reflection
/// arriving back at the strike point, a property of the string the rim has no
/// way of knowing about.
///
/// The comb delay is a fixed FRACTION of the period, so what it does depends
/// entirely on the register. At C4 it is fifteen samples and its first null
/// sits at 1.6 kHz, well above the thud, which passes untouched. At C8 it is
/// ONE sample, which is a first difference: the comb becomes a differentiator
/// and takes 47 dB out at 100 Hz. That is why the model's low-band attack fell
/// 25 dB from A2 to C8 while the reference's is flat to within 5 -- the top of
/// the keyboard was not radiating a quieter body, it was radiating the
/// derivative of one.
///
/// It had been left off on a one-knob sweep, and that sweep was answering the
/// wrong question twice over.
///
/// The first error was the metric. Levels were compared with no gain alignment
/// at all, while the model's own held level ran a note-dependent 3 to 22 dB
/// over the reference, so a body level read as "too loud" that was in fact
/// deficient. Align exactly one scalar -- the mean held-level offset over the
/// keyboard, which is what a master gain can move and nothing else is -- and
/// the sign flips: switching the blow on and touching nothing else takes the
/// keyboard's low-band error from 65.3 dB to 39.8.
///
/// The second was the sweep itself. The comb's low-frequency loss goes as its
/// delay, a fixed fraction of the period, so it had been acting as an unnamed
/// register taper with the fitted gain sitting on top of it; moving the blow
/// alone measures that compensation and not the mechanism. Solved jointly with
/// the body, the envelope and the excitation it is the largest single term in
/// the voice: taking it back out costs 11.8 dB of total error against the
/// reference where the next knob costs 2.0.
///
/// The shape generalises past this knob. A mechanism the model lacks gets
/// stood in for by whatever knob is nearest, and the stand-in is then what a
/// one-at-a-time sweep is measuring.
SONARE_TUNABLE(kKnockUncombed, 1.0f);
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
SONARE_TUNABLE(kWidthBassOct, 0.3f);
SONARE_TUNABLE(kWidthTrebleOct, 0.81966f);
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
SONARE_TUNABLE(kStrikePosBassOct, 0.18f);
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
SONARE_TUNABLE(kLongFirstHzC4, 4900.0f);
SONARE_TUNABLE(kLongFirstOct, 0.6f);
/// The level is what the bank contributes, and it is large because the drive is
/// a squared slope: two derivatives' worth of scaling sit between the string
/// signal and this number, so it carries no meaning as a ratio.
///
/// Fitting it is a two-window problem. Against the attack it wants to be large:
/// A0 has no other source at all for 0.8-3 kHz, and every dB here is a dB of
/// the growl that tells the ear a low note came from a piano. Against the
/// sustain it wants to be small, because the bank keeps ringing and the
/// sustained centroid is already 12% over the reference. Where it sits recovers
/// 7.4 dB of A0's attack and 3.3 dB of the keyboard median for 3.9 points of
/// centroid, and leaves tuning, decay, the partial stack and the level swing
/// untouched. Pushed further the 3-12 kHz octaves overshoot and the centroid
/// runs away, which is the scrub-noise trade over again.
SONARE_TUNABLE(kLongLevel, 8000.0f);
SONARE_TUNABLE(kLongTrebleTaperOct, 1.5f);
SONARE_TUNABLE(kLongT60S, 0.35f);
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
SONARE_TUNABLE(kLongDriveHpHz, 4000.0f);
/// Soundboard radiation highpass (2nd order). The board radiates poorly
/// below its first body modes, so a piano's low fundamentals barely reach
/// the air — the pitch is carried as virtual pitch by the upper partials.
/// Passing the raw string fundamental instead makes the note read as a
/// literally vibrating string (a guitar with its Helmholtz-supported lows),
/// an octave darker than a piano radiates.
SONARE_TUNABLE(kRadiationHpHz, 95.0f);
/// Section Qs of the fourth-order Butterworth radiation highpass. Fixed by the
/// filter order rather than voiced: they are 1/(2 cos(pi/8)) and
/// 1/(2 cos(3pi/8)), the pole pair that makes the cascade maximally flat. The
/// corner above is what voices the radiation; changing these detunes the
/// alignment instead.
constexpr std::array<float, 2> kRadiationHpSectionQ = {0.54119610f, 1.30656296f};
/// Bridge-hill radiation emphasis (RBJ peaking biquad). The bridge/board
/// mobility of a grand peaks broadly around 1-2 kHz (the "bridge hill"),
/// lifting whichever partials land in that fixed band: the bass h9-h12
/// partial crown, the mid-register presence, and the treble's h2-h3 body all
/// radiate from this resonance, not from the strings. Without it every
/// register reads mid-heavy and boxed-in regardless of the hammer spectrum.
SONARE_TUNABLE(kBridgeHillHz, 1485.15f);
SONARE_TUNABLE(kBridgeHillGainDb, 9.91486f);
SONARE_TUNABLE(kBridgeHillQ, 2.40983f);

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
SONARE_TUNABLE(kTwoStageDrainPartials, 4.0f);
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
SONARE_TUNABLE(kLoopSolveHf, 0.0f);
/// Where the upper-partial decay target is quoted, in Hz, and how many times
/// faster than the fundamental the partial sitting there decays.
///
/// Quoted at a fixed frequency because that is how string damping is measured,
/// and because the alternative does not work: a target quoted at the octave
/// asks a one-pole to tell apart two frequencies a third of a percent of the
/// sample rate apart in the bass, which it cannot, and the pole that finally
/// does is a brick wall four octaves up.
SONARE_TUNABLE(kLoopHfQuoteHz, 4000.0f);
SONARE_TUNABLE(kLoopHfDecayRatio, 3.0f);
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
SONARE_TUNABLE(kModalCrossNoteLo, 92.0f);
SONARE_TUNABLE(kModalCrossNoteHi, 98.0f);
/// How the modal bank grades a partial's decay: t60(f) = t60_slow * (f/f0)^-p.
///
/// This is the quantity the waveguide could not state. A power law in the
/// partial's own frequency ratio, because that is the shape string damping
/// measures as -- 0 gives every partial the fundamental's decay, 0.5 is the
/// air-loss-dominated t60 ~ 1/sqrt(f), 1 is t60 ~ 1/f. It replaces the loop
/// filter rather than joining it: above the crossover there is no loop and no
/// per-traversal loss to correct, so kLoopSolveHf and the brightness taper do
/// not reach these notes.
SONARE_TUNABLE(kModalDampPow, 0.25f);
/// Trim on the modal bank's output. The mode amplitudes are derived so the two
/// paths agree by construction (header), which makes 1 the meaningful default
/// and any departure from it a measurement of how well that derivation holds.
SONARE_TUNABLE(kModalLevel, 0.2f);
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
SONARE_TUNABLE(kLoopDampRateNorm, 1.0f);
SONARE_TUNABLE(kLoopDampRefHz, 27.5f);

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
SONARE_TUNABLE(kDamperVelSlope, 0.0083f);
SONARE_TUNABLE(kDamperVelAnchor, 120.0f);
/// Ceiling on that factor, so a pianissimo note still stops. With the fitted
/// slope the softest playable note reaches 2.7, so this never binds; it is here
/// to keep a hand-edited or swept slope from removing the damper altogether.
SONARE_TUNABLE(kDamperVelScaleMax, 4.0f);

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
SONARE_TUNABLE(kDispersionFadeNoteLo, 98.0f);
SONARE_TUNABLE(kDispersionFadeNoteHi, 108.0f);
SONARE_TUNABLE(kInharmBreakNote, 36.0f);
SONARE_TUNABLE(kInharmBAtA4, 7.718e-4f);
SONARE_TUNABLE(kInharmTrebleBeta, 0.086636f);
SONARE_TUNABLE(kInharmBassBeta, 0.064666f);

/// Railsback stretch, fitted to the same corpus (see piano_stretch_cents).
/// Two power-law branches about the A4 anchor: the treble rises as roughly
/// the fourth power of the distance in octaves, the bass falls close to
/// linearly. A tuner does not set these from the octave's second partial
/// alone -- doing so predicts about a seventh of the measured bass stretch --
/// so the curve is measured rather than derived from kInharm*.
SONARE_TUNABLE(kStretchBassCents, 2.1333f);
SONARE_TUNABLE(kStretchBassPower, 1.0756f);
SONARE_TUNABLE(kStretchTrebleCents, 0.5010f);
SONARE_TUNABLE(kStretchTreblePower, 4.0427f);

/// Keyboard bounds the two curves are fitted over. A MIDI note outside them
/// is held at the edge value: extrapolating a fourth-power stretch to note
/// 127 asks for nearly three semitones of detune, and no piano has a string
/// there to justify it.
constexpr float kLowestPianoNote = 12.0f;
constexpr float kHighestPianoNote = 108.0f;

/// Per-loop-traversal amplitude factor reaching -60 dB after @p t60_s.
float loop_gain_for(float period_samples, double sample_rate, float t60_s) noexcept {
  const float loops_to_t60 =
      static_cast<float>(sample_rate) * std::max(0.01f, t60_s) / std::max(1.0f, period_samples);
  return std::exp(-6.907755279f / loops_to_t60);
}

/// Exact phase delay (samples) of the first-order allpass
/// H(z) = (a + z^-1)/(1 + a z^-1) at normalized frequency @p w.
float allpass_phase_delay(float a, float w) noexcept {
  const float sinw = std::sin(w);
  const float cosw = std::cos(w);
  const float phi = std::atan2(-sinw, a + cosw) - std::atan2(-a * sinw, 1.0f + a * cosw);
  return -phi / std::max(w, 1.0e-6f);
}

/// Phase delay (samples) of the one-pole loop lowpass y = (1-a)x + a*y^-1 at
/// normalized frequency @p w.
float onepole_phase_delay(float a, float w) noexcept { return onepole_group_delay_samples(a, w); }

/// First-order allpass coefficient a (<= 0) for a cascade of @p stages that
/// disperses the waveguide loop into the stiff-string law f_n =
/// n*f0*sqrt(1 + B*n^2). The loop resonates where the total round-trip phase
/// delay equals an integer number of periods, and only the lowpass and the
/// allpass cascade vary the phase delay with frequency, so a is solved
/// (bisection) to supply the stiff-string phase-delay differential between
/// the fundamental and a high reference partial, then clamped so the
/// per-stage delay still fits the loop budget. Endpoint-matched after Rauhala
/// & Valimaki (2006); RT-safe (bounded, allocation-free, deterministic).
float dispersion_allpass_a(float b_coeff, float w0, float lp_a, int stages,
                           float phase_budget) noexcept {
  if (b_coeff <= 0.0f || stages <= 0) return 0.0f;
  // Reference partial: high enough for a measurable differential but shrunk
  // until its stiff-string frequency sits safely below Nyquist (so the
  // treble, where B is large, still gets dispersion instead of bailing out).
  const float n_max = 0.8f * kPi / std::max(w0, 1.0e-6f);
  int n_ref = std::clamp(static_cast<int>(n_max), 2, 12);
  while (n_ref > 2 && w0 * static_cast<float>(n_ref) *
                              std::sqrt(1.0f + b_coeff * static_cast<float>(n_ref) *
                                                   static_cast<float>(n_ref)) >=
                          0.9f * kPi)
    --n_ref;
  const float fr = static_cast<float>(n_ref);
  const float w1 = w0 * std::sqrt(1.0f + b_coeff);
  const float wr = w0 * fr * std::sqrt(1.0f + b_coeff * fr * fr);
  if (wr >= 0.97f * kPi) return 0.0f;
  const float period = kTwoPi / w0;
  // Total phase-delay differential the dispersion must realize between the
  // two partials, net of the (frequency-independent) delay line.
  const float total_diff =
      period * (1.0f / std::sqrt(1.0f + b_coeff) - 1.0f / std::sqrt(1.0f + b_coeff * fr * fr));
  const float lp_diff = onepole_phase_delay(lp_a, w1) - onepole_phase_delay(lp_a, wr);
  const float need = (total_diff - lp_diff) / static_cast<float>(stages);
  if (need <= 0.0f) return 0.0f;
  // p_ap(w1;a) - p_ap(wr;a) increases monotonically as a -> -1.
  float lo = -0.999f;
  float hi = 0.0f;
  for (int it = 0; it < 40; ++it) {
    const float a = 0.5f * (lo + hi);
    const float diff = allpass_phase_delay(a, w1) - allpass_phase_delay(a, wr);
    if (diff > need)
      lo = a;
    else
      hi = a;
  }
  float a = 0.5f * (lo + hi);
  // Clamp so the per-stage phase delay at the fundamental fits the loop
  // budget (the delay line must keep a few samples).
  const float max_pap = phase_budget / static_cast<float>(stages);
  if (max_pap > 1.0f && allpass_phase_delay(a, w1) > max_pap) {
    float blo = a;
    float bhi = 0.0f;
    for (int it = 0; it < 30; ++it) {
      const float c = 0.5f * (blo + bhi);
      if (allpass_phase_delay(c, w1) > max_pap)
        blo = c;
      else
        bhi = c;
    }
    a = bhi;
  }
  return a;
}

/// Per-loop gain for a damper resting partially on the string: the decay time
/// t60 ~ -K/ln(gain) is interpolated geometrically between the natural ring
/// @p natural and the full-damper gain @p damped as the contact @p strength
/// goes 0 -> 1, so a light touch slows the ring gently while firm contact
/// reaches the full damp.
float partial_damp_gain(float natural, float damped, float strength) noexcept {
  const float ln_nat = std::log(std::max(natural, 1.0e-6f));
  const float ln_dmp = std::log(std::max(damped, 1.0e-6f));
  if (ln_nat >= -1.0e-7f) return damped;  // no natural decay: jump to the damp
  const float a = std::log(-ln_nat);
  const float b = std::log(-ln_dmp);
  return std::exp(-std::exp(a + strength * (b - a)));
}

}  // namespace

float piano_inharmonicity_b(uint8_t note) noexcept {
  const float n = std::clamp(static_cast<float>(note & 0x7Fu), kLowestPianoNote, kHighestPianoNote);
  // Plain-wire branch: B grows steadily toward the top of the keyboard.
  const float treble = kInharmBAtA4 * std::exp(kInharmTrebleBeta * (n - 69.0f));
  if (n >= kInharmBreakNote) return treble;
  // Wound-string branch below the bass break, anchored on the plain-wire value
  // at the break so the two meet without a step.
  const float at_break = kInharmBAtA4 * std::exp(kInharmTrebleBeta * (kInharmBreakNote - 69.0f));
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
  const float octaves = (n - 69.0f) / 12.0f;
  if (octaves > 0.0f) return kStretchTrebleCents * std::pow(octaves, kStretchTreblePower);
  if (octaves < 0.0f) return -kStretchBassCents * std::pow(-octaves, kStretchBassPower);
  return 0.0f;
}

void PianoVoiceCore::start(const PianoPatchParams& params, double sample_rate, uint8_t note,
                           uint8_t velocity, uint64_t seed, bool una_corda) noexcept {
  const double sr = sample_rate > 0.0 ? sample_rate : 48000.0;
  // Stretch tuning widens the octaves so the inharmonic partials lock the way
  // a tuned grand's do (sharp treble, flat bass; A4 anchored).
  const float f0 = note_to_hz(note) * std::exp2(piano_stretch_cents(note) / 1200.0f);
  const float period = static_cast<float>(sr) / f0;
  const float w0 = kTwoPi / period;
  VoiceRandomSequence jitter(seed);

  // Two-stage decay rates (stretched down the keyboard). The treble taper
  // shortens only the aftersound; the prompt stage instead blends toward the
  // aftersound rate away from the mid-range (see kTwoStageWidthOct).
  const float stretch = std::clamp(params.decay_stretch, 0.0f, 1.0f);
  // The stretch lengthens the bass and does NOTHING above A4. Read as a signed
  // exponent it also compressed the treble, and that half of it is not in the
  // instrument: measured on three concert grands the aftersound t60 has no
  // trend from A0 to note 90, so there is nothing above A4 for a compression to
  // describe. It stacked with the taper below and the two together left C8 at
  // 0.44 s against a measured 2.3 — the top of the keyboard arriving as a
  // click. Held at unity the taper alone sets the treble, and it lands 12.0 s
  // at C6 against 11.1 measured and 2.1 s at C8 against 2.3.
  const float octaves_below_a4 = std::max(0.0f, (69.0f - static_cast<float>(note & 0x7Fu)) / 12.0f);
  const float bass_scale = std::exp2(stretch * octaves_below_a4);
  // The aftersound taper rides its own octave axis, not the shared
  // `octaves_above_c4`: that one is capped where the loop DARKENING stops
  // steepening, and the decay cliff sits an octave above it. Ordered through
  // std::max so a swept knee above the floor cannot invert the clamp.
  const float decay_knee_oct = kTrebleDecayKneeOct;
  const float decay_floor_oct = std::max(decay_knee_oct, kTrebleDecayFloorOct);
  const float decay_taper_oct = std::clamp((static_cast<float>(note & 0x7Fu) - 60.0f) / 12.0f,
                                           decay_knee_oct, decay_floor_oct) -
                                decay_knee_oct;
  const float slow_scale = bass_scale * std::exp2(-kTrebleDecayOct * decay_taper_oct);
  const float t60_slow =
      std::max(0.05f, std::max(params.decay_fast_s, params.decay_slow_s) * slow_scale);

  // Loop lowpass (frequency-dependent damping), closing toward the treble.
  const float octaves_above_c4 = std::min(
      std::max(0.0f, (static_cast<float>(note & 0x7Fu) - 60.0f) / 12.0f), kTrebleTaperOctCap);
  // ...and closes into the bass as well: the wound strings' winding friction
  // damps the mid partials far faster than the plain-wire loop loss suggests.
  // Left open, the bass h4-h6 ring 3-4x longer than the reference — a bright
  // partial stack singing over the fundamental is a harpsichord register.
  const float octaves_below_c4 =
      std::max(0.0f, -(static_cast<float>(note & 0x7Fu) - 60.0f) / 12.0f);
  const float bright_eff =
      std::clamp(std::clamp(params.brightness, 0.0f, 1.0f) -
                     kTrebleBrightPerOct * octaves_above_c4 - kBassDarkPerOct * octaves_below_c4,
                 0.05f, 1.0f);
  // The loop lowpass runs once per round trip, so a fixed coefficient costs a
  // given absolute frequency a fixed number of dB PER TRAVERSAL -- and a note
  // an octave up makes twice as many traversals per second. The damping a
  // listener actually hears is therefore proportional to f0, which is not how
  // a string behaves: its losses belong to the wire and the air around it, not
  // to how often the wave happens to come round. Uncorrected, C6's second
  // partial falls 60 dB inside the first second and the treble renders as a
  // sine. Scaling the coefficient back by the traversal rate removes the
  // register dependence; the exponent sets how much of it is removed, and the
  // reference frequency is the note left untouched.
  const float rate_norm =
      std::pow(kLoopDampRefHz / std::max(f0, 1.0f), std::clamp(kLoopDampRateNorm, 0.0f, 1.0f));
  float lp_a = std::clamp((1.0f - bright_eff) * 0.6f * rate_norm, 0.0f, 0.95f);
  // Solve the pole from what the string has to DO instead of from a tone knob.
  //
  // The rate normalization above takes the register dependence out of the
  // coefficient, but the coefficient is still chosen for tone and its loss at
  // the upper partials is then whatever it happens to be — and it is applied
  // once per traversal, so a note making four thousand of them a second pays it
  // four thousand times. Measured at C8 the second partial falls 103 dB/s
  // faster than the fundamental where the reference holds the two within a few
  // decibels of each other for the whole note, and every voicing knob in the
  // engine moves that by under twenty. It is not a setting that is wrong, it is
  // a quantity nothing states.
  //
  // Stating it is a two-point filter design: the fundamental keeps the gain its
  // t60 asks for, and the partial at a FIXED quoted frequency keeps the smaller
  // one its own target asks for. Fixed rather than at some multiple of f0,
  // because string damping is measured against frequency and because a target
  // quoted at the octave is two points a fraction of a percent apart in the
  // bass — indistinguishable to one pole — while being a brick wall four
  // octaves up. Same solver the plucked cores use (string_loop.h), which found
  // the identical defect from the other end.
  if (kLoopSolveHf > 0.0f) {
    const float quote_w = kTwoPi *
                          std::clamp(kLoopHfQuoteHz, 200.0f, 0.45f * static_cast<float>(sr)) /
                          static_cast<float>(sr);
    // Above the quote frequency there is no tilt left to ask for: the note's
    // own fundamental is already past the point the target describes.
    if (quote_w > w0 * 1.5f) {
      const float ratio = std::max(1.0f, kLoopHfDecayRatio);
      const float g0 = loop_gain_for(period, sr, t60_slow);
      const float g_ref = loop_gain_for(period, sr, t60_slow / ratio);
      lp_a = std::clamp(solve_string_loop_filter(w0, quote_w, g0, g_ref).a, 0.0f, 0.95f);
    }
  }
  loop_alpha_ = 1.0f - lp_a;
  const float tau_lp = onepole_phase_delay(lp_a, w0);

  // Stiffness dispersion: the per-note inharmonicity coefficient B drives a
  // first-order allpass cascade that stretches the partials sharp to
  // f_n = n*f0*sqrt(1 + B*n^2). The patch dispersion knob scales B
  // (0 = harmonic string).
  // Faded out where the cascade no longer fits the loop (see
  // kDispersionFadeNoteLo). Smoothstepped, so the top octave loses its stretch
  // gradually rather than at one note boundary.
  const float fade_lo = kDispersionFadeNoteLo;
  const float fade_hi = std::max(fade_lo + 1.0f, kDispersionFadeNoteHi);
  const float fade_x =
      std::clamp((static_cast<float>(note & 0x7Fu) - fade_lo) / (fade_hi - fade_lo), 0.0f, 1.0f);
  const float dispersion =
      std::clamp(params.dispersion, 0.0f, 1.0f) * (1.0f - fade_x * fade_x * (3.0f - 2.0f * fade_x));
  const float b_coeff = piano_inharmonicity_b(note) * dispersion;
  const float phase_budget = period - 4.0f - tau_lp;
  const float ap_a = dispersion_allpass_a(b_coeff, w0, lp_a, kPianoDispersionStages, phase_budget);

  const float oct_from_c4_signed = (static_cast<float>(note & 0x7Fu) - 60.0f) / 12.0f;
  const float contrast_x = oct_from_c4_signed - kTwoStageCenterOct;
  const float contrast =
      std::exp(-(contrast_x * contrast_x) / (kTwoStageWidthOct * kTwoStageWidthOct));
  const float inv_fast_full = 1.0f / std::max(0.05f, params.decay_fast_s * bass_scale);
  const float inv_slow = 1.0f / t60_slow;
  const float t60_fast =
      std::min(t60_slow, 1.0f / (inv_slow + contrast * std::max(0.0f, inv_fast_full - inv_slow)));

  // The patch string count is the treble voicing; the real grand strings the
  // bass with fewer (a single wound string has no unison aftersound).
  num_strings_ =
      std::clamp(std::min(params.strings, piano_unison_strings(note)), 1, kMaxPianoStrings);
  const float spread = std::max(0.0f, params.detune_cents);
  // Uneven strike energy across the unison (seeded ramp, mean-normalized so
  // the note level is independent of the string count).
  std::array<float, kMaxPianoStrings> strike_w{};
  float strike_mean = 0.0f;
  for (int i = 0; i < num_strings_; ++i) {
    float w = 1.0f;
    if (num_strings_ > 1) {
      w -= kUnisonStrikeUneven * static_cast<float>(i) / static_cast<float>(num_strings_ - 1);
      w *= 1.0f + 0.1f * jitter.bipolar_at(static_cast<uint64_t>(16 + i));
    }
    strike_w[static_cast<size_t>(i)] = std::max(w, 0.1f);
    strike_mean += strike_w[static_cast<size_t>(i)];
  }
  strike_mean /= static_cast<float>(num_strings_);
  for (int i = 0; i < num_strings_; ++i) {
    String& s = strings_[static_cast<size_t>(i)];
    s.buffer = slab_ != nullptr ? slab_ + static_cast<size_t>(i) * string_capacity_ : nullptr;
    // Micro-detune: symmetric spread plus seeded jitter.
    float offset = 0.0f;
    if (num_strings_ > 1) {
      offset = spread * (static_cast<float>(i) / static_cast<float>(num_strings_ - 1) - 0.5f);
      offset *= 1.0f + 0.2f * jitter.bipolar_at(static_cast<uint64_t>(i));
    }
    const float detune_ratio = std::exp2(offset / 1200.0f);
    s.base_period = period / detune_ratio;
    s.strike_weight = strike_w[static_cast<size_t>(i)] / strike_mean;
    // Uneven bridge coupling (seeded ramp, mean 1 so the note level is
    // independent of the string count): lets the antisymmetric (aftersound)
    // mode radiate at the weight-difference level.
    s.radiate_weight = 1.0f;
    if (num_strings_ > 1) {
      s.radiate_weight += kUnisonRadSpread *
                          (static_cast<float>(i) / static_cast<float>(num_strings_ - 1) - 0.5f) *
                          (1.0f + 0.3f * jitter.bipolar_at(static_cast<uint64_t>(40 + i)));
    }
    // Per-string stiffness spread: decoheres the partial-by-partial unison
    // beat rates (the compensation below keeps the fundamental tuning exact).
    s.ap_a = std::clamp(
        ap_a * (1.0f + kUnisonStiffJitter * jitter.bipolar_at(static_cast<uint64_t>(24 + i))),
        -0.998f, 0.0f);
    s.ap_state.fill(0.0f);
    s.lp_state = 0.0f;
    s.write_index = 0;
    const float tau_ap = allpass_phase_delay(s.ap_a, w0);
    s.comp = 1.0f + tau_lp + static_cast<float>(kPianoDispersionStages) * tau_ap;
    // Compensate the loop lowpass's own loss at the fundamental so the patch
    // t60s stay the FUNDAMENTAL's decay; the darkened loop then only shortens
    // the upper partials (never pushed to/over unity: the LP loss is real).
    const float lp_h1_gain =
        (1.0f - lp_a) /
        std::sqrt(std::max(1.0e-9f, 1.0f - 2.0f * lp_a * std::cos(w0) + lp_a * lp_a));
    const float lp_comp = std::min(1.0f / std::max(1.0e-3f, lp_h1_gain), 1.0f / 0.9f);
    s.g_slow = std::min(0.99997f, loop_gain_for(s.base_period, sr, t60_slow) * lp_comp);
    s.g_fast = std::min(s.g_slow, loop_gain_for(s.base_period, sr, t60_fast) * lp_comp);
    s.size = std::min(string_capacity_, static_cast<int>(s.base_period * 1.3f) + 8);
    if (s.buffer != nullptr && s.size > 0) {
      std::fill(s.buffer, s.buffer + static_cast<size_t>(s.size), 0.0f);
    }
  }
  for (int i = num_strings_; i < kMaxPianoStrings; ++i) strings_[static_cast<size_t>(i)] = String{};
  bridge_ = 0.0f;
  bridge_drain_ = 0.0f;
  // Corner of the drain's band limit, as a multiple of this note's own
  // fundamental so the mechanism keeps the same reach in partials at every
  // pitch. Zero, or a corner at or above Nyquist, leaves the coefficient at 1,
  // where the filter is the identity and the drain is exactly what it was.
  const float drain_partials = std::max(0.0f, kTwoStageDrainPartials);
  const float drain_corner = drain_partials * f0;
  drain_lp_a_ = (drain_partials <= 0.0f || drain_corner >= 0.5f * static_cast<float>(sr))
                    ? 1.0f
                    : 1.0f - std::exp(-kTwoPi * drain_corner / static_cast<float>(sr));
  // Damper keytrack: slightly heavier felt on the wound bass strings, and a
  // SHORTER stop toward the treble — the treble string carries so little
  // energy that even its light damper chokes it almost immediately (reference
  // pianos stop a C5 in ~200 ms where a C4 rings ~400 ms into the felt). Flat
  // across the tenor/alto anchor (C3-C4) where the felt geometry sits closest
  // to nominal, then bends smoothly (zero slope at the anchor edges, so no
  // audible register step) into the bass and treble.
  constexpr float kAnchorLowNote = 48.0f;   // C3
  constexpr float kAnchorHighNote = 60.0f;  // C4
  constexpr float kBassSpan = 20.0f;
  constexpr float kTrebleSpan = 10.0f;
  constexpr float kBassGain = 0.4f;
  constexpr float kTrebleGain = -0.45f;
  const float note_f = static_cast<float>(note & 0x7Fu);
  float damper_keytrack = 1.0f;
  if (note_f < kAnchorLowNote) {
    const float x = std::clamp((kAnchorLowNote - note_f) / kBassSpan, 0.0f, 1.0f);
    damper_keytrack += kBassGain * x * x * (3.0f - 2.0f * x);
  } else if (note_f > kAnchorHighNote) {
    const float x = std::clamp((note_f - kAnchorHighNote) / kTrebleSpan, 0.0f, 1.0f);
    damper_keytrack += kTrebleGain * x * x * (3.0f - 2.0f * x);
  }
  // Felt loss falls off with how hard the string drives it, so a soft note is
  // damped softly (see kDamperVelSlope).
  const float damper_vel_scale = std::min(
      kDamperVelScaleMax,
      std::exp(kDamperVelSlope * (kDamperVelAnchor - static_cast<float>(velocity & 0x7Fu))));
  const float release_t60 =
      std::max(0.01f, params.release_damp_s * damper_keytrack * damper_vel_scale);
  release_gain_ = loop_gain_for(period, sr, release_t60);

  // Modal top-octave bank (see piano_voice.h). Built from the string set the
  // loop above already derived -- same periods, same detune, same strike and
  // radiation weights -- so the two paths describe one instrument and the
  // crossover has nothing to reconcile but the synthesis method.
  const float cross_lo = kModalCrossNoteLo;
  const float cross_hi = std::max(cross_lo, kModalCrossNoteHi);
  const float cross_x = cross_hi > cross_lo
                            ? std::clamp((note_f - cross_lo) / (cross_hi - cross_lo), 0.0f, 1.0f)
                            : (note_f >= cross_lo ? 1.0f : 0.0f);
  modal_mix_ = cross_x * cross_x * (3.0f - 2.0f * cross_x);
  modal_env_ = 1.0f;
  modal_damp_ = 1.0f;
  modal_release_ = std::exp(-6.907755279f / (static_cast<float>(sr) * release_t60));
  num_modal_ = 0;
  if (modal_mix_ > 0.0f) {
    // Full stiffness here, not the faded `dispersion` the loop uses: that fade
    // exists because the allpass cascade runs out of loop to sit in, and this
    // path has no loop. The top octave is where B is largest, so it is also
    // where the fade cost the most.
    const float modal_b = piano_inharmonicity_b(note) * std::clamp(params.dispersion, 0.0f, 1.0f);
    const float damp_pow = std::max(0.0f, kModalDampPow);
    const float nyq = 0.45f * static_cast<float>(sr);
    // The stiff-string law is quoted against the IDEAL string's f0, so its own
    // first partial already sits sqrt(1+B) sharp -- seven cents at C7. The loop
    // does not carry that offset: its delay compensation is taken at w0, which
    // pins the fundamental to the tuned pitch and lets the stretch accumulate
    // upward from there. Same convention here, or the top octave would arrive
    // sharp of the tuning the Railsback curve just placed it at.
    const float modal_f1 = std::sqrt(1.0f + modal_b);
    for (int i = 0; i < num_strings_ && num_modal_ < kModalModes; ++i) {
      const String& s = strings_[static_cast<size_t>(i)];
      if (s.base_period <= 1.0f) continue;
      const float sf0 = static_cast<float>(sr) / s.base_period;
      const float weight = kModalLevel * s.strike_weight * s.radiate_weight;
      for (int n = 1; n <= kModalPartials && num_modal_ < kModalModes; ++n) {
        const float fn = static_cast<float>(n) * sf0 *
                         std::sqrt(1.0f + modal_b * static_cast<float>(n) * static_cast<float>(n)) /
                         modal_f1;
        if (fn >= nyq) break;
        const float w = kTwoPi * fn / static_cast<float>(sr);
        const float t60_n =
            std::max(0.01f, t60_slow * std::pow(fn / std::max(sf0, 1.0f), -damp_pow));
        const float r = std::exp(-6.907755279f / (static_cast<float>(sr) * t60_n));
        ModalMode& m = modal_[static_cast<size_t>(num_modal_++)];
        m.a1 = 2.0f * r * std::cos(w);
        m.a2 = -r * r;
        m.gain = weight * 2.0f * std::sin(w) / s.base_period;
        m.y1 = 0.0f;
        m.y2 = 0.0f;
      }
    }
  }

  // Dynamic felt hammer (F = k * x^p with hysteretic loss), integrated per
  // sample against the string at the strike point. The felt stiffness k is
  // calibrated so a mezzo-forte blow lands the reference contact time for the
  // register; from there the Hertz velocity laws (harder+shorter with faster
  // blows), the treble's long full-period dwell and the bass re-contact
  // chatter all EMERGE from the interaction instead of being prescribed.
  const float vel01 = std::max(static_cast<float>(velocity & 0x7Fu) / 127.0f, 0.02f);
  const float p = std::clamp(params.hammer_exponent, 1.5f, 4.0f);
  const float amp_exp = 2.0f * p / (p + 1.0f);
  const float dyn =
      std::clamp(params.hammer_dynamics, 0.0f, 1.0f) * (una_corda ? kUnaCordaDynScale : 1.0f);
  // Reference contact time for the register (mf): the patch contact scaled by
  // register, floored in fundamental PERIODS (treble dwell ~ a full period).
  float contact_ms = std::clamp(params.hammer_contact_ms, 0.2f, 10.0f) *
                     std::exp2(-(static_cast<float>(note & 0x7Fu) - 69.0f) /
                               std::max(1.0f, kContactKeytrackSemis));
  const float octaves_from_c4 = (static_cast<float>(note & 0x7Fu) - 60.0f) / 12.0f;
  const float contact_floor_periods = std::clamp(
      kContactPeriodsAtC4 + kContactPeriodsPerOct * octaves_from_c4, 0.0f, kContactPeriodsMax);
  contact_ms =
      std::max(contact_ms, contact_floor_periods * 1000.0f * period / static_cast<float>(sr));
  const float tau_mf = std::max(8.0f, contact_ms * 0.001f * static_cast<float>(sr));
  // Free bounce of a unit mass on F = k * x^p from unit velocity lasts
  // c(p) * k^(-1/(p+1)) samples; c(p) fitted over p in [1.5, 4].
  const float c_p = 3.28f - 0.066f * p;
  // Felt hysteresis: loading is stiffer than unloading, which skews the force
  // pulse forward and bleeds energy so the hammer leaves the string cleanly.
  ham_p_ = p;
  ham_mu_ = kHammerHysteresis;
  ham_y_ = 0.0f;
  // Hammer speed normalized at the mezzo-forte reference; hammer_dynamics
  // widens the pp<->ff speed spread around that pivot.
  ham_v_ = std::pow(vel01 / kHammerMfVel, 1.0f + 0.6f * dyn);
  ham_on_ = true;
  ham_ttl_ = static_cast<int>(3.0f * tau_mf);  // shank check truncates a riding hammer
  // Calibrated mezzo-forte felt stiffness and the bounce it makes (unit mass):
  // peak compression x_max = ((p+1)/2k)^(1/(p+1)) and peak force k*x_max^p.
  // Both are the reference the injection normalizes against, so the velocity
  // LEVEL curve (~ v^(2p/(p+1))) comes out of the dynamics rather than out of
  // the normalization -- anything blow-dependent folded into the stiffness has
  // to stay out of these two or it divides that curve straight back out.
  // The una-corda felt patch is softer (lower k -> longer, darker contact).
  ham_k_ = std::pow(c_p / tau_mf, p + 1.0f) * (una_corda ? 0.5f : 1.0f);
  const float x_max_mf = std::pow(0.5f * (p + 1.0f) / ham_k_, 1.0f / (p + 1.0f));
  const float f_peak_mf = ham_k_ * std::pow(x_max_mf, p);
  // This blow's free-bounce contact, and the ceiling the string's reflection
  // puts on it (see kContactPeriodsPerBlowMax). Duration goes as k^(-1/(p+1)),
  // so holding it to the ceiling costs that ratio raised to p+1 in stiffness.
  // Only ham_k_ moves: the two mezzo-forte references above are the injection's
  // normalization and have to stay where they are.
  const float tau_blow = tau_mf * std::pow(std::max(ham_v_, 1.0e-4f), -(p - 1.0f) / (p + 1.0f));
  const float dwell_cap = std::max(kContactPeriodsPerBlowMax, contact_floor_periods) * period;
  ham_k_ *= std::pow(std::max(1.0f, tau_blow / std::max(dwell_cap, 1.0e-6f)), p + 1.0f);
  // Level reference: velocity-scaled for the noise/knock paths (as before);
  // the injection normalizes to the MF level so the velocity LEVEL curve
  // (~ v^(2p/(p+1))) comes out of the dynamics, not out of this constant.
  hammer_amp_ = kOutputLevel * std::pow(vel01, amp_exp) * (una_corda ? 0.8f : 1.0f);
  const float mf_level = kOutputLevel * std::pow(kHammerMfVel, amp_exp) * (una_corda ? 0.8f : 1.0f);
  ham_force_norm_ = f_peak_mf > 1.0e-12f ? mf_level / f_peak_mf : 0.0f;
  // Re-anchor from the peak onto the delivered momentum (see kInjImpulseNorm).
  // The peak-anchored norm goes as tau_mf, so dividing it by this note's contact
  // measured against C4's takes that dependence out and leaves the injection
  // carrying the same impulse in every register. Computed from the same two
  // rules the contact above uses rather than remembered, so a change to either
  // moves the anchor with it.
  const float impulse_norm = std::clamp(kInjImpulseNorm, 0.0f, 1.0f);
  if (impulse_norm > 0.0f) {
    const float period_c4 = static_cast<float>(sr) / note_to_hz(60.0f);
    const float contact_c4_ms =
        std::max(std::clamp(params.hammer_contact_ms, 0.2f, 10.0f) *
                     std::exp2(9.0f / std::max(1.0f, kContactKeytrackSemis)),
                 std::clamp(kContactPeriodsAtC4, 0.0f, kContactPeriodsMax) * 1000.0f * period_c4 /
                     static_cast<float>(sr));
    const float tau_c4 = std::max(8.0f, contact_c4_ms * 0.001f * static_cast<float>(sr));
    ham_force_norm_ *= std::pow(tau_c4 / tau_mf, impulse_norm);
  }
  const float tilt_span = std::max(0.0f, kInjTiltOctSpan);
  ham_force_norm_ *=
      std::exp2(kInjTiltDbOct * std::clamp(octaves_from_c4, -tilt_span, tilt_span) / 6.0206f);
  // String yield under the blow: the strike point recedes with a velocity
  // proportional to the net force through the string's wave admittance, and
  // the inverted reflection from the NEAR end (agraffe side, back after
  // 2 * strike_position * period = comb_delay_ samples) cancels that recess
  // — the felt is recompressed and the measured multi-hump piano force
  // curve (and the treble's full-period dwell) emerges.
  const float yield_kt =
      kStringYield * std::exp2(-kYieldTrebleOct * std::max(0.0f, octaves_from_c4));
  // The admittance is the STRING's (force in, transverse velocity out), so it
  // does not depend on the blow. The excursion the strike point can reach does:
  // it follows this blow's peak felt compression, which is read off the
  // stiffness the blow actually ran on rather than the mezzo-forte one, so a
  // blow held to the dwell ceiling compresses the felt less and drives the
  // string aside less in the same proportion. Pinning the excursion to the mf value
  // instead makes the string effectively RIGID above mezzo-forte, so the felt
  // absorbs the whole of a fortissimo blow and the contact shortens even
  // further than the free-bounce law alone would give.
  const float x_max_unit = std::pow(0.5f * (p + 1.0f) / ham_k_, 1.0f / (p + 1.0f));
  const float x_max_v = x_max_unit * std::pow(std::max(ham_v_, 1.0e-4f), 2.0f / (p + 1.0f));
  ys_adm_ = 0.5f * yield_kt * x_max_mf;
  ys_limit_ = kYieldExcursionCap * x_max_v;
  ys_ = 0.0f;
  last_force_ = 0.0f;
  ham_exit_ = -x_max_v;
  // The strike point moves out toward 1/8 of the speaking length on the bass
  // strings (mid/treble sits nearer 1/12): the 1/8 node notches h8 right
  // below the bridge-hill crown — the reference bass ladder's signature dip.
  const float strike_pos = std::clamp(params.strike_position, 0.0f, 0.5f) *
                           std::exp2(kStrikePosBassOct * std::max(0.0f, -octaves_from_c4));
  comb_delay_ = static_cast<int>(std::min(strike_pos, 0.5f) * period + 0.5f);
  comb_delay_ = std::min(comb_delay_, kHammerCombCapacity - 1);
  comb_idx_ = 0;
  comb_tail_ = 0;
  comb_hist_.fill(0.0f);
  noise_hist_.fill(0.0f);
  // Felt stiffening: compressed felt (hard strike) passes far more of the
  // pulse's top end — a velocity-driven one-pole on the injected force. The
  // dynamics-gated brightening also scales the footprint cap: compression
  // flattens the felt crown, so a hard strike's effective contact patch
  // passes higher partials (without this the width cap, which sits below the
  // stiffness cutoff at normal velocities, swallows the whole pp<->ff
  // brightness spread).
  const float dyn_bright = std::exp2(kHammerDynBrightOct * dyn * (vel01 - kHammerMfVel));
  const float exc_cutoff = kFeltCutoffContactCycles / std::max(1.0e-4f, contact_ms * 0.001f) *
                           std::exp2(kFeltCutoffVelOct * vel01) * dyn_bright *
                           (una_corda ? 0.4f : 1.0f);
  const float width_harm =
      kHammerWidthHarmonics * std::exp2(kWidthBassOct * std::max(0.0f, -octaves_from_c4) +
                                        kWidthTrebleOct * std::max(0.0f, octaves_from_c4));
  const float width_cutoff = std::min(exc_cutoff, width_harm * f0 * dyn_bright);
  exc_alpha_ = std::clamp(1.0f - std::exp(-kTwoPi * width_cutoff / static_cast<float>(sr)),
                          kOnePoleAlphaFloor, 1.0f);
  // The noise cutoff keytracks DOWN into the bass: the bass hammer is a
  // massive deep-felt head whose scrub spectrum is far darker than the small
  // hard treble hammer's — without this the bass attack carries the same
  // 2 kHz-wide burst as the treble and reads as a jack click.
  const float noise_cutoff = kStrikeNoiseCutoffScale * exc_cutoff *
                             std::exp2(-kNoiseCutoffBassOct * std::max(0.0f, -octaves_from_c4));
  noise_alpha_ = std::clamp(1.0f - std::exp(-kTwoPi * noise_cutoff / static_cast<float>(sr)),
                            kOnePoleAlphaFloor, 1.0f);
  noise_alpha3_ = std::clamp(
      1.0f - std::exp(-kTwoPi * kNoiseSteepRatio * noise_cutoff / static_cast<float>(sr)),
      kOnePoleAlphaFloor, 1.0f);
  exc_lp_ = 0.0f;
  exc_lp2_ = 0.0f;
  body_lp_ = 0.0f;
  body_lp2_ = 0.0f;

  // Felt impact noise: a short broadband burst radiated with the knock (the
  // soft una-corda felt lands with far less impact noise than the grooved
  // normale surface).
  // The impact noise is part of the same blow — it rides the injection tilt
  // and the dynamics-gated felt compression (a compressed crown scrubs
  // louder, a soft blow on open felt barely rustles).
  noise_env_ = kStrikeNoiseGain * hammer_amp_ * dyn_bright * (una_corda ? 0.35f : 1.0f) *
               std::exp2(-kNoiseTrebleTaperOct * std::max(0.0f, octaves_from_c4) +
                         kInjTiltDbOct * std::clamp(octaves_from_c4, -1.25f, 1.25f) / 6.0206f);
  knock_gain_ = kKnockGain * std::exp2(kKnockBassBoostOct * std::max(0.0f, -octaves_from_c4) -
                                       kKnockTrebleTaperOct * std::max(0.0f, octaves_from_c4));
  knock_lp_ = 0.0f;
  knock_lp2_ = 0.0f;
  knock_lp3_ = 0.0f;
  // The thud deepens and slows into the bass: the massive bass hammer rocks
  // the whole board, a boom that takes ~10 ms to develop — an instantaneous
  // bass onset is a jack pluck, not a hammer landing.
  const float thud_hz =
      kKnockThudHz * std::exp2(-kKnockThudBassOct * std::max(0.0f, -octaves_from_c4));
  knock_lp_a_ = std::clamp(1.0f - std::exp(-kTwoPi * thud_hz / static_cast<float>(sr)), 0.0f, 1.0f);
  knock_lp3_a_ = std::clamp(
      1.0f - std::exp(-kTwoPi * kNoiseSteepRatio * thud_hz / static_cast<float>(sr)), 0.0f, 1.0f);
  bloom_ = 0.0f;
  const float bloom_tau_s = kBloomTauMsC4 * 0.001f * std::exp2(-kBloomTauOct * octaves_from_c4);
  bloom_a_ =
      std::clamp(1.0f - std::exp(-1.0f / (bloom_tau_s * static_cast<float>(sr))), 1.0e-4f, 1.0f);
  // Longitudinal mode bank. The input carries a DC/Nyquist zero (see render),
  // so each mode is exactly peak-normalized off the bandpass residue the way
  // the soundboard's are: two-pole modes taken raw pile their low-frequency
  // skirts up in phase, which here would put the squared drive's DC straight
  // back into the bass the bank exists to clear.
  long_level_ = kLongLevel * std::exp2(-kLongTrebleTaperOct * std::max(0.0f, octaves_from_c4));
  const float long_f1 = kLongFirstHzC4 * std::exp2(kLongFirstOct * octaves_from_c4);
  long_prev_ = 0.0f;
  long_hp_a_ =
      std::clamp(1.0f - std::exp(-kTwoPi * kLongDriveHpHz / static_cast<float>(sr)), 0.0f, 1.0f);
  long_x1_ = 0.0f;
  long_x2_ = 0.0f;
  for (int i = 0; i < kLongitudinalModes; ++i) {
    LongMode& m = long_modes_[static_cast<size_t>(i)];
    m = LongMode{};
    const float f = long_f1 * static_cast<float>(i + 1);
    if (long_level_ <= 0.0f || f >= 0.45f * static_cast<float>(sr)) continue;
    const float w = kTwoPi * f / static_cast<float>(sr);
    // The higher modes are lossier, as they are on the transverse side.
    const float t60 = std::max(0.01f, kLongT60S / static_cast<float>(i + 1));
    const float r = std::exp(-6.907755279f / (static_cast<float>(sr) * t60));
    m.a1 = 2.0f * r * std::cos(w);
    m.a2 = -r * r;
    const float d_re = 1.0f - m.a1 * std::cos(w) - m.a2 * std::cos(2.0f * w);
    const float d_im = m.a1 * std::sin(w) + m.a2 * std::sin(2.0f * w);
    const float d_mag = std::sqrt(d_re * d_re + d_im * d_im);
    // Peak-normalized, then rolled off along the series: the higher
    // longitudinal modes are both less excited and lossier, and left at equal
    // peaks the bank reads as a bright metallic ring rather than as the body
    // of a low note.
    m.gain = d_mag / std::max(2.0f * std::sin(w), 1.0e-6f) / static_cast<float>(i + 1);
  }
  noise_decay_ = std::exp(-1000.0f / (kStrikeNoiseTauMs * static_cast<float>(sr)));
  noise_samples_ = static_cast<int>(kStrikeNoiseMaxMs * 0.001f * sr);
  noise_pos_ = 0;
  noise_lp_ = 0.0f;
  noise_lp2_ = 0.0f;
  noise_lp3_ = 0.0f;
  noise_low_ = 0.0f;
  // The string-injected share is highpassed above the fundamental: the scrub
  // noise seeds the upper partials at random phase (the desired sheen), but a
  // random-phase component ON h1 vector-cancels against the coherent pulse —
  // an audible amplitude notch a few tens of ms into the note.
  noise_hp_a_ =
      std::clamp(1.0f - std::exp(-kTwoPi * 1.2f * f0 / static_cast<float>(sr)), 0.0f, 1.0f);
  noise_rng_ = static_cast<uint32_t>(seed ^ (seed >> 32) ^ 0x9E3779B9u) | 1u;
  // Below C4 the injection GROWS instead: the massive bass hammer's felt
  // scrub and re-strike chatter seed the dense h8-h20 partial cloud a wound
  // string radiates (absent it, the bass is a clean plucked stack).
  noise_inject_ =
      kStrikeNoiseInject * std::exp2(-kInjectTrebleTaperOct * std::max(0.0f, octaves_from_c4) +
                                     kInjectBassBoostOct * std::max(0.0f, -octaves_from_c4));

  // Radiation highpass coefficients (cascaded RBJ highpasses) and state. The
  // section Qs are the fourth-order Butterworth pair, so the passband stays
  // flat through the tenor while the stopband falls at the measured slope.
  {
    const float w = kTwoPi * kRadiationHpHz / static_cast<float>(sr);
    const float cw = std::cos(w);
    const float sw = std::sin(w);
    for (int i = 0; i < kRadiationHpSections; ++i) {
      HpSection& s = hp_[static_cast<size_t>(i)];
      const float alpha = sw / (2.0f * kRadiationHpSectionQ[static_cast<size_t>(i)]);
      const float a0 = 1.0f + alpha;
      s.b0 = (1.0f + cw) * 0.5f / a0;
      s.b1 = -(1.0f + cw) / a0;
      s.a1 = -2.0f * cw / a0;
      s.a2 = (1.0f - alpha) / a0;
      s.x1 = s.x2 = s.y1 = s.y2 = 0.0f;
    }
  }

  // Bridge-hill emphasis coefficients (RBJ peaking) and state.
  {
    const float big_a = std::pow(10.0f, kBridgeHillGainDb / 40.0f);
    const float w = kTwoPi * kBridgeHillHz / static_cast<float>(sr);
    const float cw = std::cos(w);
    const float alpha = std::sin(w) / (2.0f * kBridgeHillQ);
    const float a0 = 1.0f + alpha / big_a;
    bh_b0_ = (1.0f + alpha * big_a) / a0;
    bh_b1_ = -2.0f * cw / a0;
    bh_b2_ = (1.0f - alpha * big_a) / a0;
    bh_a1_ = -2.0f * cw / a0;
    bh_a2_ = (1.0f - alpha / big_a) / a0;
    bh_x1_ = bh_x2_ = bh_y1_ = bh_y2_ = 0.0f;
  }
}

float PianoVoiceCore::render(float pitch_ratio) noexcept {
  if (num_strings_ <= 0 || slab_ == nullptr) return 0.0f;

  // Dynamic hammer: integrate the felt mass against the string's arrival at
  // the strike point, then comb the force by the strike position and pass the
  // velocity-driven felt-stiffness lowpass.
  float exc = 0.0f;
  float knock = 0.0f;
  float thud_in = 0.0f;
  float noise_direct = 0.0f;
  float force = 0.0f;
  if (ham_on_) {
    // String surface velocity at the strike point: the string recedes under
    // the net force through its wave admittance (the soft-string effect that
    // stretches the treble dwell and caps the energy transfer). The near-end
    // reflection is applied to the INJECTED wave by the strike-position comb
    // below; coupling it back into the felt as well — so the returning wave
    // pushes the string into the hammer and ends the contact on the string's
    // timescale rather than the felt's — measures as no change at all, in the
    // partial balance, in the pp<->ff spread, or in the treble dwell it would
    // most be expected to reach, so the one-way form stands.
    const float ys_vel = ys_adm_ * last_force_;
    // Tension bounds the excursion: the strike point is a sprung wave port,
    // not a free particle — without the cap the string outruns the hammer
    // and the bounce never completes (a glued, energyless contact). The bound
    // scales with the blow, because this blow's peak felt compression does.
    ys_ = std::min(ys_ + ys_vel, ys_limit_);
    const float x = ham_y_ - ys_;
    if (x > 0.0f) {
      const float xdot = ham_v_ - ys_vel;
      force = ham_k_ * std::pow(x, ham_p_) * (1.0f + ham_mu_ * xdot);
      force = std::max(force, 0.0f);
      ham_v_ -= force;
    }
    ham_y_ += ham_v_;
    if ((x <= 0.0f && ham_v_ < 0.0f && ham_y_ - ys_ < ham_exit_) || --ham_ttl_ <= 0) {
      ham_on_ = false;  // thrown clear (or shank recovery timeout)
      comb_tail_ = comb_delay_;
    }
  }
  if (ham_on_ || comb_tail_ > 0) {
    if (!ham_on_) --comb_tail_;
    const float tap = comb_hist_[static_cast<size_t>(
        (comb_idx_ - comb_delay_ + kHammerCombCapacity) % kHammerCombCapacity)];
    comb_hist_[static_cast<size_t>(comb_idx_)] = force;
    comb_idx_ = (comb_idx_ + 1) % kHammerCombCapacity;
    const float combed = ham_force_norm_ * (force - tap);
    // Two-pole felt lowpass: the footprint is a spatial window over the
    // string, whose transmission falls ~12 dB/oct past the cap — a single
    // pole leaves the mid harmonics plectrum-bright at every register.
    exc_lp_ += exc_alpha_ * (combed - exc_lp_);
    exc_lp2_ += exc_alpha_ * (exc_lp_ - exc_lp2_);
    exc = exc_lp2_ / static_cast<float>(num_strings_);
    // The body's copy of the same blow, softened by the same felt but never
    // combed (see kKnockUncombed). Written as a lerp off the combed path so
    // the transparent coefficient is exactly the value that path already had.
    const float raw = ham_force_norm_ * force;
    body_lp_ += exc_alpha_ * (raw - body_lp_);
    body_lp2_ += exc_alpha_ * (body_lp_ - body_lp2_);
    thud_in = exc_lp2_ + kKnockUncombed * (body_lp2_ - exc_lp2_);
    last_force_ = force;
  }
  if (noise_pos_ < noise_samples_) {
    ++noise_pos_;
    noise_rng_ = noise_rng_ * 1664525u + 1013904223u;
    const float white = static_cast<float>(noise_rng_ >> 8) * (1.0f / 8388608.0f) - 1.0f;
    // Two-pole felt-noise lowpass, and the noise radiates ONLY through the
    // knock's thud filter (below): a felt hammer puts almost nothing above a
    // few kHz into the air at normal dynamics. The previous one-pole noise
    // radiated raw left the 3-12 kHz attack octaves 20-45 dB hotter than the
    // reference — a plectrum/jack click the ear keys on as "plucked string"
    // no matter how accurate the sustain is.
    noise_lp_ += noise_alpha_ * (white - noise_lp_);
    noise_lp2_ += noise_alpha_ * (noise_lp_ - noise_lp2_);
    noise_lp3_ += noise_alpha3_ * (noise_lp2_ - noise_lp3_);
    const float noise = noise_env_ * noise_lp3_;
    noise_env_ *= noise_decay_;
    thud_in += noise;
    // The direct path is tapped two poles in, not three: the third pole belongs
    // to the contact footprint, which is what the STRING is injected through,
    // and the air does not hear the strike through the string's window. Tapped
    // at the same point the direct path carries nothing above a few hundred
    // hertz and cannot fill the attack it exists to fill.
    //
    // Two, not one. A single pole falls at 6 dB/octave, which no radiating
    // mechanism does and which leaves the burst still 23 dB up at 16 kHz —
    // exactly the plectrum click the cascade above was widened to two poles to
    // remove, arriving by the other route. Measured against the dry concert
    // grand a one-pole tap ran +32 dB at 12-20 kHz and +53 dB at 20-24 kHz over
    // the first 43 ms of every note, on top of a midband that already matched
    // inside half a dB; the ear reads that as a tick on the note, not as
    // brightness. Two poles put the same corner 45 dB down at 16 kHz against a
    // measured 44, and cost nothing — the tap already exists for the cascade.
    noise_direct = noise_env_ * noise_lp2_;
    noise_low_ += noise_hp_a_ * (noise - noise_low_);
    // The scrub noise is generated AT the strike point, so it sees the same
    // near-end reflection as the force pulse: comb it by the strike position
    // (its own history — the force comb carries different units/lifetime).
    // Without this the noise fills in the comb's h8-region notch, the
    // reference bass ladder's signature dip.
    const float scrub = noise_inject_ * (noise - noise_low_);
    const size_t widx = static_cast<size_t>((noise_pos_ - 1) % kHammerCombCapacity);
    noise_hist_[widx] = scrub;
    const int64_t tap_i = noise_pos_ - 1 - comb_delay_;
    const float tap =
        tap_i >= 0 ? noise_hist_[static_cast<size_t>(tap_i % kHammerCombCapacity)] : 0.0f;
    exc += (scrub - tap) / static_cast<float>(num_strings_);
  }
  // Three-pole thud filter: the knock is a LOW-frequency body event; fewer
  // poles leak the excitation's top octaves into the radiated attack, and
  // the bass knock gain amplifies that leak into an audible jack click.
  knock_lp_ += knock_lp_a_ * (thud_in - knock_lp_);
  knock_lp2_ += knock_lp_a_ * (knock_lp_ - knock_lp2_);
  knock_lp3_ += knock_lp3_a_ * (knock_lp2_ - knock_lp3_);
  knock += knock_gain_ * knock_lp3_ + kStrikeNoiseDirect * noise_direct;

  const float ratio = pitch_ratio > 0.01f ? pitch_ratio : 0.01f;
  float sum = 0.0f;
  float lp_sum = 0.0f;
  // Fully past the crossover the loop contributes nothing, so it is not run:
  // the modal bank is meant to be the cheaper path up there, and leaving four
  // allpasses and an interpolator per string spinning behind a zero weight
  // would spend the saving it exists to make. `bridge_` then falls to zero on
  // its own, which is what a voice with no loop should hand the drain.
  const int loop_strings = modal_mix_ < 1.0f ? num_strings_ : 0;
  for (int i = 0; i < loop_strings; ++i) {
    String& s = strings_[static_cast<size_t>(i)];
    if (s.buffer == nullptr || s.size < 8) continue;
    // Coupled two-stage decay: the coherent (bridge) component recirculates
    // at the fast prompt rate, the residual at the slow aftersound rate.
    const float fb = s.g_slow * s.lp_state - (s.g_slow - s.g_fast) * bridge_drain_;
    const float delay =
        std::clamp(s.base_period / ratio - s.comp, 1.0f, static_cast<float>(s.size - 4));
    const float out = rt::lagrange3_fractional_delay(
        s.buffer, static_cast<size_t>(s.size), s.write_index, static_cast<int>(delay * 256.0f),
        exc * s.strike_weight + fb);
    // Dispersion allpass cascade then the loop lowpass.
    float v = out;
    for (float& state : s.ap_state) {
      const float y = s.ap_a * v + state;
      state = v - s.ap_a * y;
      v = y;
    }
    s.lp_state += loop_alpha_ * (v - s.lp_state);
    lp_sum += s.lp_state;
    sum += out * s.radiate_weight;
  }
  bridge_ = lp_sum / static_cast<float>(num_strings_);
  // Band-limited copy for the drain only. `bridge_` itself stays broadband:
  // it is the coherent bridge signal and other things read it as such.
  //
  // Written as a lag off the input rather than as the usual `y += a * (x - y)`,
  // which at a == 1 is `y + (x - y)` and is NOT x in floating point. The two
  // differ in the last bit, and one last bit inside a feedback loop running
  // four thousand times a second is not a rounding difference for long. This
  // form is exactly `bridge_` at the transparent coefficient.
  bridge_drain_ = bridge_ - (1.0f - drain_lp_a_) * (bridge_ - bridge_drain_);
  // Modal top-octave bank: the same hammer excitation, resolved into partials.
  // It stands in for the string's radiated wave and nothing else -- knock,
  // scrub noise, longitudinal bank, board and bridge hill all read the blended
  // `sum` exactly as they read the loop's.
  if (num_modal_ > 0) {
    float modal_sum = 0.0f;
    for (int i = 0; i < num_modal_; ++i) {
      ModalMode& m = modal_[static_cast<size_t>(i)];
      const float y = m.gain * exc + m.a1 * m.y1 + m.a2 * m.y2;
      m.y2 = m.y1;
      m.y1 = y;
      modal_sum += y;
    }
    modal_env_ *= modal_damp_;
    sum += (modal_sum * modal_env_ - sum) * modal_mix_;
  }
  // Board ring-up: the tone swells while the impact thud leads.
  bloom_ += bloom_a_ * (1.0f - bloom_);
  // Longitudinal modes, driven by the tension the transverse motion itself
  // makes. Squaring the string sum IS the tension term, so the v^2 amplitude
  // law and the doubled decay rate come out of it rather than being written
  // down; the two-sample difference puts a zero at DC and at Nyquist, without
  // which the squared drive's large DC component would be re-radiated as the
  // very bass rumble this bank is here to replace.
  float longitudinal = 0.0f;
  if (long_level_ > 0.0f) {
    // The tension follows the string's SLOPE, not its displacement, so the
    // drive is differenced before it is squared. It matters for the envelope
    // rather than the spectrum: the slope is carried by the upper transverse
    // partials, which die first, so a differenced drive concentrates the bank
    // in the attack the way a real phantom partial is. Squaring the radiated
    // sum instead keeps the bank ringing for as long as the fundamental does,
    // which measured 13 points brighter than the reference over the sustain
    // at the level the attack needs.
    long_prev_ += long_hp_a_ * (sum - long_prev_);
    const float d = sum - long_prev_;
    const float t = d * d;
    const float bp = t - long_x2_;
    long_x2_ = long_x1_;
    long_x1_ = t;
    for (LongMode& m : long_modes_) {
      const float y = m.gain * bp + m.a1 * m.y1 + m.a2 * m.y2;
      m.y2 = m.y1;
      m.y1 = y;
      longitudinal += y;
    }
    longitudinal *= long_level_;
  }
  sum = sum * bloom_ + knock + longitudinal;
  // Soundboard radiation: the board barely radiates the lowest partials.
  float y = sum;
  for (HpSection& s : hp_) {
    const float in = y;
    y = s.b0 * in + s.b1 * s.x1 + s.b0 * s.x2 - s.a1 * s.y1 - s.a2 * s.y2;
    s.x2 = s.x1;
    s.x1 = in;
    s.y2 = s.y1;
    s.y1 = y;
  }
  // Bridge hill: the fixed-band mobility peak lifts whatever partials land
  // near it (bass crown, mid presence, treble body).
  const float z =
      bh_b0_ * y + bh_b1_ * bh_x1_ + bh_b2_ * bh_x2_ - bh_a1_ * bh_y1_ - bh_a2_ * bh_y2_;
  bh_x2_ = bh_x1_;
  bh_x1_ = y;
  bh_y2_ = bh_y1_;
  bh_y1_ = z;
  return z;
}

void PianoVoiceCore::release() noexcept {
  for (int i = 0; i < num_strings_; ++i) {
    String& s = strings_[static_cast<size_t>(i)];
    s.g_slow = std::min(s.g_slow, release_gain_);
    s.g_fast = std::min(s.g_fast, release_gain_);
  }
  // The modal bank's damper multiplies the modes' own decay instead of
  // capping it. Decay RATES add when a second loss mechanism is introduced,
  // which is what a damper is; the loop's min() is a cap only because a
  // per-traversal gain has no term to add to.
  modal_damp_ = std::min(modal_damp_, modal_release_);
}

void PianoVoiceCore::damp(float strength) noexcept {
  strength = std::clamp(strength, 0.0f, 1.0f);
  if (strength <= 0.0f) return;
  if (strength >= 1.0f) {
    release();
    return;
  }
  for (int i = 0; i < num_strings_; ++i) {
    String& s = strings_[static_cast<size_t>(i)];
    s.g_slow = std::min(s.g_slow, partial_damp_gain(s.g_slow, release_gain_, strength));
    s.g_fast = std::min(s.g_fast, partial_damp_gain(s.g_fast, release_gain_, strength));
  }
  // Partial contact scales the ADDED rate, so a half-resting damper removes
  // half the energy per second the full one does. partial_damp_gain's
  // geometric rule cannot be reused here: it interpolates between two finite
  // t60s, and the modal bank's undamped end is an infinite one.
  // Ratcheted like the loop's gains: a damper that has touched the string is
  // not lifted off it by a later, lighter call.
  modal_damp_ = std::min(modal_damp_, std::pow(modal_release_, strength));
}

void PianoVoiceCore::kill() noexcept {
  for (String& s : strings_) s = String{};
  num_strings_ = 0;
  hammer_amp_ = 0.0f;
  ham_on_ = false;
  noise_pos_ = 0;
  noise_samples_ = 0;
  noise_env_ = 0.0f;
  noise_lp_ = 0.0f;
  noise_lp2_ = 0.0f;
  noise_lp3_ = 0.0f;
  body_lp_ = 0.0f;
  body_lp2_ = 0.0f;
  long_level_ = 0.0f;
  long_prev_ = 0.0f;
  long_x1_ = 0.0f;
  long_x2_ = 0.0f;
  for (LongMode& m : long_modes_) m = LongMode{};
  num_modal_ = 0;
  modal_mix_ = 0.0f;
  modal_env_ = 1.0f;
  modal_damp_ = 1.0f;
}

}  // namespace sonare::midi::synth
