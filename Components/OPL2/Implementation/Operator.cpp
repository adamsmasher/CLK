//
//  Operator.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 15/04/2020.
//  Copyright © 2020 Thomas Harte. All rights reserved.
//

#include "Operator.hpp"

#include <algorithm>
#include "Tables.h"

using namespace Yamaha::OPL;

int OperatorState::level() {
	return power_two(attenuation.logsin) * attenuation.sign;
}

void Operator::set_attack_decay(uint8_t value) {
	attack_rate_ = (value & 0xf0) >> 2;
	decay_rate_ = (value & 0x0f) << 2;
}

void Operator::set_sustain_release(uint8_t value) {
	sustain_level_ = (value & 0xf0) >> 4;
	release_rate_ = (value & 0x0f) << 2;
}

void Operator::set_scaling_output(uint8_t value) {
	level_key_scaling_ = value >> 6;
	attenuation_ = value & 0x3f;
}

void Operator::set_waveform(uint8_t value) {
	waveform_ = Operator::Waveform(value & 3);
}

void Operator::set_am_vibrato_hold_sustain_ksr_multiple(uint8_t value) {
	apply_amplitude_modulation_ = value & 0x80;
	apply_vibrato_ = value & 0x40;
	use_sustain_level_ = value & 0x20;
	key_scaling_rate_ = value & 0x10;
	frequency_multiple_ = value & 0xf;
}

bool Operator::is_audible(OperatorState &state, OperatorOverrides *overrides) {
	if(state.adsr_phase_ == OperatorState::ADSRPhase::Release) {
		if(overrides) {
			if(overrides->attenuation == 0xf) return false;
		} else {
			if(attenuation_ == 0x3f) return false;
		}
	}
	return state.adsr_attenuation_ != 511;
}

void Operator::update(OperatorState &state, bool key_on, int channel_period, int channel_octave, int phase_offset, OperatorOverrides *overrides) {
	// Per the documentation:
	//
	// Delta phase = ( [desired freq] * 2^19 / [input clock / 72] ) / 2 ^ (b - 1)
	//
	// After experimentation, I think this gives rate calculation as formulated below.

	// This encodes the MUL -> multiple table given on page 12,
	// multiplied by two.
	constexpr int multipliers[] = {
		1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30
	};

	// Update the raw phase.
	state.raw_phase_ += multipliers[frequency_multiple_] * channel_period << channel_octave;

	// Hence calculate phase (TODO: by also taking account of vibrato).
	constexpr int waveforms[4][4] = {
		{1023, 1023, 1023, 1023},	// Sine: don't mask in any quadrant.
		{511, 511, 0, 0},			// Half sine: keep the first half in tact, lock to 0 in the second half.
		{511, 511, 511, 511},		// AbsSine: endlessly repeat the first half of the sine wave.
		{255, 0, 255, 0},			// PulseSine: act as if the first quadrant is in the first and third; lock the other two to 0.
	};
	const int phase = (state.raw_phase_ >> 12) + phase_offset;
	state.attenuation = negative_log_sin(phase & waveforms[int(waveform_)][(phase >> 8) & 3]);

	// Key-on logic: any time it is false, be in the release state.
	// On the leading edge of it becoming true, enter the attack state.
	if(!key_on) {
		state.adsr_phase_ = OperatorState::ADSRPhase::Release;
		state.time_in_phase_ = 0;
	} else if(!state.last_key_on_) {
		state.adsr_phase_ = OperatorState::ADSRPhase::Attack;
		state.time_in_phase_ = 0;
	}
	state.last_key_on_ = key_on;

	// Adjust the ADSR attenuation appropriately;
	// cf. http://forums.submarine.org.uk/phpBB/viewtopic.php?f=9&t=16 (primarily) for the source of the maths below.

	// "An attack rate value of 52 (AR = 13) has 32 samples in the attack phase, an attack rate value of 48 (AR = 12)
	// has 64 samples in the attack phase, but pairs of samples show the same envelope attenuation. I am however struggling to find a plausible algorithm to match the experimental results.

	const auto current_phase = state.adsr_phase_;
	switch(current_phase) {
		case OperatorState::ADSRPhase::Attack: {
			const int attack_rate = attack_rate_;	// TODO: key scaling rate. Which I do not yet understand.

			// Rules:
			//
			// An attack rate of '13' has 32 samples in the attack phase; a rate of '12' has the same 32 steps, but spread out over 64 samples, etc.
			// An attack rate of '14' uses a divide by four instead of two.
			// 15 is instantaneous.

			if(attack_rate >= 56) {
				state.adsr_attenuation_ = state.adsr_attenuation_ - (state.adsr_attenuation_ >> 2) - 1;
			} else {
				const int sample_length = 1 << (14 - (attack_rate >> 2));	// TODO: don't throw away KSR bits.
				if(!(state.time_in_phase_ & (sample_length - 1))) {
					state.adsr_attenuation_ = state.adsr_attenuation_ - (state.adsr_attenuation_ >> 3) - 1;
				}
			}

			// Two possible terminating conditions: (i) the attack rate is 15; (ii) full volume has been reached.
			if(attack_rate > 60 || state.adsr_attenuation_ <= 0) {
				state.adsr_attenuation_ = 0;
				state.adsr_phase_ = OperatorState::ADSRPhase::Decay;
			}
		} break;

		case OperatorState::ADSRPhase::Release:
		case OperatorState::ADSRPhase::Decay:
		{
			// Rules:
			//
			// (relative to a 511 scale)
			//
			// A rate of 0 is no decay at all.
			// A rate of 1 means increase 4 per cycle.
			// A rate of 2 means increase 2 per cycle.
			// A rate of 3 means increase 1 per cycle.
			// A rate of 4 means increase 1 every other cycle.
			// (etc)
			const int decrease_rate = (state.adsr_phase_ == OperatorState::ADSRPhase::Decay) ? decay_rate_ : release_rate_;	// TODO: again, key scaling rate.

			if(decrease_rate) {
				// TODO: don't throw away KSR bits.
				switch(decrease_rate >> 2) {
					case 1: state.adsr_attenuation_ += 4;	break;
					case 2: state.adsr_attenuation_ += 2;	break;
					default: {
						const int sample_length = 1 << ((decrease_rate >> 2) - 4);
						if(!(state.time_in_phase_ & (sample_length - 1))) {
							++state.adsr_attenuation_;
						}
					} break;
				}
			}

			// Clamp to the proper range.
			state.adsr_attenuation_ = std::min(state.adsr_attenuation_, 511);

			// Check for the decay exit condition.
			if(state.adsr_phase_ == OperatorState::ADSRPhase::Decay && state.adsr_attenuation_ >= (sustain_level_ << 5)) {
				state.adsr_attenuation_ = sustain_level_ << 5;
				state.adsr_phase_ = ((overrides && overrides->use_sustain_level) || use_sustain_level_) ? OperatorState::ADSRPhase::Sustain : OperatorState::ADSRPhase::Release;
			}
		} break;

		case OperatorState::ADSRPhase::Sustain:
			// Nothing to do.
		break;
	}
	if(state.adsr_phase_ == current_phase) {
		++state.time_in_phase_;
	} else {
		state.time_in_phase_ = 0;
	}

	// Combine the ADSR attenuation and overall channel attenuation.
	if(overrides) {
		// Overrides here represent per-channel volume on an OPLL. The bits are defined to represent
		// attenuations of 24db to 3db; the main envelope generator is stated to have a resolution of
		// 0.325db (which I've assumed is supposed to say 0.375db).
		state.attenuation.logsin += state.adsr_attenuation_ + (overrides->attenuation << 4);
	} else {
		// Overrides here represent per-channel volume on an OPLL. The bits are defined to represent
		// attenuations of 24db to 0.75db.
		state.attenuation.logsin += (state.adsr_attenuation_ << 3) + (attenuation_ << 5);
	}
}
