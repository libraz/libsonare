#include <algorithm>
#include <cmath>

#include "midi/synth/piano_voice.h"
#include "midi/synth/pitch.h"
#include "util/constants.h"
#include "util/tunable.h"

namespace sonare::midi::synth {

namespace {

using sonare::constants::kTwoPi;

/// Sympathetic-resonance gate floor with the dampers down: the dampers only
/// rest on the speaking lengths, so the duplex/aliquot segments beyond the
/// bridge and the undamped top octaves keep a faint shimmer ringing even with
/// the pedal up. Without this floor the pedal-up sustain is a spectrally bare
/// harmonic stack — it reads as a plucked string, not a whole instrument.
SONARE_TUNABLE(kDuplexFloor, 0.3f);

/// The permanently undamped treble: coupling level, where the dampers stop,
/// and how long those strings ring. Level 0 removes the population entirely
/// and is the identity.
///
/// A grand's damper felts run out partway up the treble; from there to the top
/// every string is free whether or not a foot is on the pedal. So with the
/// pedal UP -- which is how nearly everything is played, and how the reference
/// was captured -- the only strings that can answer another note are those,
/// and the damped register the bank had been tuned to (E1..E6, every four
/// semitones) is exactly the part of the instrument that is held silent.
///
/// It is not a subtlety. Strike C8 on the reference and look below its
/// fundamental, where the string it belongs to cannot radiate: a second later
/// the 1.15-3.4 kHz band holds a row of discrete peaks on CONSECUTIVE
/// semitones -- notes 88, 90, 91, 92, 93, 94, 95, 96, 97 -- each a few cents
/// sharp of equal temperament, which is a tuner's stretch and not a
/// coincidence. A soundboard answers with a wash; that is a set of strings.
/// The same row appears under C7 from note 83 up. The model radiated nothing
/// there, and no amount of fitting could produce it, because a resonator that
/// is not in the bank has no knob.
///
/// The decay is measured off the same band: it falls 22.7 dB between the
/// 0.5-1.5 s and 2.5-3.5 s windows on C8 and 23.1 dB on C7, which is a t60 of
/// about five seconds. Longer than a struck treble note by a wide margin, and
/// it should be -- these strings are never hit, they are only leaned on, and
/// nothing is damping them.
///
/// The level is zero, and it has now been zero for two different reasons. The
/// first was that the bank moved a keyboard-wide error by nothing at any
/// coupling from 2 to 100. That reading was about the measurement rather than
/// the mechanism: what this population acts on is the top octave's aftersound,
/// which every term in that error de-weights — the cell comparison floors
/// eighty-five decibels under the note's own peak and its weight bottoms out at
/// seventy, and a C7 four seconds in is below both. A knob whose whole effect
/// lies under the floor reads as a dead knob.
///
/// Measured where it acts it is emphatically not dead, and that is the trap.
/// Tracking the played note's OWN partials, C7 arrives correct and then
/// collapses — thirty-seven decibels under the reference at two seconds, forty-
/// seven by three. Turning this on at 2 puts the curve within eleven decibels
/// at two seconds and exactly on the reference at three, improves the fit AND
/// the held-out notes on the shipped constants and on a fitted set alike, and
/// takes the treble's tail from forty-six decibels more non-harmonic than the
/// instrument to within four. Every one of those is a real measurement and the
/// result is audibly a chime.
///
/// The mechanism it recruits is the one the paragraph below already warned
/// about, at the line the instrument actually has rather than at 84: notes 88
/// and up find THEMSELVES in the bank. A struck C7 stops being a string with a
/// two-second aftersound and becomes a resonator with a five-second one, at a
/// fixed pitch, and every note below it lights the same twenty resonators
/// through whichever upper partials land on them. Fixed pitches ringing five
/// seconds under everything is the definition of the defect, and it improved
/// every number available at the time — including the one written to catch it,
/// because a row-wise minimum across notes prices invariant energy by its
/// weakest relative appearance, and a bank that is a small fraction of a loud
/// note and a large fraction of a quiet one has no weak appearance to find.
///
/// So this stays at zero until the played note can be excluded from what
/// answers it, and the treble's real deficit is the string's to fix: its
/// aftersound taper, not a bank standing in for one.
SONARE_TUNABLE(kSympTopLevel, 0.0f);
SONARE_TUNABLE(kSympTopNoteLo, 88.0f);
SONARE_TUNABLE(kSympTopT60S, 5.0f);
/// How the ring shortens with pitch, in halvings of t60 per octave above the
/// lowest undamped string.
SONARE_TUNABLE(kSympTopT60Oct, 0.5f);

/// The pedal-lifted register: how long a string the felt has left rings, how
/// its partials sit under its fundamental, how much faster they decay, and how
/// hard the bank couples back into the output.
///
/// None of these were parameters before -- the ring was a literal 0.6 s and the
/// coupling a literal 0.06 buried in prepare() -- and that is worth stating
/// rather than quietly fixing, because it explains why no fit ever touched
/// them. It is not that the search rejected these values. The search could not
/// see them, and the corpus it runs on could not have judged them either: every
/// note in it is struck alone with the pedal up, which is the one condition
/// under which this entire population is silent.
///
/// The ring is the value most obviously wrong on inspection. A string with its
/// damper lifted rings for as long as the pedal is held -- that is what a pedal
/// is -- and 0.6 s is shorter than the note that drove it.
SONARE_TUNABLE(kSympRingT60S, 0.6f);
SONARE_TUNABLE(kSympPartialTilt, 0.7f);
SONARE_TUNABLE(kSympPartialDamp, 0.5f);
SONARE_TUNABLE(kSympCoupling, 0.06f);

/// Halvings of a mode's coupling per octave below kSympTaperAnchorHz, and that
/// anchor. Zero couples every fundamental equally, which is not what a bridge
/// does: a string answers the bridge through its own impedance, so a wound bass
/// string barely moves while a treble string answers readily. That is the same
/// fact as a pedalled bass note lighting up the upper half of the keyboard.
///
/// Set on the pedal take, the only probe that excites this: the tail below
/// 160 Hz sat 3.7 dB over the three grands' own span, and 1.0 lands within
/// 0.1 dB of its midpoint.
SONARE_TUNABLE(kSympBassTaperOct, 1.0f);
SONARE_TUNABLE(kSympTaperAnchorHz, 261.6256f);

/// Soundboard radiating band: the modes are log-spread between these corners.
SONARE_TUNABLE(kFLow, 92.0f);
SONARE_TUNABLE(kFHigh, 5400.0f);

/// Soundboard decay: the lowest mode's t60 in seconds, how fast that falls
/// with frequency, and the ceiling the whole law is clamped to.
///
/// These had been written inline at six-tenths of a second, which both hid what
/// they claim and was too short: strike C8, whose string is gone within a
/// second, and the reference still carries 125-1000 Hz two and a half seconds
/// later where the model was 85 dB under.
///
/// A second is the number spruce gives. A resonator's decay follows its loss
/// factor as t60 ~ 2.2/(f*eta), and spruce's one-to-three percent puts a 100 Hz
/// board mode at about a second — so this is now quoted from the material
/// rather than fitted, and what the fit had wanted instead is worth recording.
/// Against the reference, and with no frame bank present, the board scored best
/// at 2.5 s: it recovered 38 dB of the C7 sustain and cost under 2 dB of
/// broadband level anywhere. That was the board standing in for a member the
/// instrument has and the model did not, and it is why the number came out two
/// and a half times what the wood can do. With the frame bank carrying the long
/// tail the board no longer has to, and the whole-keyboard fit takes it back
/// under the material figure rather than to it: four tenths of a second, which
/// is spruce at the stiff end of its loss range. The material argument bounds
/// the number from above and does not pin it, so a fitted value inside the
/// bound is the fit telling the board how much of the tail is not its job.
///
/// The slope is the part of the law the fit is confident about, and it is
/// steep: a board mode at 1 kHz keeps a small fraction of the t60 of one at 92.
/// That is the wood doing what wood does, and it is also what leaves room for
/// the frame — with a gentle slope the board would still be ringing where only
/// the iron should be, and the two banks would be fitting against each other
/// instead of dividing the spectrum between them.
SONARE_TUNABLE(kBoardT60Base, 0.4f);
SONARE_TUNABLE(kBoardT60Slope, 2.0f);
SONARE_TUNABLE(kBoardT60Max, 1.0f);

/// Frame (plate and rim) bank: its radiating band, its decay, and its return
/// level. See the bank's own commentary in piano_voice.h for why an iron
/// member is a different resonator from a wooden one.
///
/// The decay is quoted at the bottom of the band and graded barely at all, not
/// because a material says so but because that is as much grade as the fit will
/// accept: what the bank is standing in for turns out to be the whole
/// long-decay structure and not a plate whose loss factor can be looked up. It
/// is worth being precise about what is and is not derived here, because a
/// claim about this bank is easy to make and hard to earn.
///
/// What IS derived is that the member exists. A grand carries 150 kg of cast
/// iron and a laminated rim; iron's loss factor is an order of magnitude below
/// wood's, so it is the only part of the instrument that can hold a low
/// frequency for seconds, and the reference plainly does hold one. Adding the
/// bank at all takes the keyboard's low-band error from 15.8 dB to about 7.
///
/// What is NOT derived is nine seconds. Quote it against the bottom of the band
/// and it implies a loss factor above the 0.1-0.3 percent that iron gives, and
/// with almost no grade the implied figure varies right across the band, which
/// no single material does. The value it replaces was eighteen, which does land
/// on iron's own 0.20 percent at 62 Hz — and it was wrong: scored only on the
/// note while the key is DOWN it looked best, and it left a released treble
/// note ringing forty times louder than the instrument does. Nine is what
/// survives once the release is in the metric. A number that lands on a
/// textbook constant is evidence for nothing if the measurement it came from
/// could not see half the note.
///
/// The band is wider at both ends than a plate is, and deliberately. It reaches
/// below the lowest note's fundamental because the reference radiates there
/// under every note including the ones whose own series starts an octave up,
/// and it runs into the low kilohertz rather than stopping where the plate
/// stops behaving as a plate: above roughly 600 Hz the plate's modal density
/// turns its response into a broadband floor rather than a set of rings, so the
/// upper part of this band is not literally plate modes; it is the rim, the lid
/// and the case answering, which have the same property of ringing long and the
/// same indifference to which note was struck.
SONARE_TUNABLE(kFrameFLow, 39.68f);
SONARE_TUNABLE(kFrameFHigh, 4500.0f);
SONARE_TUNABLE(kFrameT60S, 9.0f);
SONARE_TUNABLE(kFrameT60Slope, 0.1f);
/// Return level. Zero renders exactly as a build without the bank, and that is
/// what ships: the late field below carries this member's job with two orders
/// of magnitude more modes, and eight resonators standing in for a structure
/// are audible as eight resonators.
///
/// The bank had been at four, which is where a whole-keyboard fit settled with
/// its decay and band free to disagree. What no term in that fit could price is
/// that a bank is a set of PITCHES. Rendered on its own -- the voice minus the
/// same voice without it, which is exact, since it feeds nothing back -- it
/// answers every note at 40, 195, 945 and 4547 Hz, with 40 to 90 dB of canyon
/// between them, and the last of those is a pair split by 8 cents that beats at
/// 21 Hz. Its share of the note rises with pitch, from 47 dB under a C3 to 29
/// under a C6, so the higher the note the more of it is the bank. That is a
/// bell, it is present from the first 200 ms, and it is what listening reported
/// while the level-based terms were calling the bank an improvement.
///
/// It was also not filling the hole it appeared to fill. Measured as
/// non-harmonic energy per octave against three concert grands, the bank leaves
/// 62 and 125 Hz exactly where they were -- 30 to 45 dB under the instrument,
/// identical with the bank on and off -- and puts 15 to 20 dB TOO MUCH at 250 Hz
/// and 1 kHz, on two pitches. The hole and the spike were the same defect, and
/// replacing the eight resonators with a continuum closes both at once: the
/// whole profile's error over 31 Hz to 1 kHz goes from 24.16 dB rms to 7.93,
/// with 62, 125 and 1000 Hz landing within 1.6 dB of the instrument.
SONARE_TUNABLE(kFrameLevel, 0.0f);
/// Split between the two halves of each frame mode, in cents. Zero leaves the
/// bank as eight separate frequencies and renders exactly as a build without
/// this; above zero the same eight resonators are re-read as four near-
/// degenerate PAIRS, which costs nothing on the audio thread because the count
/// does not change.
///
/// A ring that does not waver is what a bell is, and the bank at eight separate
/// frequencies does not waver: spread over the plate band its neighbours sit
/// tens of hertz apart, far enough that any beating between them is averaged
/// away inside a single envelope window, so each mode decays smoothly and their
/// sum decays smoothly. Measured, the bank's low aftersound is FLATTER than
/// band-limited noise -- below the floor a diffuse field produces -- while the
/// instrument's sits around the figure two partials beating to a full null
/// give. The instrument is not more diffuse there than the model. It is less
/// steady, and those are opposite readings of the same number.
///
/// Pairs are how a real structure produces that. A plate's modes come in
/// near-degenerate families that a real instrument's broken symmetry splits by
/// a small fraction of their frequency, and a split pair driven together beats
/// at the difference. Cents rather than hertz because the splitting mechanism
/// is proportional: the same asymmetry moves a high mode further in absolute
/// terms than a low one.
///
/// Eight cents is four tenths of a percent, which is where a real structure's
/// broken symmetry puts a near-degenerate pair, and it is also where the
/// measurement settles from both directions. Against the shipped constants it
/// takes the whole-keyboard error from 10.39 to 9.50 and the held-out notes
/// from 10.80 to 9.97; against a fitted set, 7.93 to 7.69 and 8.79 to 8.36. On
/// the term that counts how much of the spectrum answers every note alike --
/// which is what a bell is, and what four rounds of listening kept reporting
/// while the level-based terms called each change an improvement -- it goes
/// from 12.68 to 6.79 on the shipped constants and 8.45 to 6.25 on the fitted
/// one. Four and fourteen cents measure within noise of eight on one basis
/// each; twenty and thirty-five are worse on both.
SONARE_TUNABLE(kFrameSplitCents, 8.0f);

/// Soundboard phase diffusion coefficient (both allpass stages).
SONARE_TUNABLE(kDiffuserG, 0.22f);

/// Soundboard air/sizzle noise gain, envelope-followed off the radiated
/// signal. Reference renders measure 20-40 dB tone-to-noise; a clean partial
/// stack reads dry and synthetic. Small: what the whole-keyboard fit wants is
/// a trace, and the level above which it starts paying for the hiss with tone
/// is not far above it.
SONARE_TUNABLE(kAirGain, 0.01f);

/// The band the air layer occupies. The lowpass is two poles, and that order is
/// the point rather than an implementation detail.
///
/// A single pole leaves the band at 6 dB per octave, so at 8 kHz it is only
/// 9 dB below its own peak. The instrument's content there is nowhere near that
/// high: measured against three concert grands over a sustained note, the
/// octave above 8 kHz sits 60 to 70 dB under the octave holding the fundamental
/// on every note from C2 to C5, which made this layer's skirt the loudest thing
/// in that octave -- 14 to 31 dB over the instrument. It reads as a veil rather
/// than as hiss because it does not decay with the note the way a partial does:
/// the follower holds it up while the tone underneath falls away, so the top
/// end loses its shape as the note sustains.
///
/// The second pole was chosen against the band's tonality -- the negated log
/// spectral flatness of 2 to 8 kHz, which says whether the band is a comb of
/// partials or a bed -- because that is the quantity the defect is heard as and
/// a level alone cannot separate the two. One pole reads 3 to 8 dB less tonal
/// than the instrument, two lands within 2.5 dB on every note, three overshoots
/// to 3 to 6 dB MORE tonal, which is the dry-and-synthetic side this layer
/// exists to stay off. Nothing below 4 kHz moves by so much as a tenth of a
/// decibel at any order, so the level the whole-keyboard fit put on the layer
/// carries over unchanged and is not refitted here.
/// How much of the board's return is radiated differently to the two legs,
/// and the allpass coefficient that makes it different.
///
/// A grand's board is metres across and radiates to two listening positions
/// through paths that differ in phase at every frequency, so a close pair
/// hears one instrument twice rather than one signal twice. Measured over a
/// sustained note against three concert grands, their channel correlation runs
/// 0.09 to 0.92 depending on band and note and their side energy sits 0 to
/// 12 dB under their mid; this voice returned its board to both legs
/// identically, which is a correlation of exactly 1 and a side 24 to 53 dB
/// down -- a point source. Per-voice pan scatter cannot supply this: it moves
/// one mono voice sideways, so a single note stays perfectly correlated.
///
/// Allpasses rather than filters or delays, so each leg receives the same
/// magnitude spectrum the mono return had and only its phase differs. That is
/// what the two paths actually differ in, and it is also what keeps the mono
/// sum from developing a comb.
///
/// Zero is the identity and folds the whole layer out. Eight tenths is where
/// the three instruments stop agreeing with each other: they span 0.29 to 0.85
/// of correlation at C6 and -0.35 to 0.13 at C7, so no single setting lands
/// every note, and the value was chosen by listening inside that range rather
/// than fitted to a median none of them sits at.
/// How fast the air layer follows the string up, and how fast it comes back
/// down. Both in milliseconds, and the release is the one that carries a
/// mechanism rather than a smoothing choice.
///
/// The layer is an envelope follower on the radiated signal, so at a release of
/// 200 ms against a note that decays over seconds it keeps up exactly and the
/// board noise is a FIXED PROPORTION of the string. A real board is not: its
/// high-frequency field has its own decay, so as the partials die away what is
/// left is proportionally more field and less partial. Measured as the 2-8 kHz
/// tonality of a held note against the same figure for its decay tail, three
/// concert grands fill IN by about 20 dB between the two windows -- 36.4..39.4
/// during a C4, 11.9..19.6 after it -- while this voice goes the other way and
/// becomes 7.5 dB purer. That is the same sign on a held triad.
///
/// It is also why the layer's level cannot settle: every setting that brings
/// the tail toward the instrument takes the note past it, because one envelope
/// is serving two decays. A release that outlasts the string is what separates
/// them.
SONARE_TUNABLE(kAirAttackMs, 30.0f);
SONARE_TUNABLE(kAirReleaseMs, 200.0f);

/// How much of the air layer's excitation is taken from the air BAND of the
/// drive rather than from the drive broadband. Zero is broadband and renders
/// exactly as before.
///
/// The board is shared by every note sounding on the part, so this layer cannot
/// be told which note it is radiating and cannot carry a keytrack. It does not
/// need one: a structure's field in a band is driven by the energy in that
/// band, and reading the drive through the same corners the layer radiates
/// through is that statement rather than an approximation of it.
///
/// What it fixes is a top octave that would not stop. Broadband, a C8 -- whose
/// fundamental at 4186 Hz sits ABOVE this layer's own band -- drives the layer
/// as hard as a C4 does, so a release long enough to let a middle note's tail
/// fill in leaves the layer sounding after the string has gone. Measured
/// against three concert grands, whose C8 decay they agree on to within 0.57
/// dB/s -- the tightest note on the keyboard -- a 1500 ms release took that
/// note 7.8 dB/s slower, and 1000 ms took it 3.7. Below about 700 ms the cost
/// disappears and so does most of the fill.
SONARE_TUNABLE(kAirDriveBandMix, 0.0f);

SONARE_TUNABLE(kBoardWidth, 0.8f);
SONARE_TUNABLE(kBoardWidthG, 0.62f);
SONARE_TUNABLE(kAirHpHz, 500.0f);
SONARE_TUNABLE(kAirLpHz, 2800.0f);

/// Case and rim network: its return level, its decay, and where that decay
/// starts falling with frequency. See the network's own commentary in
/// piano_voice.h for why the late field is made this way and not with more
/// resonators. Zero renders exactly as a build without the network.
///
/// The decay is quoted at the bottom of the range, like the frame bank's, and
/// is the same quantity that bank was carrying: the long tail a wooden member
/// cannot hold. What is new is that it arrives dense instead of as eight
/// pitches, so it can be long without being a chord.
///
/// It is the second of the two figures the reference states about its low field
/// (see the header for the first, and for how both were measured). Thirty
/// decibels between a mode's peak and the floor beside it is a modal overlap
/// near 0.15, and at the density below that is 2.2 * 0.28 / 0.15 seconds. It is
/// not the same quantity as the frame bank's nine and does not contradict it:
/// nine seconds was one bank's ring, this is the decay a field of a thousand-odd
/// modes needs for its peaks to stand where the instrument's do.
///
/// The level is fitted against the instrument's non-harmonic octave profile
/// rather than against the corpus loss, and that choice needs stating because
/// the two disagree.
///
/// Built to the two figures above the network measures right: it takes the low
/// band's density from 0.137 per hertz to 0.21 against the instrument's 0.28,
/// and its peak-to-floor from 71 dB to 42 against 30. It nevertheless scores
/// WORSE on the corpus loss than the eight-resonator bank at every setting --
/// over thirty tried by hand across level, drive corner, bank level and bank
/// band, and a seven-coordinate descent that was free to turn it on and
/// declined.
///
/// The two are not in contradiction, because the loss cannot resolve what was
/// measured. Its long scale is an 8192-point transform, whose bins are 5.86 Hz,
/// and a 0.28-per-hertz field has its modes 3.58 Hz apart -- every one of them
/// falls inside a neighbour's bin. Counted through that geometry the three
/// reference grands show 0.3, 1.7 and 3.7 peaks below 300 Hz where a 0.25 Hz
/// transform finds 69, 72 and 80; the bank shows 4.0, which is more than any of
/// them. In the band where the defect lives, the term written to catch a bell
/// already reads the bank as the peakiest thing in the comparison and has no
/// resolution left to reward putting the density back.
///
/// What settles it is a measurement the loss's geometry does not bound. Read as
/// non-harmonic energy per octave -- the partials masked out, each octave
/// against the render's own total -- the instrument is a smooth curve and the
/// bank makes a comb. Against the median of three grands over 31 Hz to 1 kHz the
/// bank is 24.16 dB rms out; this network at one is 7.93, and 62, 125 and
/// 1000 Hz land within 1.6 dB. Listening agrees, which is the direction that
/// matters: the bank's ring was reported by ear before any of this was measured.
///
/// One rather than the profile's own optimum, which sits a little lower: the
/// remaining profile error is a 17.5 dB shortfall at 31 Hz that this member
/// cannot fix, because the fourth-order radiation highpass at kRadiationHpHz
/// removes the drive two octaves below its corner before the network sees it.
/// Fitting the level against an error owned by something else would buy that
/// band by overshooting 250 and 500 Hz, which is what the smaller values do.
///
/// Quoted against the board's own return rather than against the output, since
/// process() sends this through `out_gain_` with the two banks. The figure the
/// profile was fitted at is 1.0 of the OUTPUT, and the piano family's soundboard
/// mix is 0.35, so the number here is that over this one.
SONARE_TUNABLE(kCaseLevel, 2.857143f);
SONARE_TUNABLE(kCaseT60S, 4.2f);
/// How long a blow takes to enter the board bank, in milliseconds.
///
/// A hammer is in contact for a fraction of a millisecond at the top of the
/// keyboard and a couple at the bottom, and the board's answer builds over
/// longer than either. Delivered in one sample instead, the same blow is a
/// click: measured against three concert grands, the level that fills C8's
/// first fifty milliseconds of energy overshoots the loudest its body ever
/// gets by eleven decibels, and the level that lands the second leaves the
/// first seven and a half short -- one blow, two readings, fourteen decibels
/// apart, which is a shape in time and not a level. The one-pole has unity
/// gain at DC, so the area is the blow whatever this is set to and the two can
/// be fitted apart.
///
/// Ten milliseconds is where the two readings stop disagreeing: at three the
/// blow still moves the peak faster than it moves the energy and the bass
/// notes' levels shift with it, at twenty-five the peak stops arriving at all.
SONARE_TUNABLE(kBoardStrikeSpreadMs, 10.0f);
/// Ratio between the slowest line's decay time and kCaseT60S, which the fastest
/// keeps. At one the network decays at a single rate; above it the lines are
/// spread log-uniformly between the two and the aggregate falls fast and then
/// slow, which is what a structure whose modes carry a range of loss factors
/// does. A rigid case is not one oscillator, and the weakly coupled part of it
/// holds energy long after the strongly coupled part has given it up.
///
/// A structure whose modes carry a range of loss factors does behave this way,
/// but the reading that produced this knob does not survive its reference. The
/// phrases it was fitted on are read in bands that sit under the corpus's own
/// recorded rumble (see kCaseDriveDirect), and against a floor that does not
/// decay the fit simply lengthens the slowest line until it stops decaying
/// either: it walked to sixteen, which puts that line at sixty-seven seconds
/// and no piano case anywhere near it.
///
/// Identity, and unproven, for the same reason and on the same terms as
/// kCaseDriveDirect.
SONARE_TUNABLE(kCaseT60Spread, 1.0f);
/// One-pole corner on each line's feedback. A radiating case loses its high
/// frequencies first, and this is the only place that grading is stated: the
/// frame bank's slope had to be flat because its modes are too far apart to
/// grade anything between them.
SONARE_TUNABLE(kCaseDampHz, 2200.0f);
/// Corner of the two-pole filter on what the network is DRIVEN with, which is a
/// different job from the damping above: that grades the decay inside the loop,
/// this decides which part of the spectrum reaches the loop at all. See the
/// header for why the network is band-limited rather than full-band.
///
/// Eighty rather than the three hundred the density measurement is quoted at,
/// because the octave profile wants a tilt and not just a limit. At 300 the
/// network lands 62 and 125 Hz correctly and then carries 7 and 4 dB too much
/// into 250 and 500; two poles at 80 keep the same in-band level -- both have
/// unity gain at DC -- while taking 16 dB more out of 250 Hz, which is where
/// the instrument's own curve falls away. The physical reading is the same one:
/// what couples a string's bridge force into a structure of that mass falls with
/// frequency well before the structure stops radiating.
SONARE_TUNABLE(kCaseInHz, 80.0f);
/// Share of the board's own diffused signal in what drives the case network,
/// against the second-difference residue the two modal banks are struck with.
///
/// The residue is a differentiator: its magnitude is 2*|sin(w)|, which is 42 dB
/// down at 30 Hz and 36 dB at 60 Hz. That is precisely the band this member
/// radiates in, so a network fed the residue and then band-limited by kCaseInHz
/// is handed almost nothing to radiate, and its level has to be raised out of
/// proportion before anything is audible. A resonator does not mind being struck
/// by a difference -- it rings at its own frequency whatever the drive's tilt --
/// but a radiator does, because its output IS its drive shaped by the network.
/// The physical quantity that moves a soundboard is the bridge force, not the
/// second difference of the bridge force.
///
/// The filter shape above is arithmetic and is not in doubt. Whether it is a
/// DEFECT is not established, and the attempt to establish it failed on the
/// reference rather than on the voice: the corpus this voice is calibrated
/// against carries about -64 dBFS of rumble recorded into its samples, flat
/// from five hertz to two hundred, identical to within a decibel across all
/// three instruments -- one recording chain, not three pianos. Nothing below
/// roughly 250 Hz in its tail is the instrument, so it cannot say what a grand's
/// low band should be, and a fit against it walks this control to its bound.
///
/// A reading the recorded rumble cannot reach has since been taken, and it
/// settles both halves. It is a RATIO — the energy below a note's fundamental
/// against the fundamental band, read from 0.2 to 1.2 s after the strike, where
/// both bands sit thirty to forty decibels clear of that floor. On three concert
/// grands it climbs about forty decibels from C3 to C8, and at C8 what is under
/// the note outweighs the note: the string up there is short and quiet while the
/// body's answer to a blow is not, so the top of a grand radiates mostly body.
/// This voice is flat instead — it matches at C3 and is 18, 35, 38 and 45 dB
/// short at C6, F#6, C7 and C8.
///
/// So the drive path is confirmed as the only thing that reaches that deficit:
/// taking this control to unity moves it by 29 to 32 dB where every other
/// candidate moves it by less than two — the modal bank's low corner, the frame
/// bank, sympathetic coupling and the patch's own soundboard knob all measured
/// dead. And the same measurement rules the control out as the fix, because it
/// moves the WHOLE keyboard by that amount: at unity C3 overshoots by 34 dB
/// while C8 is still 13 dB short, which is the shape of a fixed fraction of the
/// string's output rather than of a body. The register dependence is what the
/// instrument has and what this voice has no mechanism for at all, and no
/// setting of a level can supply it.
///
/// The knob therefore stays at its identity, now for a stated reason rather than
/// for want of evidence: what is missing is a body excited by the blow rather
/// than driven by the note.
SONARE_TUNABLE(kCaseDriveDirect, 0.0f);
/// Delay lengths in samples at the network's own 6 kHz internal rate (a 48 kHz
/// host decimated by kCaseDecim), all prime and so mutually prime, which makes
/// the network's period their product rather than a short common multiple.
///
/// Their TOTAL is the design quantity and the individual values are only a
/// spread around it: a delay network's modal density in modes per hertz is its
/// total delay in seconds, so this set's 1676 samples at 6 kHz is 0.2793 s and
/// therefore 0.28 modes per hertz -- the figure measured off three concert
/// grands' low band. The previous set was half that in time and was chosen
/// against a criterion that turned out to be a different quantity (see the
/// header). The ratio between longest and shortest is kept near the old set's,
/// so the spread of line lengths -- which is what stops a network sounding like
/// one delay -- is unchanged.
constexpr uint32_t kCaseDelays6k[8] = {131, 149, 173, 197, 223, 241, 269, 293};
/// Injection signs, one per line. Driving every line with the same sign and
/// gain feeds the network eight coherent copies of its input, and coherent
/// copies at eight fixed delays are a comb filter -- which is audible as
/// flutter and measures as an envelope that swings far wider than a diffuse
/// field's. A fixed sign pattern decorrelates them at no cost and keeps a
/// bounce bit-stable, which a random one would not.
constexpr float kCaseInSign[8] = {1.0f, -1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f};
/// Coefficient of the allpass diffuser inside each line, and its lengths at
/// 48 kHz. Zero skips the stage outright, which renders exactly as the network
/// without it; see the header for why skipping and not passing through.
SONARE_TUNABLE(kCaseDiffuseG, 0.0f);
constexpr uint32_t kCaseApDelays6k[8] = {11, 13, 17, 19, 23, 29, 31, 37};

}  // namespace

void PianoResonanceBank::prepare(double sample_rate) noexcept {
  const float sr = sample_rate > 0.0 ? static_cast<float>(sample_rate) : 48000.0f;
  for (Mode& m : modes_) m = Mode{};
  int n = 0;
  // The permanently undamped treble, one mode per semitone and ungated. The
  // pitches carry the same Railsback stretch the played strings do, because
  // they ARE played strings — the reference's rows sit a few cents sharp and
  // a bank on bare equal temperament would beat against every note that
  // drives it.
  const int top_lo = static_cast<int>(std::lround(std::clamp(kSympTopNoteLo, 21.0f, 120.0f)));
  if (kSympTopLevel != 0.0f) {
    for (int note = top_lo; note <= 108 && n < kResonanceModes; ++note) {
      const auto midi_note = static_cast<uint8_t>(note);
      const float f = note_to_hz(midi_note) * std::exp2(piano_stretch_cents(midi_note) / 1200.0f);
      if (f >= 0.45f * sr) break;
      const float oct = static_cast<float>(note - top_lo) / 12.0f;
      const float t60 = std::max(0.05f, kSympTopT60S * std::exp2(-kSympTopT60Oct * oct));
      const float w = kTwoPi * f / sr;
      const float r = std::exp(-6.907755279f / (sr * t60));
      Mode& m = modes_[static_cast<size_t>(n++)];
      m.a1 = 2.0f * r * std::cos(w);
      m.a2 = -r * r;
      m.gain = kSympTopLevel * (1.0f - r);
    }
  }
  ungated_count_ = n;
  // The damped register, which answers only while the pedal holds the felt off
  // the strings: fundamentals spread E1..E6 every 4 semitones, then the second
  // and third partials of the same strings.
  //
  // The partials are the mechanism, not a refinement of it. A pedalled bass
  // note is heard lighting up the upper half of the keyboard, and what is
  // sounding up there is the treble strings' UPPER partials, not their
  // fundamentals -- those sit far below the energy that reached them. A bank of
  // fundamentals alone is therefore not a reduced model of sympathetic
  // resonance; it is a set of pure tones under the note, which is a hum. Every
  // played note also arrives as a partial series, so a bank with no partials
  // has almost nothing for the coupling to find in the first place.
  //
  // They are free. The process loop runs over all forty-four slots whichever
  // are filled, and twenty-eight of them stand empty whenever the undamped
  // treble population above ships at a zero level -- which is its default. The
  // cost of this was already being paid to circulate zeros.
  //
  // Stretched, for the reason the population above states about itself: these
  // pitches ARE played strings, the instrument's own rows sit a few cents
  // sharp of equal temperament, and a bank on bare equal temperament beats
  // against every note that drives it. That the two populations disagreed
  // about this was an oversight rather than a decision.
  const float ring = std::max(0.05f, kSympRingT60S);
  const float tilt = std::max(0.0f, kSympPartialTilt);
  const float pdamp = std::max(0.0f, kSympPartialDamp);
  for (int k = 1; k <= 3 && n < kResonanceModes; ++k) {
    const float kf = static_cast<float>(k);
    for (int i = 0; i < 16 && n < kResonanceModes; ++i) {
      const auto note = static_cast<uint8_t>(28 + 4 * i);
      const float b = piano_inharmonicity_b(note);
      const float f0 = note_to_hz(note) * std::exp2(piano_stretch_cents(note) / 1200.0f);
      // Stiff-string placement, the same law the played strings carry: the
      // third partial of a bass string is sharp by more than a cent, and a
      // sympathetic bank that answers a note's own third partial has to be
      // where that partial actually is or it beats instead of ringing.
      const float f = f0 * kf * std::sqrt(1.0f + b * kf * kf);
      if (f >= 0.45f * sr) continue;
      // A string's upper partials shed energy faster than its fundamental, so
      // the higher the partial the shorter the ring.
      const float t60 = std::max(0.02f, ring * std::pow(kf, -pdamp));
      const float w = kTwoPi * f / sr;
      const float r = std::exp(-6.907755279f / (sr * t60));
      Mode& m = modes_[static_cast<size_t>(n++)];
      m.a1 = 2.0f * r * std::cos(w);
      m.a2 = -r * r;
      // Normalize the resonator to ~unity peak gain (the (1-r) factor cancels
      // the high-Q resonant boost) so the bank is a weak coupling, not a
      // runaway bandpass on the played note, then tilt the series down.
      m.gain = (1.0f - r) * std::pow(kf, -tilt);
      // Bridge admittance: the heavier the string, the less of the bridge's
      // motion it takes up. Applied per MODE rather than per string, because a
      // bass string's third partial is a light, high-frequency motion of the
      // same wire and answers more readily than its fundamental does.
      if (kSympBassTaperOct != 0.0f && f > 0.0f) {
        const float octaves_below =
            std::max(0.0f, std::log2(std::max(kSympTaperAnchorHz, 1.0f) / f));
        m.gain *= std::exp2(-kSympBassTaperOct * octaves_below);
      }
    }
  }
  gate_ = 0.0f;
  // Damper-open envelope: ~10 ms to lift, ~60 ms to fall.
  gate_open_coeff_ = 1.0f - std::exp(-1.0f / (0.010f * sr));
  gate_close_coeff_ = 1.0f - std::exp(-1.0f / (0.060f * sr));
  // Extra ring-out applied while the dampers are falling (~0.15 s t60).
  ringout_ = std::exp(-6.907755279f / (sr * 0.15f));
  // Weak sympathetic coupling (the played string still dominates).
  out_gain_ = std::max(0.0f, kSympCoupling);
}

void PianoResonanceBank::prepare_custom(double sample_rate, const float* freqs, int count,
                                        float ring_t60_s, float out_gain) noexcept {
  const float sr = sample_rate > 0.0 ? static_cast<float>(sample_rate) : 48000.0f;
  const int n = std::min(count, kResonanceModes);
  // Plucked open strings have no dampers either, but this path is driven with
  // damper_open held true, so the gate is transparent and the split the piano
  // path needs would be a distinction without a difference here.
  ungated_count_ = 0;
  const float t60 = std::max(0.02f, ring_t60_s);
  const float r = std::exp(-6.907755279f / (sr * t60));
  for (int i = 0; i < kResonanceModes; ++i) {
    Mode& m = modes_[static_cast<size_t>(i)];
    const float f = (i < n && freqs != nullptr) ? freqs[i] : 0.0f;
    if (f <= 0.0f || f >= 0.45f * sr) {
      m = Mode{};
      continue;
    }
    const float w = kTwoPi * f / sr;
    m.a1 = 2.0f * r * std::cos(w);
    m.a2 = -r * r;
    // Unity-peak normalization (the (1-r) factor cancels the high-Q resonant
    // boost) so the bank is a weak coupling, not a runaway bandpass on the note.
    m.gain = 1.0f - r;
    m.y1 = 0.0f;
    m.y2 = 0.0f;
  }
  gate_ = 0.0f;
  // Held open by the caller (plucked strings have no dampers), so the fall-time
  // coefficient is only the ~10 ms lift; reuse the piano smoothing constants.
  gate_open_coeff_ = 1.0f - std::exp(-1.0f / (0.010f * sr));
  gate_close_coeff_ = 1.0f - std::exp(-1.0f / (0.060f * sr));
  ringout_ = std::exp(-6.907755279f / (sr * 0.15f));
  out_gain_ = std::max(0.0f, out_gain);
}

void PianoResonanceBank::prepare_guitar_sympathetic(double sample_rate) noexcept {
  constexpr uint8_t kOpenStrings[6] = {40, 45, 50, 55, 59, 64};  // E2 A2 D3 G3 B3 E4
  float freqs[kResonanceModes];
  int n = 0;
  for (uint8_t note : kOpenStrings) freqs[n++] = note_to_hz(note);
  for (uint8_t note : kOpenStrings) freqs[n++] = 2.0f * note_to_hz(note);
  for (int i = 0; i < 4; ++i) freqs[n++] = 3.0f * note_to_hz(kOpenStrings[i]);
  // Open guitar/harp strings ring for seconds; a ~1.5 s bank t60 keeps the
  // halo audible without a runaway tail, at a weak coupling level.
  prepare_custom(sample_rate, freqs, n, /*ring_t60_s=*/1.5f, /*out_gain=*/0.05f);
}

void PianoResonanceBank::reset() noexcept {
  for (Mode& m : modes_) {
    m.y1 = 0.0f;
    m.y2 = 0.0f;
  }
  gate_ = 0.0f;
}

float PianoResonanceBank::process(float bridge_in, bool damper_open) noexcept {
  const float target = damper_open ? 1.0f : kDuplexFloor;
  gate_ += (damper_open ? gate_open_coeff_ : gate_close_coeff_) * (target - gate_);
  float sum = 0.0f;
  // The undamped treble takes the drive raw: no felt ever touches it, so
  // neither the gate nor the ring-out below has anything to say about it.
  for (int i = 0; i < ungated_count_; ++i) {
    Mode& m = modes_[static_cast<size_t>(i)];
    const float y = m.a1 * m.y1 + m.a2 * m.y2 + m.gain * bridge_in;
    m.y2 = m.y1;
    m.y1 = y;
    sum += y;
  }
  const float x = gate_ * bridge_in;
  for (int i = ungated_count_; i < kResonanceModes; ++i) {
    Mode& m = modes_[static_cast<size_t>(i)];
    const float y = m.a1 * m.y1 + m.a2 * m.y2 + m.gain * x;
    m.y2 = m.y1;
    m.y1 = y;
    sum += y;
  }
  // As the dampers fall back the pedal-lifted strings stop ringing quickly
  // (down to the duplex floor, whose faint ring stays).
  if (!damper_open && gate_ < 0.5f && gate_ > 1.2f * kDuplexFloor) {
    for (int i = ungated_count_; i < kResonanceModes; ++i) {
      Mode& m = modes_[static_cast<size_t>(i)];
      m.y1 *= ringout_;
      m.y2 *= ringout_;
    }
  }
  return out_gain_ * sum;
}

void PianoSoundboard::prepare(double sample_rate, float mix) noexcept {
  const float sr = sample_rate > 0.0 ? static_cast<float>(sample_rate) : 48000.0f;
  out_gain_ = std::clamp(mix, 0.0f, 1.0f);
  // Phase diffusers: two short Schroeder allpasses (flat magnitude) standing
  // in for the board's dense high-order mode lattice, which scatters every
  // partial's phase by a different amount. Sized in ms so the smear tracks
  // the sample rate; incommensurate lengths avoid a combined echo.
  constexpr float kDiffuserMs[2] = {4.1f, 9.7f};
  for (int d = 0; d < 2; ++d) {
    diff_len_[d] =
        std::clamp<size_t>(static_cast<size_t>(kDiffuserMs[d] * 0.001f * sr), 4, kDiffuserCapacity);
    diff_buf_[d].fill(0.0f);
    diff_idx_[d] = 0;
  }
  // One decorrelator per leg. Longer than the diffusers and incommensurate
  // with them, so the two legs' phase fields separate at low frequencies too
  // rather than only where the diffusers act.
  constexpr float kSideMs[2] = {13.3f, 21.1f};
  for (int d = 0; d < 2; ++d) {
    side_len_[d] =
        std::clamp<size_t>(static_cast<size_t>(kSideMs[d] * 0.001f * sr), 4, kDiffuserCapacity);
    side_buf_[d].fill(0.0f);
    side_idx_[d] = 0;
  }
  side_ = 0.0f;
  // Modes log-spread across the soundboard's radiating band. A perfectly
  // geometric spacing would comb; a deterministic per-mode nudge breaks the
  // periodicity (no RNG — derived from the index so bounces stay bit-stable).
  for (int i = 0; i < kSoundboardModes; ++i) {
    Mode& m = modes_[static_cast<size_t>(i)];
    const float u = static_cast<float>(i) / static_cast<float>(kSoundboardModes - 1);
    const uint32_t h = (static_cast<uint32_t>(i) + 1u) * 2654435761u;
    const float jit = (static_cast<float>((h >> 9) & 0xFFFFu) / 65535.0f - 0.5f) * 0.08f;
    const float f = kFLow * std::pow(kFHigh / kFLow, u) * (1.0f + jit);
    if (f >= 0.45f * sr) {
      m = Mode{};
      continue;
    }
    const float w = kTwoPi * f / sr;
    // Damping rises with frequency: the low body modes ring ~0.45 s, the
    // high modes are broad and brief. The ring matters as much as the colour
    // — the board smears the strike into a short diffuse bloom, which is a
    // large part of why a piano does not read as a naked plucked string.
    const float t60 =
        std::clamp(kBoardT60Base * std::pow(kFLow / f, kBoardT60Slope), 0.04f, kBoardT60Max);
    const float r = std::exp(-6.907755279f / (sr * t60));
    m.a1 = 2.0f * r * std::cos(w);
    m.a2 = -r * r;
    // Radiation envelope: a low-mid tilt plus a broad bridge formant near
    // ~320 Hz, where a grand soundboard radiates most efficiently.
    const float tilt = std::pow(320.0f / f, 0.35f);
    const float l = std::log(f / 320.0f);
    const float formant = 1.0f + 1.0f * std::exp(-l * l / 0.9f);
    // Bandpass residue (the process() zero at DC/Nyquist), exactly
    // peak-normalized: |H| at the resonance is |1 - e^{-2jw}| / |D(e^{jw})|,
    // so gain = envelope * |D| / (2 sin w) puts every mode's peak at the
    // envelope level. Two-pole modes without the DC zero pile their
    // low-frequency skirts up in phase (a >10 dB bass shelf over the dry
    // path), and their inter-mode phase flips carve deep notches into the
    // dry sum right where note harmonics can land; the bandpass residue is
    // in quadrature with the dry path off-resonance, so it can only add.
    const float d_re = 1.0f - m.a1 * std::cos(w) - m.a2 * std::cos(2.0f * w);
    const float d_im = m.a1 * std::sin(w) + m.a2 * std::sin(2.0f * w);
    const float d_mag = std::sqrt(d_re * d_re + d_im * d_im);
    m.gain = tilt * formant * d_mag / std::max(2.0f * std::sin(w), 1.0e-6f);
    m.y1 = 0.0f;
    m.y2 = 0.0f;
  }
  // Frame modes, log-spread over the plate/rim band. Same bandpass-residue
  // normalization as the board's, so the two sum without one of them piling a
  // low-frequency skirt onto the other.
  const float frame_hi = std::max(kFrameFHigh, kFrameFLow * 1.5f);
  for (int i = 0; i < kFrameModes; ++i) {
    Mode& m = frame_[static_cast<size_t>(i)];
    m = Mode{};
    // Paired or separate, decided once here: above a zero split the eight
    // resonators are four pair centres, each half moved by half the split, and
    // the jitter is drawn per PAIR so the two halves stay a pair rather than
    // being pulled apart by an amount that dwarfs the split itself.
    const bool paired = kFrameSplitCents > 0.0f;
    const int slot = paired ? i / 2 : i;
    const int slots = paired ? kFrameModes / 2 : kFrameModes;
    const float u = static_cast<float>(slot) / static_cast<float>(slots - 1);
    const uint32_t h = (static_cast<uint32_t>(slot) + 7u) * 2246822519u;
    const float jit = (static_cast<float>((h >> 9) & 0xFFFFu) / 65535.0f - 0.5f) * 0.10f;
    const float split =
        paired ? std::exp2(((i % 2 == 0) ? -0.5f : 0.5f) * kFrameSplitCents / 1200.0f) : 1.0f;
    const float f = kFrameFLow * std::pow(frame_hi / kFrameFLow, u) * (1.0f + jit) * split;
    if (kFrameLevel <= 0.0f || f >= 0.45f * sr) continue;
    const float w = kTwoPi * f / sr;
    const float t60 =
        std::max(0.05f, kFrameT60S * std::pow(kFrameFLow / f, std::max(0.0f, kFrameT60Slope)));
    const float r = std::exp(-6.907755279f / (sr * t60));
    m.a1 = 2.0f * r * std::cos(w);
    m.a2 = -r * r;
    const float d_re = 1.0f - m.a1 * std::cos(w) - m.a2 * std::cos(2.0f * w);
    const float d_im = m.a1 * std::sin(w) + m.a2 * std::sin(2.0f * w);
    const float d_mag = std::sqrt(d_re * d_re + d_im * d_im);
    m.gain = kFrameLevel * d_mag / std::max(2.0f * std::sin(w), 1.0e-6f);
  }
  // Case network, at its own decimated rate. Lengths scale with that rate so
  // the network's modal density is a property of time and not of the host's
  // rate, and the whole set is scaled down together if it will not fit the pool
  // -- which costs density at very high rates rather than truncating one line
  // into a different network.
  {
    const float sr_case = sr / static_cast<float>(kCaseDecim);
    const double rate = static_cast<double>(sr_case) / 6000.0;
    uint32_t total = 0;
    for (const uint32_t d6 : kCaseDelays6k) {
      total += std::max(2u, static_cast<uint32_t>(std::lround(static_cast<double>(d6) * rate)));
    }
    const double fit = total > kCaseCapacity
                           ? static_cast<double>(kCaseCapacity) / static_cast<double>(total)
                           : 1.0;
    uint32_t ap_total = 0;
    for (const uint32_t d6 : kCaseApDelays6k) {
      ap_total += std::max(2u, static_cast<uint32_t>(std::lround(static_cast<double>(d6) * rate)));
    }
    const double ap_fit = ap_total > kCaseApCapacity
                              ? static_cast<double>(kCaseApCapacity) / static_cast<double>(ap_total)
                              : 1.0;
    uint32_t off = 0;
    uint32_t ap_off = 0;
    for (size_t i = 0; i < kCaseLines; ++i) {
      const auto len = std::max(
          2u,
          static_cast<uint32_t>(std::lround(static_cast<double>(kCaseDelays6k[i]) * fit * rate)));
      case_off_[i] = off;
      case_len_[i] = len;
      case_idx_[i] = 0;
      off += len;
      const auto ap_len =
          kCaseDiffuseG == 0.0f
              ? 0u
              : std::max(2u, static_cast<uint32_t>(std::lround(
                                 static_cast<double>(kCaseApDelays6k[i]) * ap_fit * rate)));
      case_ap_off_[i] = ap_off;
      case_ap_len_[i] = ap_len;
      case_ap_idx_[i] = 0;
      ap_off += ap_len;
      // Per-line feedback for a common t60: a long line is traversed fewer
      // times a second, so it must lose less each time for the network to
      // decay at one rate rather than eight. The diffuser sits in the loop, so
      // its length counts toward the traversal -- charging the loss against the
      // delay alone would make every line decay faster than it was asked to,
      // by more the shorter the line.
      //
      // The spread, when there is one, is log-uniform across the lines so the
      // aggregate is a sum of exponentials rather than one; kCaseT60S stays the
      // fastest of them, so a spread of one leaves every line exactly where it
      // was and the network keeps the single rate it was designed at.
      float t60 = std::max(0.05f, kCaseT60S);
      if (kCaseT60Spread != 1.0f && kCaseLines > 1) {
        const float u = static_cast<float>(i) / static_cast<float>(kCaseLines - 1);
        t60 *= std::pow(std::max(0.01f, kCaseT60Spread), u);
      }
      case_g_[i] = std::min(
          0.9999f, std::exp(-6.907755279f * static_cast<float>(len + ap_len) / (sr_case * t60)));
      case_lp_[i] = 0.0f;
    }
    case_buf_.fill(0.0f);
    case_ap_buf_.fill(0.0f);
    // In-loop damping is quoted against the network's own rate, since that is
    // what its one-poles run at.
    case_lp_a_ =
        1.0f - std::exp(-kTwoPi * std::clamp(kCaseDampHz, 100.0f, 0.45f * sr_case) / sr_case);
    // The drive filter runs at the HOST rate -- it is the anti-alias filter, so
    // it has to act before the decimation and not after it. Its ceiling is a
    // tenth of the internal rate, which keeps what folds back at least two
    // octaves down its own skirt.
    case_in_a_ = 1.0f - std::exp(-kTwoPi * std::clamp(kCaseInHz, 20.0f, 0.1f * sr_case) / sr);
    case_in_1_ = 0.0f;
    case_in_2_ = 0.0f;
    case_phase_ = 0;
    case_hold_ = 0.0f;
    case_out_lp_ = 0.0f;
    // Smooths the held sample. Well above the band the drive filter passes, so
    // it costs the member nothing and only removes the hold's own steps.
    case_out_a_ = 1.0f - std::exp(-kTwoPi * std::min(800.0f, 0.45f * sr) / sr);
    board_strike_a_ = std::clamp(
        1.0f - std::exp(-1000.0f / (std::max(0.01f, kBoardStrikeSpreadMs) * sr)), 0.0f, 1.0f);
  }
  in1_ = 0.0f;
  in2_ = 0.0f;
  air_env_ = 0.0f;
  air_lp_ = 0.0f;
  air_lp2_ = 0.0f;
  air_hp_ = 0.0f;
  air_rng_ = 0x9E3779B9u;
  air_attack_ =
      1.0f - std::exp(-1.0f / (std::max(kAirAttackMs, 0.1f) * 0.001f * static_cast<float>(sr)));
  air_release_ =
      1.0f - std::exp(-1.0f / (std::max(kAirReleaseMs, 0.1f) * 0.001f * static_cast<float>(sr)));
  air_lp_a_ = 1.0f - std::exp(-kTwoPi * std::min(kAirLpHz, 0.45f * sr) / sr);
  air_hp_a_ = 1.0f - std::exp(-kTwoPi * kAirHpHz / sr);
}

void PianoSoundboard::reset() noexcept {
  for (Mode& m : modes_) {
    m.y1 = 0.0f;
    m.y2 = 0.0f;
  }
  for (Mode& m : frame_) {
    m.y1 = 0.0f;
    m.y2 = 0.0f;
  }
  for (int d = 0; d < 2; ++d) {
    diff_buf_[d].fill(0.0f);
    diff_idx_[d] = 0;
  }
  for (int d = 0; d < 2; ++d) {
    side_buf_[d].fill(0.0f);
    side_idx_[d] = 0;
  }
  side_ = 0.0f;
  board_strike_ = 0.0f;
  board_strike_lp_ = 0.0f;
  case_strike_ = 0.0f;
  case_buf_.fill(0.0f);
  case_idx_.fill(0u);
  case_lp_.fill(0.0f);
  case_ap_buf_.fill(0.0f);
  case_ap_idx_.fill(0u);
  // The per-line damping poles hold energy the delay lines do not. Zeroing the
  // buffers alone leaves it, and it is re-injected on the next traversal --
  // so a network "cleared" by an All Sound Off goes on radiating at its own
  // t60, which is seconds. That is exactly the wash a DAW panic exists to
  // stop, and it survived one until this line was here.
  case_lp_.fill(0.0f);
  case_in_1_ = 0.0f;
  case_in_2_ = 0.0f;
  case_phase_ = 0;
  case_hold_ = 0.0f;
  case_out_lp_ = 0.0f;
  in1_ = 0.0f;
  in2_ = 0.0f;
  air_env_ = 0.0f;
  air_lp_ = 0.0f;
  air_lp2_ = 0.0f;
  air_hp_ = 0.0f;
  air_rng_ = 0x9E3779B9u;
}

float PianoSoundboard::process(float in) noexcept {
  // Radiate: diffuse the phases, then colour with the mode bank. The return
  // is the (1 - kPianoDirectGain) complement of the host's direct share plus
  // the modal colour, so the overall level is preserved while most of the
  // note arrives phase-scattered.
  float d = in;
  for (int st = 0; st < 2; ++st) {
    if (diff_len_[st] == 0) break;
    float* buf = diff_buf_[st].data();
    size_t& idx = diff_idx_[st];
    const float v = d + kDiffuserG * buf[idx];
    const float y = buf[idx] - kDiffuserG * v;
    buf[idx] = v;
    idx = idx + 1 < diff_len_[st] ? idx + 1 : 0;
    d = y;
  }
  const float bp = d - in2_;
  in2_ = in1_;
  in1_ = d;
  float sum = 0.0f;
  // The blow, if one is pending. Tested rather than added for the same reason
  // every other block here is: with none the drive is `bp` exactly, with no
  // sign of zero carried into forty resonators.
  float board_in = bp;
  if (board_strike_ != 0.0f || board_strike_lp_ != 0.0f) {
    board_strike_lp_ += board_strike_a_ * (board_strike_ - board_strike_lp_);
    board_strike_ = 0.0f;
    board_in += board_strike_lp_;
  }
  for (Mode& m : modes_) {
    const float y = m.a1 * m.y1 + m.a2 * m.y2 + m.gain * board_in;
    m.y2 = m.y1;
    m.y1 = y;
    sum += y;
  }
  // Frame bank, off the same bandpass residue. The diffusers ahead of it are
  // allpass, so they cost the drive no magnitude and a resonator does not care
  // about the phase it is struck with; sharing the residue keeps the two banks
  // on one normalization instead of two that have to be kept in step. Tested
  // rather than multiplied out for the same reason the air layer below is: at
  // a zero return level the modes are cleared and the loop would spend eight
  // biquads producing a zero, and `0.0f * x` is not a constant the optimizer
  // may fold. In a shipped build kFrameLevel is a constexpr zero and the
  // block folds out entirely.
  if (kFrameLevel != 0.0f) {
    for (Mode& m : frame_) {
      const float y = m.a1 * m.y1 + m.a2 * m.y2 + m.gain * bp;
      m.y2 = m.y1;
      m.y1 = y;
      sum += y;
    }
  }
  // Sustain air: level-tracked bandpassed noise. Real piano sustain is not a
  // bare line spectrum — string/board sizzle and the undamped-segment wash
  // fill the space between the partials (reference renders measure 20-40 dB
  // tone-to-noise; a clean stack reads dry and synthetic). Deterministic
  // seed, so bounces stay bit-stable.
  //
  // `0.0f * x` is not a constant the optimizer may fold (a NaN or an infinity
  // in x would have to survive), so without this test the follower, the
  // generator and all three one-poles would run per sample to produce a zero
  // at a zero gain. The layer's state feeds nothing but `air`, so skipping it
  // cannot reach any other output. A patch that asks for no air gets none for
  // free, and a fit can switch the layer off without a rebuild.
  // Case and rim network, off the same bandpass residue the two banks use so
  // all three sit on one normalization, then band-limited to the range this
  // member radiates in (see kCaseInHz). Tested rather than multiplied out for
  // the same reason the frame bank and the air layer are: at a zero return
  // level the lines would circulate a zero at the cost of eight delay reads,
  // eight writes and a one-pole each, and `0.0f * x` is not a constant the
  // optimizer may fold. A patch that asks for no case network pays nothing
  // for it.
  float late = 0.0f;
  if (kCaseLevel != 0.0f) {
    constexpr size_t kLines = static_cast<size_t>(kCaseLines);
    // Band-limit the drive before anything is injected, at the HOST rate: both
    // poles have unity gain at DC, so inside the band the network is fed exactly
    // what the two resonator banks are and no level has to be re-fitted for the
    // filter, and running it here rather than after the decimation is what makes
    // it the anti-alias filter as well.
    // Tested rather than interpolated for the same reason the blocks above are:
    // at the identity share the mix is bp exactly, with no sign-of-zero to carry
    // and no multiply to fold.
    const float case_in = kCaseDriveDirect != 0.0f ? bp + kCaseDriveDirect * (d - bp) : bp;
    case_in_1_ += case_in_a_ * (case_in - case_in_1_);
    case_in_2_ += case_in_a_ * (case_in_1_ - case_in_2_);
    if (case_phase_ == 0u) {
      // The blow enters here rather than through the two poles above, and the
      // distinction is the one kCaseInHz states: that filter is how a string's
      // SUSTAINED bridge force couples into a structure of this mass, which
      // falls with frequency well before the structure stops radiating. A
      // hammer landing is not that transfer — it is an impulse into the plate,
      // and what bounds its band is the loss grading inside the loop.
      // Tested rather than added for the same reason the blocks around it are:
      // with no blow pending the drive is case_in_2_ exactly, with no sign of
      // zero carried into the network.
      float drive = case_in_2_;
      if (case_strike_ != 0.0f) {
        drive += case_strike_;
        case_strike_ = 0.0f;
      }
      float scaled[kLines];
      float out_sum = 0.0f;
      float mix_sum = 0.0f;
      for (size_t i = 0; i < kLines; ++i) {
        float tap = case_buf_[case_off_[i] + case_idx_[i]];
        // Diffuser, ahead of the output tap so the network radiates what it
        // circulates rather than the undispersed echo. A lattice allpass: unity
        // magnitude at every frequency, so it spreads one echo over its own
        // length without touching the decay the per-line gain sets.
        if (case_ap_len_[i] != 0u) {
          const size_t p = case_ap_off_[i] + case_ap_idx_[i];
          const float stored = case_ap_buf_[p];
          const float v = tap + kCaseDiffuseG * stored;
          case_ap_buf_[p] = v;
          case_ap_idx_[i] = case_ap_idx_[i] + 1 < case_ap_len_[i] ? case_ap_idx_[i] + 1 : 0u;
          tap = stored - kCaseDiffuseG * v;
        }
        out_sum += kCaseInSign[i] * tap;
        // Damp, then attenuate, and only then mix. The matrix has to act on the
        // vector that is actually fed back: applied to the raw taps instead it
        // is no longer the orthogonal map of what circulates, and the network's
        // decay stops being the per-line gain it was designed from.
        case_lp_[i] += case_lp_a_ * (tap - case_lp_[i]);
        scaled[i] = case_g_[i] * case_lp_[i];
        mix_sum += scaled[i];
      }
      // Householder: y = x - (2/N) * sum(x). Orthogonal, so the matrix is
      // lossless and the decay is entirely the per-line gain and damping --
      // which is what lets one t60 be stated for the network rather than
      // emerging from the mixing.
      const float mix = (2.0f / static_cast<float>(kCaseLines)) * mix_sum;
      for (size_t i = 0; i < kLines; ++i) {
        case_buf_[case_off_[i] + case_idx_[i]] = scaled[i] - mix + kCaseInSign[i] * drive;
        case_idx_[i] = case_idx_[i] + 1 < case_len_[i] ? case_idx_[i] + 1 : 0u;
      }
      // The output tap sums the lines back with the same signs, so what the
      // injection decorrelated is recombined rather than left half cancelled.
      case_hold_ = out_sum;
    }
    case_phase_ = case_phase_ + 1u < kCaseDecim ? case_phase_ + 1u : 0u;
    // Smooth the held sample rather than radiating its steps.
    case_out_lp_ += case_out_a_ * (case_hold_ - case_out_lp_);
    late = case_out_lp_ * kCaseLevel;
  } else {
    // Nothing consumes the accumulator when the network is off, so it is spent
    // here rather than left to grow across a render. Folds out of a shipped
    // build, where the level above is a non-zero constexpr.
    case_strike_ = 0.0f;
  }
  float air = 0.0f;
  if (kAirGain != 0.0f) {
    float drive = d;
    if (kAirDriveBandMix != 0.0f) {
      // The same two poles and the same corner the radiated noise gets, so the
      // excitation and what it excites are shaped alike.
      air_drv_lp_ += air_lp_a_ * (d - air_drv_lp_);
      air_drv_lp2_ += air_lp_a_ * (air_drv_lp_ - air_drv_lp2_);
      air_drv_hp_ += air_hp_a_ * (air_drv_lp2_ - air_drv_hp_);
      drive = d + kAirDriveBandMix * ((air_drv_lp2_ - air_drv_hp_) - d);
    }
    const float mag = drive >= 0.0f ? drive : -drive;
    air_env_ += (mag > air_env_ ? air_attack_ : air_release_) * (mag - air_env_);
    air_rng_ = air_rng_ * 1664525u + 1013904223u;
    const float white = static_cast<float>(air_rng_ >> 8) * (1.0f / 8388608.0f) - 1.0f;
    air_lp_ += air_lp_a_ * (white - air_lp_);
    air_lp2_ += air_lp_a_ * (air_lp_ - air_lp2_);
    air_hp_ += air_hp_a_ * (air_lp2_ - air_hp_);
    air = kAirGain * air_env_ * (air_lp2_ - air_hp_);
  }
  // The late field goes through `out_gain_` with the banks. It had been added
  // outside it, on the reading that the case is a member of the instrument
  // rather than a share of the board's colour -- which was untested while the
  // network shipped at a zero level, and is wrong the moment it does not: a
  // patch that sets `piano.soundboard` to zero is asking for no body, and it
  // was still getting a four-second one. `kCaseLevel` is therefore a level
  // relative to the board's own return rather than an absolute one.
  const float out = (1.0f - kPianoDirectGain) * d + out_gain_ * (sum + late) + air;
  // Radiation split across the legs. Tested rather than multiplied out for the
  // same reason the air layer and the case network are: at a zero width the
  // two allpasses would circulate a zero at the cost of two buffer reads, two
  // writes and four multiplies, and `0.0f * x` is not a constant the optimizer
  // may fold. A patch that asks for a point source pays nothing for one.
  side_ = 0.0f;
  if (kBoardWidth != 0.0f) {
    float leg[2];
    for (int st = 0; st < 2; ++st) {
      float* buf = side_buf_[st].data();
      size_t& idx = side_idx_[st];
      const float v = out + kBoardWidthG * buf[idx];
      leg[st] = buf[idx] - kBoardWidthG * v;
      buf[idx] = v;
      idx = idx + 1 < side_len_[st] ? idx + 1 : 0;
    }
    side_ = kBoardWidth * 0.5f * (leg[0] - leg[1]);
  }
  return out;
}

}  // namespace sonare::midi::synth
