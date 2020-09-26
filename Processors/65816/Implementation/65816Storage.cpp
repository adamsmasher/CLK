//
//  65816Storage.cpp
//  Clock Signal
//
//  Created by Thomas Harte on 23/09/2020.
//  Copyright © 2020 Thomas Harte. All rights reserved.
//

#include "../65816.hpp"
#include <map>

using namespace CPU::WDC65816;

struct CPU::WDC65816::ProcessorStorageConstructor {
	// Establish that a storage constructor needs access to ProcessorStorage.
	ProcessorStorage &storage_;
	ProcessorStorageConstructor(ProcessorStorage &storage) : storage_(storage) {}

	enum class AccessType {
		Read, Write
	};

	constexpr static AccessType access_type_for_operation(Operation operation) {
		switch(operation) {
			case ADC:	case AND:	case BIT:	case CMP:
			case CPX:	case CPY:	case EOR:	case ORA:
			case SBC:

			case LDA:	case LDX:	case LDY:

			// The access type for the rest of these ::Reads is arbitrary; they're
			// not relevantly either read or write.
			case JMP:	case JSR:	case JML:	case JSL:

			case ASL:	case DEC:	case INC:	case LSR:
			case ROL:	case ROR:	case TRB:	case TSB:

			case MVN:	case MVP:

			return AccessType::Read;

			case STA:	case STX:	case STY:	case STZ:
			return AccessType::Write;
		}
	}

	typedef void (* Generator)(AccessType, bool, const std::function<void(MicroOp)>&);
	using GeneratorKey = std::tuple<AccessType, Generator>;
	std::map<GeneratorKey, std::pair<size_t, size_t>> installed_patterns;

	uint8_t opcode = 0;
	void install(Generator generator, Operation operation) {
		// Determine the access type implied by this operation.
		const AccessType access_type = access_type_for_operation(operation);

		// Check whether this access type + addressing mode generator has already been generated.
		const auto key = std::make_pair(access_type, generator);
		const auto map_entry = installed_patterns.find(key);
		size_t micro_op_location_8, micro_op_location_16;

		// If it wasn't found, generate it now in both 8- and 16-bit variants.
		// Otherwise, get the location of the existing entries.
		if(map_entry == installed_patterns.end()) {
			// Generate 8-bit steps.
			micro_op_location_8 = storage_.micro_ops_.size();
			(*generator)(access_type, true, [this] (MicroOp op) {
				this->storage_.micro_ops_.push_back(op);
			});
			storage_.micro_ops_.push_back(OperationMoveToNextProgram);

			// Generate 16-bit steps.
			micro_op_location_16 = storage_.micro_ops_.size();
			(*generator)(access_type, false, [this] (MicroOp op) {
				this->storage_.micro_ops_.push_back(op);
			});
			storage_.micro_ops_.push_back(OperationMoveToNextProgram);

			// Minor optimisation: elide the steps if 8- and 16-bit steps are equal.
			bool are_equal = true;
			size_t c = 0;
			while(true) {
				if(storage_.micro_ops_[micro_op_location_8 + c] != storage_.micro_ops_[micro_op_location_16 + c]) {
					are_equal = false;
					break;
				}
				if(storage_.micro_ops_[micro_op_location_8 + c] == OperationMoveToNextProgram) break;
				++c;
			}

			if(are_equal) {
				storage_.micro_ops_.resize(micro_op_location_16);
				micro_op_location_16 = micro_op_location_8;
			}

			// Insert into the map.
			installed_patterns[key] = std::make_pair(micro_op_location_8, micro_op_location_16);
		} else {
			micro_op_location_8 = map_entry->second.first;
			micro_op_location_16 = map_entry->second.second;
		}

		// Fill in the proper table entries and increment the opcode pointer.
		storage_.instructions[opcode].program_offset = micro_op_location_8;
		storage_.instructions[opcode].operation = operation;

		storage_.instructions[opcode + 256].program_offset = micro_op_location_16;
		storage_.instructions[opcode + 256].operation = operation;

		++opcode;
	}

	/*
		Code below is structured to ease translation from Table 5-7 of the 2018
		edition of the WDC 65816 datasheet.

		In each case the relevant addressing mode is described here via a generator
		function that will spit out the correct MicroOps based on access type
		(i.e. read, write or read-modify-write) and data size (8- or 16-bit).

		That leads up to being able to declare the opcode map by addressing mode
		and operation alone.

		Things the generators can assume before they start:

			1) the opcode has already been fetched and decoded, and the program counter incremented;
			2) the data buffer is empty; and
			3) the data address is undefined.
	*/

	// Performs the closing 8- or 16-bit read or write common to many modes below.
	static void read_write(AccessType type, bool is8bit, const std::function<void(MicroOp)> &target) {
		if(type == AccessType::Write) {
			target(OperationPerform);						// Perform operation to fill the data buffer.
			if(!is8bit) target(CycleStoreIncrementData);	// Data low.
			target(CycleStoreData);							// Data [high].
		} else {
			if(!is8bit) target(CycleFetchIncrementData);	// Data low.
			target(CycleFetchData);							// Data [high].
			target(OperationPerform);						// Perform operation from the data buffer.
		}
	}

	static void read_modify_write(bool is8bit, const std::function<void(MicroOp)> &target) {
		if(!is8bit)	target(CycleFetchIncrementData);	// Data low.
		target(CycleFetchData);							// Data [high].

		if(!is8bit)	target(CycleFetchData);				// 16-bit: reread final byte of data.
		else target(CycleStoreData);					// 8-bit rewrite final byte of data.

		target(OperationPerform);						// Perform operation within the data buffer.

		if(!is8bit)	target(CycleStoreDecrementData);	// Data high.
		target(CycleStoreData);							// Data [low].
	}

	// 1a. Absolute; a.
	static void absolute(AccessType type, bool is8bit, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);					// AAL.
		target(CycleFetchIncrementPC);					// AAH.
		target(OperationConstructAbsolute);				// Calculate data address.

		read_write(type, is8bit, target);
	};

	// 1b. Absolute; a, JMP.
	static void absolute_jmp(AccessType, bool, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);			// New PCL.
		target(CycleFetchPC);					// New PCH.
		target(OperationConstructAbsolute);		// Calculate data address.
		target(OperationPerform);				// [JMP]
	};

	// 1c. Absolute; a, JSR.
	static void absolute_jsr(AccessType, bool, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);			// New PCL.
		target(CycleFetchPC);					// New PCH.
		target(CycleFetchPC);					// IO
		target(OperationConstructAbsolute);		// Calculate data address.
		target(OperationPerform);				// [JSR]
		target(CyclePush);						// PCH
		target(CyclePush);						// PCL
	};

	// 1d. Absolute; a, read-modify-write.
	static void absolute_rmw(AccessType, bool is8bit, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);					// AAL.
		target(CycleFetchIncrementPC);					// AAH.
		target(OperationConstructAbsolute);				// Calculate data address.

		read_modify_write(is8bit, target);
	};

	// 2a. Absolute Indexed Indirect; (a, x), JMP.
	static void absolute_indexed_indirect_jmp(AccessType, bool, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);						// AAL.
		target(CycleFetchPC);								// AAH.
		target(CycleFetchPC);								// IO.
		target(OperationConstructAbsoluteIndexedIndirect);	// Calculate data address.
		target(CycleFetchIncrementData);					// New PCL
		target(CycleFetchData);								// New PCH.
		target(OperationPerform);							// [JMP]
	};

	// 2b. Absolute Indexed Indirect; (a, x), JSR.
	static void absolute_indexed_indirect_jsr(AccessType, bool, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);						// AAL.

		target(OperationCopyPCToData);						// Prepare to push.
		target(CyclePush);									// PCH
		target(CyclePush);									// PCL

		target(CycleFetchPC);								// AAH.
		target(CycleFetchPC);								// IO.

		target(OperationConstructAbsoluteIndexedIndirect);	// Calculate data address.
		target(CycleFetchIncrementData);					// New PCL
		target(CycleFetchData);								// New PCH.
		target(OperationPerform);							// ['JSR' (actually: JMP will do)]
	}

	// 3a. Absolute Indirect; (a), JML.
	static void absolute_indirect_jml(AccessType, bool, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);			// New AAL.
		target(CycleFetchPC);					// New AAH.

		target(OperationConstructAbsolute);		// Calculate data address.
		target(CycleFetchIncrementData);		// New PCL
		target(CycleFetchIncrementData);		// New PCH
		target(CycleFetchData);					// New PBR

		target(OperationPerform);				// [JML]
	};

	// 3b. Absolute Indirect; (a), JMP.
	static void absolute_indirect_jmp(AccessType, bool, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);			// New AAL.
		target(CycleFetchPC);					// New AAH.

		target(OperationConstructAbsolute);		// Calculate data address.
		target(CycleFetchIncrementData);		// New PCL
		target(CycleFetchData);					// New PCH

		target(OperationPerform);				// [JMP]
	};

	// 4a. Absolute long; al.
	static void absolute_long(AccessType type, bool is8bit, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);			// AAL.
		target(CycleFetchIncrementPC);			// AAH.
		target(CycleFetchPC);					// AAB.

		target(OperationConstructAbsolute);		// Calculate data address.

		read_write(type, is8bit, target);
	};

	// 4b. Absolute long; al, JMP.
	static void absolute_long_jmp(AccessType, bool, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);			// New PCL.
		target(CycleFetchIncrementPC);			// New PCH.
		target(CycleFetchPC);					// New PBR.

		target(OperationConstructAbsolute);		// Calculate data address.
		target(OperationPerform);				// ['JMP' (though it's JML in internal terms)]
	};

	// 4c. Absolute long al, JSL.
	static void absolute_long_jsl(AccessType, bool, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);			// New PCL.
		target(CycleFetchIncrementPC);			// New PCH.

		target(OperationCopyPBRToData);			// Copy PBR to the data register.
		target(CyclePush);						// PBR.
		target(CycleAccessStack);				// IO.

		target(CycleFetchIncrementPC);			// New PBR.

		target(OperationConstructAbsolute);		// Calculate data address.
		target(OperationPerform);				// [JSL]

		target(CyclePush);						// PCH
		target(CyclePush);						// PCL
	};

	// 5. Absolute long, X;	al, x.
	static void absolute_long_x(AccessType type, bool is8bit, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);			// AAL.
		target(CycleFetchIncrementPC);			// AAH.
		target(CycleFetchIncrementPC);			// AAB.

		target(OperationConstructAbsoluteLongX);		// Calculate data address.

		read_write(type, is8bit, target);
	}

	// 6a. Absolute, X;	a, x.
	static void absolute_x(AccessType type, bool is8bit, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);			// AAL.
		target(CycleFetchIncrementPC);			// AAH.

		if(type == AccessType::Read) {
			target(OperationConstructAbsoluteXRead);	// Calculate data address, potentially skipping the next fetch.
		} else {
			target(OperationConstructAbsoluteX);		// Calculate data address.
		}
		target(CycleFetchIncorrectDataAddress);	// Do the dummy read if necessary; OperationConstructAbsoluteX
												// will skip this if it isn't required.

		read_write(type, is8bit, target);
	}

	// 6b. Absolute, X;	a, x, read-modify-write.
	static void absolute_x_rmw(AccessType, bool is8bit, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);					// AAL.
		target(CycleFetchIncrementPC);					// AAH.

		target(OperationConstructAbsoluteX);			// Calculate data address.
		target(CycleFetchIncorrectDataAddress);			// Perform dummy read.

		read_modify_write(is8bit, target);
	}

	// 7. Absolute, Y;	a, y.
	static void absolute_y(AccessType type, bool is8bit, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);			// AAL.
		target(CycleFetchIncrementPC);			// AAH.

		if(type == AccessType::Read) {
			target(OperationConstructAbsoluteYRead);	// Calculate data address, potentially skipping the next fetch.
		} else {
			target(OperationConstructAbsoluteY);		// Calculate data address.
		}
		target(CycleFetchIncorrectDataAddress);	// Do the dummy read if necessary; OperationConstructAbsoluteX
												// will skip this if it isn't required.

		read_write(type, is8bit, target);
	}

	// 8. Accumulator; A.
	static void accumulator(AccessType, bool, const std::function<void(MicroOp)> &target) {
		target(CycleFetchPC);			// IO.

		// TODO: seriously consider a-specific versions of all relevant operations;
		// the cost of interpreting three things here is kind of silly.
		target(OperationCopyAToData);
		target(OperationPerform);
		target(OperationCopyDataToA);
	}

	// 9a. Block Move Negative [and]
	// 9b. Block Move Positive.
	//
	// These don't fit the general model very well at all, hence the specialised fetch and store cycles.
	static void block_move(AccessType, bool, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);	// DBA.
		target(CycleFetchIncrementPC);	// SBA.

		target(CycleFetchBlockX);		// SRC Data.
		target(CycleStoreBlockY);		// Dest Data.

		target(CycleFetchBlockY);		// IO.
		target(CycleFetchBlockY);		// IO.

		target(OperationPerform);		// [MVN or MVP]
	}

	// 10a. Direct; d.
	// (That's zero page in 6502 terms)
	static void direct(AccessType type, bool is8bit, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);					// DO.

		target(OperationConstructDirect);
		target(CycleFetchPC);							// IO.

		read_write(type, is8bit, target);
	};

	// 10b. Direct; d, read-modify-write.
	// (That's zero page in 6502 terms)
	static void direct_rmw(AccessType, bool is8bit, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);					// DO.

		target(OperationConstructDirect);
		target(CycleFetchPC);							// IO.

		read_modify_write(is8bit, target);
	};

	// 11. Direct Indexed Indirect; (d, x).
	static void direct_indexed_indirect(AccessType type, bool is8bit, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);					// DO.

		target(OperationConstructDirectIndexedIndirect);
		target(CycleFetchPC);							// IO.

		target(CycleFetchPC);							// IO.

		read_write(type, is8bit, target);
	};

	// 12. Direct Indirect; (d).
	static void direct_indirect(AccessType type, bool is8bit, const std::function<void(MicroOp)> &target) {
		target(CycleFetchIncrementPC);					// DO.

		target(OperationConstructDirectIndirect);
		target(CycleFetchPC);							// IO.

		read_write(type, is8bit, target);
	};

	// 13. Direct Indirect Indexed; (d), y.
	// 14. Direct Indirect Indexed Long; [d], y.
	// 15. Direct Indirect Long; [d].
};

// TEMPORARY. Kneejerk way to get a step debug of 65816 storage construction.
ProcessorStorage TEMPORARY_test_instance;

ProcessorStorage::ProcessorStorage() {
	ProcessorStorageConstructor constructor(*this);

	// Install the instructions.
#define op(x, y) constructor.install(&ProcessorStorageConstructor::x, y)

	/* 0x00 BRK s */
	/* 0x01 ORA (d, x) */		op(direct_indexed_indirect, ORA);
	/* 0x02 COP s */
	/* 0x03 ORA d, s */
	/* 0x04 TSB d */			op(direct_rmw, TSB);
	/* 0x05 ORA d */			op(direct, ORA);
	/* 0x06 ASL d */			op(direct_rmw, ASL);
	/* 0x07 ORA [d] */
	/* 0x08 PHP s */
	/* 0x09 ORA # */
	/* 0x0a ASL A */			op(accumulator, ASL);
	/* 0x0b PHD s */
	/* 0x0c TSB a */			op(absolute_rmw, TSB);
	/* 0x0d ORA a */			op(absolute, ORA);
	/* 0x0e ASL a */			op(absolute_rmw, ASL);
	/* 0x0f ORA al */			op(absolute_long, ORA);

	/* 0x10 BPL r */
	/* 0x11 ORA (d), y */
	/* 0x12 ORA (d) */			op(direct_indirect, ORA);
	/* 0x13 ORA (d, s), y */
	/* 0x14 TRB d */			op(absolute_rmw, TRB);
	/* 0x15 ORA d, x */
	/* 0x16 ASL d, x */
	/* 0x17 ORA [d], y */
	/* 0x18 CLC i */
	/* 0x19 ORA a, y */			op(absolute_y, ORA);
	/* 0x1a INC A */			op(accumulator, INC);
	/* 0x1b TCS i */
	/* 0x1c TRB a */			op(absolute_rmw, TRB);
	/* 0x1d ORA a, x */			op(absolute_x, ORA);
	/* 0x1e ASL a, x */			op(absolute_x_rmw, ASL);
	/* 0x1f ORA al, x */		op(absolute_long_x, ORA);

	/* 0x20 JSR a */			op(absolute_jsr, JSR);
	/* 0x21 ORA (d), y */
	/* 0x22 AND (d, x) */		op(direct_indexed_indirect, AND);
	/* 0x23 JSL al */			op(absolute_long_jsl, JSL);
	/* 0x24 BIT d */			op(direct, BIT);
	/* 0x25 AND d */			op(direct, AND);
	/* 0x26 ROL d */			op(absolute_rmw, ROL);
	/* 0x27 AND [d] */
	/* 0x28 PLP s */
	/* 0x29 AND # */
	/* 0x2a ROL A */			op(accumulator, ROL);
	/* 0x2b PLD s */
	/* 0x2c BIT a */			op(absolute, BIT);
	/* 0x2d AND a */			op(absolute, AND);
	/* 0x2e ROL a */			op(absolute_rmw, ROL);
	/* 0x2f AND al */			op(absolute_long, AND);

	/* 0x30 BMI R */
	/* 0x31 AND (d), y */
	/* 0x32 AND (d) */			op(direct_indirect, AND);
	/* 0x33 AND (d, s), y */
	/* 0x34 BIT d, x */
	/* 0x35 AND d, x */
	/* 0x36 ROL d, x */			op(absolute_x_rmw, ROL);
	/* 0x37 AND [d], y */
	/* 0x38 SEC i */
	/* 0x39 AND a, y */			op(absolute_y, AND);
	/* 0x3a DEC A */			op(accumulator, DEC);
	/* 0x3b TSC i */
	/* 0x3c BIT a, x */			op(absolute_x, BIT);
	/* 0x3d AND a, x */			op(absolute_x, AND);
	/* 0x3e TLD a, x */
	/* 0x3f AND al, x */		op(absolute_long_x, AND);

	/* 0x40 RTI s */
	/* 0x41	EOR (d, x) */		op(direct_indexed_indirect, EOR);
	/* 0x42	WDM i */
	/* 0x43	EOR d, s */
	/* 0x44	MVP xyc */			op(block_move, MVP);
	/* 0x45	EOR d */			op(direct, EOR);
	/* 0x46	LSR d */			op(direct_rmw, LSR);
	/* 0x47	EOR [d] */
	/* 0x48	PHA s */
	/* 0x49	EOR # */
	/* 0x4a	LSR A */			op(accumulator, LSR);
	/* 0x4b	PHK s */
	/* 0x4c	JMP a */			op(absolute, JMP);
	/* 0x4d	EOR a */			op(absolute, EOR);
	/* 0x4e	LSR a */			op(absolute_rmw, LSR);
	/* 0x4f	EOR al */			op(absolute_long, EOR);

	/* 0x50 BVC r */
	/* 0x51 EOR (d), y */
	/* 0x52 EOR (d) */			op(direct_indirect, EOR);
	/* 0x53 EOR (d, s), y */
	/* 0x54 MVN xyc */			op(block_move, MVN);
	/* 0x55 EOR d, x */
	/* 0x56 LSR d, x */
	/* 0x57 EOR [d],y */
	/* 0x58 CLI i */
	/* 0x59 EOR a, y */			op(absolute_y, EOR);
	/* 0x5a PHY s */
	/* 0x5b TCD i */
	/* 0x5c JMP al */			op(absolute_long_jmp, JML);	// [sic]; this updates PBR so it's JML.
	/* 0x5d EOR a, x */			op(absolute_x, EOR);
	/* 0x5e LSR a, x */			op(absolute_x_rmw, LSR);
	/* 0x5f EOR al, x */		op(absolute_long_x, EOR);

	/* 0x60 RTS s */
	/* 0x61 ADC (d, x) */		op(direct_indexed_indirect, ADC);
	/* 0x62 PER s */
	/* 0x63 ADC d, s */
	/* 0x64 STZ d */			op(direct, STZ);
	/* 0x65 ADC d */			op(direct, ADC);
	/* 0x66 ROR d */			op(direct_rmw, ROR);
	/* 0x67 ADC [d] */
	/* 0x68 PLA s */
	/* 0x69 ADC # */
	/* 0x6a ROR A */			op(accumulator, ROR);
	/* 0x6b RTL s */
	/* 0x6c JMP (a) */			op(absolute_indirect_jmp, JMP);
	/* 0x6d ADC a */			op(absolute, ADC);
	/* 0x6e ROR a */			op(absolute_rmw, ROR);
	/* 0x6f ADC al */			op(absolute_long, ADC);

	/* 0x70 BVS r */
	/* 0x71 ADC (d), y */
	/* 0x72 ADC (d) */			op(direct_indirect, ADC);
	/* 0x73 ADC (d, s), y */
	/* 0x74 STZ d, x */
	/* 0x75 ADC d, x */
	/* 0x76 ROR d, x */
	/* 0x77 ADC [d], y */
	/* 0x78 SEI i */
	/* 0x79 ADC a, y */			op(absolute_y, ADC);
	/* 0x7a PLY s */
	/* 0x7b TDC i */
	/* 0x7c JMP (a, x) */		op(absolute_indexed_indirect_jmp, JMP);
	/* 0x7d ADC a, x */			op(absolute_x, ADC);
	/* 0x7e ROR a, x */			op(absolute_x_rmw, ROR);
	/* 0x7f ADC al, x */		op(absolute_long_x, ADC);

	/* 0x80 BRA r */
	/* 0x81 STA (d, x) */		op(direct_indexed_indirect, STA);
	/* 0x82 BRL rl */
	/* 0x83 STA d, s */
	/* 0x84 STY d */			op(direct, STY);
	/* 0x85 STA d */			op(direct, STA);
	/* 0x86 STX d */			op(direct, STX);
	/* 0x87 STA [d] */
	/* 0x88 DEY i */
	/* 0x89 BIT # */
	/* 0x8a TXA i */
	/* 0x8b PHB s */
	/* 0x8c STY a */			op(absolute, STY);
	/* 0x8d STA a */			op(absolute, STA);
	/* 0x8e STX a */			op(absolute, STX);
	/* 0x8f STA al */			op(absolute_long, STA);

	/* 0x90 BCC r */
	/* 0x91 STA (d), y */
	/* 0x92 STA (d) */			op(direct_indirect, STA);
	/* 0x93 STA (d, x), y */
	/* 0x94 STY d, x */
	/* 0x95 STA d, x */
	/* 0x96 STX d, y */
	/* 0x97 STA [d], y */
	/* 0x98 TYA i */
	/* 0x99 STA a, y */			op(absolute_y, STA);
	/* 0x9a TXS i */
	/* 0x9b TXY i */
	/* 0x9c STZ a */			op(absolute, STZ);
	/* 0x9d STA a, x */			op(absolute_x, STA);
	/* 0x9e STZ a, x */			op(absolute_x, STZ);
	/* 0x9f STA al, x */		op(absolute_long_x, STA);

	/* 0xa0 LDY # */
	/* 0xa1 LDA (d, x) */		op(direct_indexed_indirect, LDA);
	/* 0xa2 LDX # */
	/* 0xa3 LDA d, s */
	/* 0xa4 LDY d */			op(direct, LDY);
	/* 0xa5 LDA d */			op(direct, LDA);
	/* 0xa6 LDX d */			op(direct, LDX);
	/* 0xa7 LDA [d] */
	/* 0xa8 TAY i */
	/* 0xa9 LDA # */
	/* 0xaa TAX i */
	/* 0xab PLB s */
	/* 0xac LDY a */			op(absolute, LDY);
	/* 0xad LDA a */			op(absolute, LDA);
	/* 0xae LDX a */			op(absolute, LDX);
	/* 0xaf LDA al */			op(absolute_long, LDA);

	/* 0xb0 BCS r */
	/* 0xb1 LDA (d), y */
	/* 0xb2 LDA (d) */			op(direct_indirect, LDA);
	/* 0xb3 LDA (d, s), y */
	/* 0xb4 LDY d, x */
	/* 0xb5 LDA d, x */
	/* 0xb6 LDX d, y */
	/* 0xb7 LDA [d], y */
	/* 0xb8 CLV i */
	/* 0xb9 LDA a, y */			op(absolute_y, LDA);
	/* 0xba TSX i */
	/* 0xbb TYX i */
	/* 0xbc LDY a, x */			op(absolute_x, LDY);
	/* 0xbd LDA a, x */			op(absolute_x, LDA);
	/* 0xbe LDX a, y */			op(absolute_y, LDX);
	/* 0xbf LDA al, x */		op(absolute_long_x, LDA);

	/* 0xc0 CPY # */
	/* 0xc1 CMP (d, x) */		op(direct_indexed_indirect, CMP);
	/* 0xc2 REP # */
	/* 0xc3 CMP d, s */
	/* 0xc4 CPY d */			op(direct, CPY);
	/* 0xc5 CMP d */			op(direct, CMP);
	/* 0xc6 DEC d */			op(direct_rmw, DEC);
	/* 0xc7 CMP [d] */
	/* 0xc8 INY i */
	/* 0xc9 CMP # */
	/* 0xca DEX i */
	/* 0xcb WAI i */
	/* 0xcc CPY a */			op(absolute, CPY);
	/* 0xcd CMP a */			op(absolute, CMP);
	/* 0xce DEC a */			op(absolute_rmw, DEC);
	/* 0xcf CMP al */			op(absolute_long, CMP);

	/* 0xd0 BNE r */
	/* 0xd1 CMP (d), y */
	/* 0xd2 CMP (d) */			op(direct_indirect, CMP);
	/* 0xd3 CMP (d, s), y */
	/* 0xd4 PEI s */
	/* 0xd5 CMP d, x */
	/* 0xd6 DEC d, x */
	/* 0xd7 CMP [d], y */
	/* 0xd8 CLD i */
	/* 0xd9 CMP a, y */			op(absolute_y, CMP);
	/* 0xda PHX s */
	/* 0xdb STP i */
	/* 0xdc JML (a) */			op(absolute_indirect_jml, JML);
	/* 0xdd CMP a, x */			op(absolute_x, CMP);
	/* 0xde DEC a, x */			op(absolute_x_rmw, DEC);
	/* 0xdf CMP al, x */		op(absolute_long_x, CMP);

	/* 0xe0 CPX # */
	/* 0xe1 SBC (d, x) */		op(direct_indexed_indirect, SBC);
	/* 0xe2 SEP # */
	/* 0xe3 SBC d, s */
	/* 0xe4 CPX d */			op(direct, CPX);
	/* 0xe5 SBC d */			op(direct, SBC);
	/* 0xe6 INC d */			op(direct_rmw, INC);
	/* 0xe7 SBC [d] */
	/* 0xe8 INX i */
	/* 0xe9 SBC # */
	/* 0xea NOP i */
	/* 0xeb XBA i */
	/* 0xec CPX a */			op(absolute, CPX);
	/* 0xed SBC a */			op(absolute, SBC);
	/* 0xee INC a */			op(absolute_rmw, INC);
	/* 0xef SBC al */			op(absolute_long, SBC);

	/* 0xf0 BEQ r */
	/* 0xf1 SBC (d), y */
	/* 0xf2 SBC (d) */			op(direct_indirect, SBC);
	/* 0xf3 SBC (d, s), y */
	/* 0xf4 PEA s */
	/* 0xf5 SBC d, x */
	/* 0xf6 INC d, x */
	/* 0xf7 SBC [d], y */
	/* 0xf8 SED i */
	/* 0xf9 SBC a, y */			op(absolute_y, SBC);
	/* 0xfa PLX s */
	/* 0xfb XCE i */
	/* 0xfc JSR (a, x) */		op(absolute_indexed_indirect_jsr, JMP);	// [sic]
	/* 0xfd SBC a, x */			op(absolute_x, SBC);
	/* 0xfe INC a, x */			op(absolute_x_rmw, INC);
	/* 0xff SBC al, x */		op(absolute_long_x, SBC);

#undef op

	// TEMPORARY: for my interest. To be removed.
	printf("Generated %zd micro-ops in total; covered %d opcodes\n", micro_ops_.size(), constructor.opcode);
}
