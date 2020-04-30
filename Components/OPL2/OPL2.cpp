//
//  OPL2.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 02/04/2020.
//  Copyright © 2020 Thomas Harte. all rights reserved.
//

#include "OPL2.hpp"

#include <cassert>
#include <cmath>

using namespace Yamaha::OPL;

template <typename Child>
OPLBase<Child>::OPLBase(Concurrency::DeferringAsyncTaskQueue &task_queue) : task_queue_(task_queue) {}

template <typename Child>
void OPLBase<Child>::write(uint16_t address, uint8_t value) {
	if(address & 1) {
		static_cast<Child *>(this)->write_register(selected_register_, value);
	} else {
		selected_register_ = value;
	}
}

template class Yamaha::OPL::OPLBase<Yamaha::OPL::OPLL>;
template class Yamaha::OPL::OPLBase<Yamaha::OPL::OPL2>;


OPLL::OPLL(Concurrency::DeferringAsyncTaskQueue &task_queue, int audio_divider, bool is_vrc7): OPLBase(task_queue), audio_divider_(audio_divider) {
	// Due to the way that sound mixing works on the OPLL, the audio divider may not
	// be larger than 4.
	assert(audio_divider <= 4);

	// Install fixed instruments.
	const uint8_t *patch_set = is_vrc7 ? vrc7_patch_set : opll_patch_set;
	for(int c = 0; c < 15; ++c) {
		setup_fixed_instrument(c+1, patch_set);
		patch_set += 8;
	}

	// Install rhythm patches.
	for(int c = 0; c < 3; ++c) {
		setup_fixed_instrument(c+16, &percussion_patch_set[c * 8]);
	}

	// Set default modulators.
	for(int c = 0; c < 9; ++c) {
		channels_[c].modulator = &operators_[0];
	}
}

bool OPLL::is_zero_level() {
//	for(int c = 0; c < 9; ++c) {
//		if(channels_[c].is_audible()) return false;
//	}
	return false;
}

void OPLL::get_samples(std::size_t number_of_samples, std::int16_t *target) {
	// Both the OPLL and the OPL2 divide the input clock by 72 to get the base tick frequency;
	// unlike the OPL2 the OPLL time-divides the output for 'mixing'.

	const int update_period = 72 / audio_divider_;
	const int channel_output_period = 4 / audio_divider_;

	while(number_of_samples--) {
		if(!audio_offset_) update_all_chanels();

		*target = int16_t(output_levels_[audio_offset_ / channel_output_period]);
		++target;
		audio_offset_ = (audio_offset_ + 1) % update_period;
	}


/*	// Fill in any leftover from the previous session.
	if(audio_offset_) {
		while(audio_offset_ < update_period && number_of_samples) {
			*target = int16_t(channels_[audio_offset_ / channel_output_period].level);
			++target;
			++audio_offset_;
			--number_of_samples;
		}
		audio_offset_ = 0;
	}

	// End now if that provided everything that was asked for.
	if(!number_of_samples) return;

	int total_updates = int(number_of_samples) / update_period;
	number_of_samples %= size_t(update_period);
	audio_offset_ = int(number_of_samples);

	while(total_updates--) {
		update_all_chanels();

		for(int c = 0; c < update_period; ++c) {
			*target = int16_t(channels_[c / channel_output_period].level);
			++target;
		}
	}

	// If there are any other spots remaining, fill them.
	if(number_of_samples) {
		update_all_chanels();

		for(int c = 0; c < int(number_of_samples); ++c) {
			*target = int16_t(channels_[c / channel_output_period].level);
			++target;
		}
	}*/
}

void OPLL::set_sample_volume_range(std::int16_t range) {
	total_volume_ = range;
}

uint8_t OPLL::read(uint16_t address) {
	// I've seen mention of an undocumented two-bit status register. I don't yet know what is in it.
	return 0xff;
}

void OPLL::write_register(uint8_t address, uint8_t value) {
	// The OPLL doesn't have timers or other non-audio functions, so all writes
	// go to the audio queue.
	task_queue_.defer([this, address, value] {
		// The first 8 locations are used to define the custom instrument, and have
		// exactly the same format as the patch set arrays at the head of this file.
		if(address < 8) {
			custom_instrument_[address] = value;

			// Update whatever that did to the instrument.
			setup_fixed_instrument(0, custom_instrument_);
			return;
		}

		// Register 0xe is a cut-down version of the OPLL's register 0xbd.
		if(address == 0xe) {
			depth_rhythm_control_ = value & 0x3f;
//			if(depth_rhythm_control_ & 0x08)
//				printf("%02x\n", depth_rhythm_control_);
			return;
		}

		const auto index = address & 0xf;
		if(index > 8) return;

		switch(address & 0xf0) {
			case 0x30:
				// Select an instrument in the top nibble, set a channel volume in the lower.
				channels_[index].overrides.attenuation = value & 0xf;
				channels_[index].modulator = &operators_[(value >> 4) * 2];

				// Also crib volume levels for rhythm mode, possibly.
				if(index >= 6) {
					rhythm_overrides_[(index - 6) * 2 + 0].attenuation = value >> 4;
					rhythm_overrides_[(index - 6) * 2 + 1].attenuation = value & 0xf;
				}
			break;

			case 0x10:	channels_[index].set_frequency_low(value);	break;

			case 0x20:
				// Set sustain on/off, key on/off, octave and a single extra bit of frequency.
				// So they're a lot like OPLL registers 0xb0 to 0xb8, but not identical.
				channels_[index].set_9bit_frequency_octave_key_on(value);
				channels_[index].overrides.use_sustain_level = value & 0x20;
			break;

			default: printf("Unknown write to %02x?!?\n", address); break;
		}
	});
}

void OPLL::setup_fixed_instrument(int number, const uint8_t *data) {
	auto modulator = &operators_[number * 2];
	auto carrier = &operators_[number * 2 + 1];

	modulator->set_am_vibrato_hold_sustain_ksr_multiple(data[0]);
	carrier->set_am_vibrato_hold_sustain_ksr_multiple(data[1]);
	modulator->set_scaling_output(data[2]);

	// Set waveforms — only sine and halfsine are available.
	modulator->set_waveform((data[3] >> 3) & 1);
	carrier->set_waveform((data[3] >> 4) & 1);

	// TODO: data[3] b0-b2: modulator feedback level
	// TODO: data[3] b6, b7: carrier key-scale level

	// Set ADSR parameters.
	modulator->set_attack_decay(data[4]);
	carrier->set_attack_decay(data[5]);
	modulator->set_sustain_release(data[6]);
	carrier->set_sustain_release(data[7]);
}

void OPLL::update_all_chanels() {
	// Update the LFO and then the channels.
	oscillator_.update();
	for(int c = 0; c < 6; ++c) {
		channels_[c].update(oscillator_);
		oscillator_.update_lfsr();	// TODO: should update per slot, not per channel? Or even per cycle?
	}

	output_levels_[8] = output_levels_[12] = 0;

#define VOLUME(x)	((x) * total_volume_) >> 12

	// Channels that are updated for melodic output regardless;
	// in rhythm mode the final three channels — 6, 7, and 8 —
	// are lost as their operators are used for drum noises.
	output_levels_[3] = VOLUME(channels_[0].melodic_output());
	output_levels_[4] = VOLUME(channels_[1].melodic_output());
	output_levels_[5] = VOLUME(channels_[2].melodic_output());

	output_levels_[9] = VOLUME(channels_[3].melodic_output());
	output_levels_[10] = VOLUME(channels_[4].melodic_output());
	output_levels_[11] = VOLUME(channels_[5].melodic_output());

	if(depth_rhythm_control_ & 0x20) {
		// TODO: pervasively, volume. And LFSR updates.

		channels_[6].update(oscillator_, &operators_[32], depth_rhythm_control_ & 0x10);
		channels_[7].update(true, oscillator_, operators_[34], bool(depth_rhythm_control_ & 0x01));
		channels_[7].update(false, oscillator_, operators_[35], bool(depth_rhythm_control_ & 0x08));
		channels_[8].update(true, oscillator_, operators_[36], bool(depth_rhythm_control_ & 0x04));
		channels_[8].update(false, oscillator_, operators_[37], bool(depth_rhythm_control_ & 0x02));

		// Update channel 6 as if melodic, but with the bass instrument.
		output_levels_[2] = output_levels_[15] = VOLUME(channels_[6].melodic_output());

		// Use the carrier from channel 7 for the snare.
		output_levels_[6] = output_levels_[16] = VOLUME(channels_[7].snare_output(operators_[35]));

		// Use the modulator from channel 8 for the tom tom.
		output_levels_[1] = output_levels_[14] = VOLUME(channels_[8].tom_tom_output(operators_[37]));

		// Use the channel 7 modulator and the channel 8 carrier for a cymbal.
		output_levels_[7] = output_levels_[17] = VOLUME(channels_[7].cymbal_output(operators_[36], operators_[35], channels_[8]));

		// Use the channel 7 modulator and the channel 8 modulator (?) for a high-hat.
		output_levels_[0] = output_levels_[13] = VOLUME(channels_[7].high_hat_output(operators_[36], operators_[35], channels_[8]));
	} else {
		// Not in rhythm mode; channels 7, 8 and 9 are melodic.
		for(int c = 6; c < 9; ++ c) {
			channels_[c].update(oscillator_);
			oscillator_.update_lfsr();	// TODO: should update per slot, not per channel? Or even per cycle?
		}

		output_levels_[0] = output_levels_[1] = output_levels_[2] =
		output_levels_[6] = output_levels_[7] =
		output_levels_[13] = output_levels_[14] = 0;

		output_levels_[15] = VOLUME(channels_[6].melodic_output());
		output_levels_[16] = VOLUME(channels_[7].melodic_output());
		output_levels_[17] = VOLUME(channels_[8].melodic_output());
	}

	// Test!
//	for(int c = 0; c < 18; ++c) {
//		if(c != 6 && c != 16)
//			output_levels_[c] = 0;
//	}

//	channels_[2].level = (channels_[2].update() * total_volume_) >> 14;

#undef VOLUME
}

/*
template <Personality personality>
void OPL2<personality>::get_samples(std::size_t number_of_samples, std::int16_t *target) {
	// TODO.
	//  out = exp(logsin(phase2 + exp(logsin(phase1) + gain1)) + gain2)

//		Melodic channels are:
//
//		Channel		Operator 1		Operator 2
//		0			0				3
//		1			1				4
//		2			2				5
//		3			6				9
//		4			7				10
//		5			8				11
//		6			12				15
//		7			13				16
//		8			14				17
//
//		In percussion mode, only channels 0–5 are use as melodic, with 6, 7 and 8 being
//		replaced by:
//
//		Bass drum, using operators 12 and 15;
//		Snare, using operator 16;
//		Tom tom, using operator 14,
//		Cymbal, using operator 17; and
//		Symbol, using operator 13.
}

*/

void OPL2::write_register(uint8_t address, uint8_t value) {

	// Deal with timer changes synchronously.
	switch(address) {
		case 0x02:	timers_[0] = value; 	return;
		case 0x03:	timers_[1] = value;		return;
		case 0x04:	timer_control_ = value;	return;
		// TODO from register 4:
		//	b7 = IRQ reset;
		//	b6/b5 = timer 1/2 mask (irq enabling flags, I think?)
		//	b4/b3 = timer 2/1 start (seemingly the opposite order to b6/b5?)

		default: break;
	}

	// Enqueue any changes that affect audio output.
	task_queue_.enqueue([this, address, value] {
		//
		// Modal modifications.
		//

		switch(address) {
			case 0x01:	waveform_enable_ = value & 0x20;	break;
			case 0x08:
				// b7: "composite sine wave mode on/off"?
				csm_keyboard_split_ = value;
				// b6: "Controls the split point of the keyboard. When 0, the keyboard split is the
				// second bit from the bit 8 of the F-Number. When 1, the MSB of the F-Number is used."
			break;
			case 0xbd:	depth_rhythm_control_ = value;		break;

			default: break;
		}


		//
		// Operator modifications.
		//

		if((address >= 0x20 && address < 0xa0) || address >= 0xe0) {
			// The 18 operators are spreat out across 22 addresses; each group of
			// six is framed within an eight-byte area thusly:
			constexpr int operator_by_address[] = {
				0,	1,	2,	3,	4,	5,	-1,	-1,
				6,	7,	8,	9,	10,	11,	-1,	-1,
				12,	13,	14,	15,	16,	17,	-1,	-1,
				-1,	-1, -1, -1, -1, -1, -1, -1,
			};

			const auto index = operator_by_address[address & 0x1f];
			if(index == -1) return;

			switch(address & 0xe0) {
				case 0x20:	operators_[index].set_am_vibrato_hold_sustain_ksr_multiple(value);	break;
				case 0x40:	operators_[index].set_scaling_output(value);						break;
				case 0x60:	operators_[index].set_attack_decay(value);							break;
				case 0x80:	operators_[index].set_sustain_release(value);						break;
				case 0xe0:	operators_[index].set_waveform(value);								break;

				default: break;
			}
		}


		//
		// Channel modifications.
		//

		const auto index = address & 0xf;
		if(index > 8) return;

		switch(address & 0xf0) {
			case 0xa0:	channels_[index].set_frequency_low(value);					break;
			case 0xb0:	channels_[index].set_10bit_frequency_octave_key_on(value);	break;
			case 0xc0:	channels_[index].set_feedback_mode(value);					break;

			default: break;
		}
	});
}

uint8_t OPL2::read(uint16_t address) {
	// TODO. There's a status register where:
	//	b7 = IRQ status (set if interrupt request ongoing)
	//	b6 = timer 1 flag (set if timer 1 expired)
	//	b5 = timer 2 flag
	return 0xff;
}
