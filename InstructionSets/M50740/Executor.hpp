//
//  Executor.h
//  Clock Signal
//
//  Created by Thomas Harte on 1/16/21.
//  Copyright © 2021 Thomas Harte. All rights reserved.
//

#ifndef Executor_h
#define Executor_h

#include "Instruction.hpp"
#include "Parser.hpp"

namespace InstructionSet {
namespace M50740 {

class Executor;

class Executor {
	public:
		/*!
			M50740 actions require no further context; the addressing mode and operation is baked in,
			so using the Executor to enquire of memory and the program counter is sufficient.
		*/
		struct Action {
			using Performer = void (*)(Executor *);
			Performer perform = nullptr;
		};

		Action action_for(Instruction);

	private:
		/*!
			Performs @c operation using @c operand as the value fetched from memory, if any.
		*/
		template <Operation operation> static void perform(uint8_t *operand);

		/*!
			Performs @c operation in @c addressing_mode.
		*/
		template <Operation operation, AddressingMode addressing_mode> static void perform(Executor *);

		/*!
			Provides dynamic lookup of @c perform(Executor*).
		*/
		class PerformerLookup {
			public:
				PerformerLookup() {
					fill<int(MinOperation), int(MinAddressingMode)>(performers);
				}

				Action::Performer performer(Operation operation, AddressingMode addressing_mode) {
					return performers[int(addressing_mode) * (MaxOperation - MinOperation) + int(operation) - MinOperation];
				}

			private:
				Action::Performer performers[(MaxAddressingMode - MinAddressingMode) * (MaxOperation - MinOperation)];

				template<int operation, int addressing_mode> void fill_operation(Action::Performer *target) {
					*target = &Executor::perform<Operation(operation), AddressingMode(addressing_mode)>;
					if constexpr (addressing_mode+1 < MaxAddressingMode) {
						fill<operation, addressing_mode+1>(target + 1);
					}
				}

				template<int operation, int addressing_mode> void fill(Action::Performer *target) {
					fill_operation<operation, addressing_mode>(target);
					target += MaxOperation - MinOperation;
					if constexpr (operation+1 < MaxOperation) {
						fill<operation+1, addressing_mode>(target);
					}
				}
		};

		inline static PerformerLookup performer_lookup_;
};

}
}

#endif /* Executor_h */
