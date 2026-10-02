#pragma once

/// @file piano_voice_calibration.h
/// @brief Private calibration constants and helper declarations for the piano voice.

namespace sonare::midi::synth::piano_detail {

extern const float kHammerMfVel;
extern const float kHammerHysteresis;
extern const float kHammerDynBrightOct;
extern const float kFeltCutoffContactCycles;
extern const float kFeltCutoffVelOct;
extern const float kContactKeytrackSemis;
extern const float kContactPeriodsAtC4;
extern const float kContactPeriodsPerOct;
extern const float kContactPeriodsMax;
extern const float kInjImpulseNorm;
extern const float kContactPeriodsPerBlowMax;
extern const float kOutputLevel;
extern const float kTrebleDecayKneeOct;
extern const float kTrebleDecayOct;
extern const float kTrebleDecayFloorOct;
extern const float kTwoStageWidthOct;
extern const float kTwoStageCenterOct;
extern const float kTrebleTaperOctCap;
extern const float kTrebleBrightPerOct;
extern const float kBassDarkPerOct;
extern const float kUnisonStiffJitter;
extern const float kUnaCordaDynScale;
extern const float kUnisonStrikeUneven;
extern const float kUnisonRadSpread;
extern const float kStrikeNoiseGain;
extern const float kStrikeNoiseTauMs;
extern const float kStrikeNoiseMaxMs;
extern const float kStrikeNoiseCutoffScale;
extern const float kNoiseCutoffBassOct;
extern const float kNoiseSteepRatio;
extern const float kHammerWidthHarmonics;
extern const float kStrikeNoiseInject;
extern const float kScrubHfShelfHz;
extern const float kInjectTrebleTaperOct;
extern const float kInjectBassBoostOct;
extern const float kNoiseTrebleTaperOct;
extern const float kKnockGain;
extern const float kKnockVelExp;
extern const float kKnockThudHz;
extern const float kKnockThudBassOct;
extern const float kBloomTauMsC4;
extern const float kBloomTauOct;
extern const float kStringYield;
extern const float kInjTiltDbOct;
extern const float kInjTiltOctSpan;
extern const float kYieldTrebleOct;
extern const float kKnockBassBoostOct;
extern const float kKnockTrebleTaperOct;
extern const float kCaseStrikeGain;
extern const float kBoardStrikeGain;
extern const float kBoardStrikeTrebleOct;
extern const float kWidthBassOct;
extern const float kWidthTrebleOct;
extern const float kStrikePosBassOct;
extern const float kLongFirstHzC4;
extern const float kLongFirstOct;
extern const float kLongLevel;
extern const float kLongTrebleTaperOct;
extern const float kLongT60S;
extern const float kLongDriveHpHz;
extern const float kRadiationHpHz;
extern const float kBridgeHillHz;
extern const float kBridgeHillGainDb;
extern const float kBridgeHillQ;
extern const float kTwoStageDrainPartials;
extern const float kBridgeHfDrain;
extern const float kBridgeHfHz;
extern const float kBridgeHfRefHz;
extern const float kBridgeHfDrainMax;
extern const float kLoopSolveHf;
extern const float kLoopHfQuoteHz;
extern const float kLoopHfDecayRatio;
extern const float kModalCrossNoteLo;
extern const float kModalCrossNoteHi;
extern const float kModalDampPow;
extern const float kModalLevel;
extern const float kModalResidue;
extern const float kLoopDampRateNorm;
extern const float kLoopDampRefHz;
extern const float kDamperVelSlope;
extern const float kDamperVelAnchor;
extern const float kDamperVelScaleMax;
extern const float kDispersionFadeNoteLo;
extern const float kDispersionFadeNoteHi;
extern const float kInharmBreakNote;
extern const float kInharmBAtA4;
extern const float kInharmTrebleBeta;
extern const float kInharmBassBeta;
extern const float kStretchBassCents;
extern const float kStretchBassPower;
extern const float kStretchTrebleCents;
extern const float kStretchTreblePower;

float loop_gain_for(float period_samples, double sample_rate, float t60_s) noexcept;
float allpass_phase_delay(float a, float w) noexcept;
float onepole_phase_delay(float a, float w) noexcept;
float dispersion_allpass_a(float b_coeff, float w0, float lp_a, int stages,
                           float phase_budget) noexcept;
float partial_damp_gain(float natural, float damped, float strength) noexcept;

}  // namespace sonare::midi::synth::piano_detail
