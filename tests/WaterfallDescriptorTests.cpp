// Focused reproduction for the waterfall descriptor used by the Demon's Souls
// character-creation pixel shader (guest hash 0x06240562ae88d4f0):
//
//   mask  = Phi([ballot, entry], [mask ^ (1 << (find_lsb(mask) & 31)), latch])
//   lsb   = find_lsb(mask)
//   key   = per-lane record index
//   desc  = load 8 dwords at (heap + (lsb << 5))      <- no IAdd32 wrap
//   image = GetImageResource(desc[0..7])
//
// The lane index is folded straight into the address, so the table offset never
// materializes as an IAdd32. Resource tracking must still fold the waterfall
// descriptor onto the uniform key.
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"
#include "graphics/shader/recompiler/ir/passes/WaterfallDescriptor.h"

#include <array>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using namespace Libs::Graphics::ShaderRecompiler::IR;
using Libs::Graphics::ShaderType;
namespace Decoder = Libs::Graphics::ShaderRecompiler::Decoder;

struct Fixture {
	Program program;
	Block*  block = nullptr;

	explicit Fixture(ShaderType stage = ShaderType::Pixel) {
		program.stage           = stage;
		program.shader_hash     = 0x06240562ae88d4f0ull;
		program.user_data_count = 64;
		block                   = AddBlock();
	}

	Block* AddBlock() {
		auto  storage = std::make_unique<Block>();
		auto* result  = storage.get();
		program.block_storage.push_back(std::move(storage));
		program.blocks.push_back(result);
		program.block_info.push_back({.id = static_cast<uint32_t>(program.block_info.size())});
		return result;
	}

	Value Emit(ValueOpcode opcode, std::initializer_list<Value> args = {}, uint64_t flags = 0,
	           Block* destination = nullptr) {
		auto& inst = (destination != nullptr ? destination : block)->AppendNewInst(opcode, args, flags);
		return Value(&inst);
	}

	template <typename T>
	Value Emit(ValueOpcode opcode, std::initializer_list<Value> args, T flags,
	           Block* destination = nullptr) {
		uint64_t bits = 0;
		std::memcpy(&bits, &flags, sizeof(flags));
		return Emit(opcode, args, bits, destination);
	}

	MemoryFlags AddMemory(MemoryInfo memory, uint32_t pc) {
		const auto index = static_cast<uint32_t>(program.memory_info.size());
		program.memory_info.push_back(memory);
		return {index, pc};
	}
};

void Check(bool condition, const char* message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

} // namespace

// Keep the typed-IR implementation self-contained, mirroring the pass unit tests.
#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"

namespace {

// Models the shader's packed per-lane attribute: the record index lives in bits
// 24..31, is per-lane (it reaches LaneId), and is also truncated to five bits for
// the ballot bit.
struct LaneIndex {
	Value packed;
	Value index;  // BitFieldUExtract(packed, 24, 8)
	Value key;    // packed >> 24
};

LaneIndex MakeLaneIndex(Fixture& fixture) {
	const auto lane = fixture.Emit(ValueOpcode::LaneId);
	const auto tag =
	    fixture.Emit(ValueOpcode::GetUserData, {Value(static_cast<Libs::Graphics::ShaderRecompiler::IR::ScalarReg>(0))});
	const auto packed = fixture.Emit(
	    ValueOpcode::BitwiseOr32,
	    {fixture.Emit(ValueOpcode::ShiftLeftLogical32, {lane, Value(24u)}),
	     fixture.Emit(ValueOpcode::BitwiseAnd32, {tag, Value(0x00ffffffu)})});
	const auto index =
	    fixture.Emit(ValueOpcode::BitFieldUExtract, {packed, Value(24u), Value(8u)});
	const auto key = fixture.Emit(ValueOpcode::ShiftRightLogical32, {packed, Value(24u)});
	return {packed, index, key};
}

// entry = BitwiseOr32(ReadLane(SelectU32(pred, 1 << (index & 31), 0), 31), ReadLane(..., 63))
Value MakeBallotEntry(Fixture& fixture, const LaneIndex& lanes) {
	const auto predicate =
	    fixture.Emit(ValueOpcode::INotEqual32, {fixture.Emit(ValueOpcode::BitwiseAnd32,
	                                                          {lanes.packed, Value(0x1fu)}),
	                                            Value(0u)});
	const auto masked  = fixture.Emit(ValueOpcode::BitwiseAnd32, {lanes.index, Value(0x1fu)});
	const auto bit     = fixture.Emit(ValueOpcode::ShiftLeftLogical32, {Value(1u), masked});
	const auto select  = fixture.Emit(ValueOpcode::SelectU32, {predicate, bit, Value(0u)});
	const auto low     = fixture.Emit(ValueOpcode::ReadLane, {select, Value(31u)});
	const auto high    = fixture.Emit(ValueOpcode::ReadLane, {select, Value(63u)});
	return fixture.Emit(ValueOpcode::BitwiseOr32, {low, high});
}

void TestWaterfallDescriptorWithoutTableAdd() {
	Fixture fixture;
	auto*   entry_block = fixture.block;
	auto*   header      = fixture.AddBlock();
	auto*   body        = fixture.AddBlock();

	const auto lanes = MakeLaneIndex(fixture);
	const auto entry = MakeBallotEntry(fixture, lanes);
	entry_block->AddBranch(header);

	// header: the waterfall mask phi and the loop's lowest-set-lane probe.
	auto& mask = header->AppendNewInst(ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
	const auto lsb = fixture.Emit(ValueOpcode::FindILsb32, {Value(&mask)}, 0, header);
	fixture.Emit(ValueOpcode::IEqual32, {lsb, lanes.key}, 0, header);

	// body: descriptor table read indexed by the scaled lane index, with the
	// table base folded into the scalar load's immediate offset.
	const auto scaled = fixture.Emit(ValueOpcode::ShiftLeftLogical32, {lsb, Value(5u)}, 0, body);
	const auto heap =
	    fixture.Emit(ValueOpcode::GetAddressResource,
	                 {fixture.Emit(ValueOpcode::GetUserData, {Value(static_cast<Libs::Graphics::ShaderRecompiler::IR::ScalarReg>(1))}, 0, body),
	                  fixture.Emit(ValueOpcode::GetUserData, {Value(static_cast<Libs::Graphics::ShaderRecompiler::IR::ScalarReg>(2))}, 0, body)},
	                 0, body);
	std::array<Value, 8> dwords {};
	for (uint32_t dword = 0; dword < dwords.size(); dword++) {
		MemoryInfo memory;
		memory.kind = ResourceKind::ScalarAddress;
		// The record table starts 344 bytes into the heap, exactly as the guest shader's
		// scalar-load metadata describes it.
		memory.offset = 344u + dword * sizeof(uint32_t);
		dwords[dword] = fixture.Emit(
		    ValueOpcode::LoadAddressU32, {heap, scaled, Value(0u), Value(true)},
		    fixture.AddMemory(memory, 0xfc), body);
	}
	const auto image = fixture.Emit(ValueOpcode::GetImageResource,
	                                {dwords[0], dwords[1], dwords[2], dwords[3], dwords[4],
	                                 dwords[5], dwords[6], dwords[7]},
	                                0, body);
	const auto sampler = fixture.Emit(ValueOpcode::GetSamplerResource,
	                                  {Value(0u), Value(0u), Value(0u), Value(0u)}, 0, body);
	const auto address = fixture.Emit(ValueOpcode::MakeImageAddress,
	                                  {Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
	                                   Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
	                                   Value(0u), Value(0u), Value(0u)},
	                                  0, body);
	MemoryInfo sample;
	sample.kind            = ResourceKind::Image;
	sample.image_dimension = Decoder::ImageDimension::Dim2D;
	fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, address},
	             fixture.AddMemory(sample, 0xfc), body);

	const auto cleared_bits = fixture.Emit(ValueOpcode::BitwiseAnd32, {lsb, Value(0x1fu)}, 0, body);
	const auto cleared_bit =
	    fixture.Emit(ValueOpcode::ShiftLeftLogical32, {Value(1u), cleared_bits}, 0, body);
	const auto cleared = fixture.Emit(ValueOpcode::BitwiseXor32, {Value(&mask), cleared_bit}, 0, body);
	body->AddBranch(header);
	mask.AddPhiOperand(entry_block, entry);
	mask.AddPhiOperand(body, cleared);

	const auto rewritten = RewriteWaterfallDescriptors(fixture.program);
	Check(rewritten == 1u, "waterfall descriptor with a folded table base was not de-scalarized");

	BuildSrtPlan(fixture.program);
	TrackResources(fixture.program);

	// The record index is per-lane, so the descriptor cannot be resolved statically: the
	// tracker has to bind the record table and let the shader read it at runtime.
	Check(!fixture.program.info.images.empty(), "sampled image was not tracked");
	const auto source = fixture.program.info.images[0].source;
	Check(source < fixture.program.descriptor_sources.size(),
	      "sampled image descriptor source is out of range");
	Check(fixture.program.descriptor_sources[source].indirect_image.has_value(),
	      "per-lane descriptor table was not resolved as a runtime indirect image");

	const auto& indirect = *fixture.program.descriptor_sources[source].indirect_image;
	// The 32-byte record stride is implicit in the `index << 5` addressing, and the record
	// bound comes from the `key < 32` guard the de-scalarization pass installs.
	Check(indirect.table_offset == 344u, "record table base was not taken from the load metadata");
	Check(indirect.key_bound == 32u, "indirect table lost its 32-record bound");
	Check(indirect.item_bound == 0u, "indirect table was bounded per item instead of per key");
}

} // namespace

int main() {
	try {
		TestWaterfallDescriptorWithoutTableAdd();
	} catch (const std::exception& exception) {
		std::cerr << "waterfall descriptor test failed: " << exception.what() << '\n';
		return 1;
	}
	std::cout << "waterfall descriptor test passed\n";
	return 0;
}

// Standalone assertion hooks for the typed IR.
namespace Common {
int DbgExitHandler(const char*, int, std::string_view text) {
	throw std::runtime_error(std::string(text));
}
int DbgExitHandler(const char*, int, fmt::text_style, std::string_view text) {
	throw std::runtime_error(std::string(text));
}
int DbgExitIfHandler(const char* expression, const char* file, int line) {
	throw std::runtime_error(std::string("typed IR assertion: ") + expression + " at " + file + ':' +
	                         std::to_string(line));
}
int DbgNotImplementedHandler(const char* expression, const char* file, int line) {
	throw std::runtime_error(std::string("typed IR not implemented: ") + expression + " at " + file +
	                         ':' + std::to_string(line));
}
void DbgExit(int) { throw std::runtime_error("typed IR assertion failed"); }
} // namespace Common
