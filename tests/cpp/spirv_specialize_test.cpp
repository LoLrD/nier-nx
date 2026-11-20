#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include <SPIRV/spirv.hpp>

#include "spirv_specialize.h"

using nier::native::AllowContraction;
using nier::native::ByteRange;
using nier::native::Specialize;
using nier::native::SpecializableSlots;
using nier::native::UniformSlot;

static int failures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

namespace {

enum Id : uint32_t {
  kVoid = 1,
  kFunctionType,
  kUint,
  kV4Uint,
  kC0,
  kC1,
  kC2,
  kC3,
  kArrayUint2,
  kSystemBlock,
  kArrayV4Uint3,
  kFetchBlock,
  kSystemPointer,
  kFetchPointer,
  kUintPointer,
  kOutPointer,
  kSystem,
  kFetch,
  kOut,
  kMain,
  kLabel,
  kFlagsChain,
  kFlags,
  kDynamicChain,
  kDynamic,
  kSwizzleChain,
  kSwizzle,
  kSizeChain,
  kSize,
  kAddressChain,
  kAddress,
  kSecondAddressChain,
  kSecondAddress,
  kSecondSizeChain,
  kSecondSize,
  kFlagsAgainChain,
  kFlagsAgain,
  kIndexedChain,
  kIndexed,
  kSum,
  kBound,
};

class Assembler {
 public:
  Assembler() { words_ = {spv::MagicNumber, 0x00010000, 0, kBound, 0}; }

  void Emit(spv::Op op, std::initializer_list<uint32_t> operands) {
    words_.push_back(uint32_t(operands.size() + 1) << 16 | op);
    words_.insert(words_.end(), operands);
  }

  void Name(uint32_t target, const std::string& name) {
    std::vector<uint32_t> packed((name.size() + 4) / 4, 0);
    for (size_t i = 0; i < name.size(); ++i) {
      packed[i / 4] |= uint32_t(uint8_t(name[i])) << (8 * (i % 4));
    }
    words_.push_back(uint32_t(packed.size() + 2) << 16 | spv::OpName);
    words_.push_back(target);
    words_.insert(words_.end(), packed.begin(), packed.end());
  }

  const std::vector<uint32_t>& words() const { return words_; }

 private:
  std::vector<uint32_t> words_;
};

std::vector<uint32_t> TestModule() {
  Assembler a;
  a.Emit(spv::OpCapability, {spv::CapabilityShader});
  a.Emit(spv::OpMemoryModel, {spv::AddressingModelLogical, spv::MemoryModelGLSL450});
  a.Emit(spv::OpEntryPoint, {spv::ExecutionModelFragment, kMain, 0x6E69616D, 0});
  a.Emit(spv::OpExecutionMode, {kMain, spv::ExecutionModeOriginUpperLeft});
  a.Name(kSystem, "xe_uniform_system_constants");
  a.Name(kFetch, "xe_uniform_fetch_constants");
  a.Emit(spv::OpDecorate, {kArrayUint2, spv::DecorationArrayStride, 4});
  a.Emit(spv::OpMemberDecorate, {kSystemBlock, 0, spv::DecorationOffset, 0});
  a.Emit(spv::OpMemberDecorate, {kSystemBlock, 1, spv::DecorationOffset, 4});
  a.Emit(spv::OpMemberDecorate, {kSystemBlock, 2, spv::DecorationOffset, 16});
  a.Emit(spv::OpDecorate, {kArrayV4Uint3, spv::DecorationArrayStride, 16});
  a.Emit(spv::OpMemberDecorate, {kFetchBlock, 0, spv::DecorationOffset, 0});
  a.Emit(spv::OpDecorate, {kSum, spv::DecorationNoContraction});
  a.Emit(spv::OpTypeVoid, {kVoid});
  a.Emit(spv::OpTypeFunction, {kFunctionType, kVoid});
  a.Emit(spv::OpTypeInt, {kUint, 32, 0});
  a.Emit(spv::OpTypeVector, {kV4Uint, kUint, 4});
  a.Emit(spv::OpConstant, {kUint, kC0, 0});
  a.Emit(spv::OpConstant, {kUint, kC1, 1});
  a.Emit(spv::OpConstant, {kUint, kC2, 2});
  a.Emit(spv::OpConstant, {kUint, kC3, 3});
  a.Emit(spv::OpTypeArray, {kArrayUint2, kUint, kC2});
  a.Emit(spv::OpTypeStruct, {kSystemBlock, kUint, kUint, kArrayUint2});
  a.Emit(spv::OpTypeArray, {kArrayV4Uint3, kV4Uint, kC3});
  a.Emit(spv::OpTypeStruct, {kFetchBlock, kArrayV4Uint3});
  a.Emit(spv::OpTypePointer, {kSystemPointer, spv::StorageClassUniform, kSystemBlock});
  a.Emit(spv::OpTypePointer, {kFetchPointer, spv::StorageClassUniform, kFetchBlock});
  a.Emit(spv::OpTypePointer, {kUintPointer, spv::StorageClassUniform, kUint});
  a.Emit(spv::OpTypePointer, {kOutPointer, spv::StorageClassOutput, kUint});
  a.Emit(spv::OpVariable, {kSystemPointer, kSystem, spv::StorageClassUniform});
  a.Emit(spv::OpVariable, {kFetchPointer, kFetch, spv::StorageClassUniform});
  a.Emit(spv::OpVariable, {kOutPointer, kOut, spv::StorageClassOutput});
  a.Emit(spv::OpFunction, {kVoid, kMain, spv::FunctionControlMaskNone, kFunctionType});
  a.Emit(spv::OpLabel, {kLabel});
  a.Emit(spv::OpAccessChain, {kUintPointer, kFlagsChain, kSystem, kC0});
  a.Emit(spv::OpLoad, {kUint, kFlags, kFlagsChain});
  a.Emit(spv::OpAccessChain, {kUintPointer, kDynamicChain, kSystem, kC1});
  a.Emit(spv::OpLoad, {kUint, kDynamic, kDynamicChain});
  a.Emit(spv::OpAccessChain, {kUintPointer, kSwizzleChain, kSystem, kC2, kC1});
  a.Emit(spv::OpLoad, {kUint, kSwizzle, kSwizzleChain});
  a.Emit(spv::OpAccessChain, {kUintPointer, kSizeChain, kFetch, kC0, kC0, kC2});
  a.Emit(spv::OpLoad, {kUint, kSize, kSizeChain});
  a.Emit(spv::OpAccessChain, {kUintPointer, kAddressChain, kFetch, kC0, kC0, kC1});
  a.Emit(spv::OpLoad, {kUint, kAddress, kAddressChain});
  a.Emit(spv::OpAccessChain, {kUintPointer, kSecondAddressChain, kFetch, kC0, kC1, kC3});
  a.Emit(spv::OpLoad, {kUint, kSecondAddress, kSecondAddressChain});
  a.Emit(spv::OpAccessChain, {kUintPointer, kSecondSizeChain, kFetch, kC0, kC2, kC0});
  a.Emit(spv::OpLoad, {kUint, kSecondSize, kSecondSizeChain});
  a.Emit(spv::OpAccessChain, {kUintPointer, kFlagsAgainChain, kSystem, kC0});
  a.Emit(spv::OpLoad, {kUint, kFlagsAgain, kFlagsAgainChain});
  a.Emit(spv::OpAccessChain, {kUintPointer, kIndexedChain, kFetch, kC0, kDynamic, kC2});
  a.Emit(spv::OpLoad, {kUint, kIndexed, kIndexedChain});
  a.Emit(spv::OpIAdd, {kUint, kSum, kFlags, kSwizzle});
  a.Emit(spv::OpStore, {kOut, kSum});
  a.Emit(spv::OpReturn, {});
  a.Emit(spv::OpFunctionEnd, {});
  return a.words();
}

struct Instruction {
  uint32_t op;
  std::vector<uint32_t> operands;
};

std::vector<Instruction> Decode(const std::vector<uint32_t>& words) {
  std::vector<Instruction> result;
  for (size_t at = 5; at < words.size(); at += words[at] >> 16) {
    result.push_back({words[at] & 0xFFFF, {words.begin() + at + 1, words.begin() + at + (words[at] >> 16)}});
  }
  return result;
}

uint32_t ConstantValue(const std::vector<Instruction>& code, uint32_t id) {
  for (const Instruction& i : code) {
    if (i.op == spv::OpConstant && i.operands[1] == id) {
      return i.operands[2];
    }
  }
  return UINT32_MAX;
}

bool CopiesConstant(const std::vector<Instruction>& code, uint32_t result, uint32_t value) {
  for (const Instruction& i : code) {
    if (i.op == spv::OpCopyObject && i.operands[1] == result) {
      return i.operands[0] == kUint && ConstantValue(code, i.operands[2]) == value;
    }
  }
  return false;
}

uint32_t Decorations(const std::vector<Instruction>& code, uint32_t decoration) {
  uint32_t count = 0;
  for (const Instruction& i : code) {
    count += i.op == spv::OpDecorate && i.operands[1] == decoration;
  }
  return count;
}

bool Loads(const std::vector<Instruction>& code, uint32_t result) {
  for (const Instruction& i : code) {
    if (i.op == spv::OpLoad && i.operands[1] == result) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  const std::vector<uint32_t> module = TestModule();
  const ByteRange ranges[] = {{0, 4}, {16, 24}};
  const std::vector<UniformSlot> slots = SpecializableSlots(module, ranges);
  const std::vector<UniformSlot> expected = {{false, 0}, {false, 20}, {true, 8}, {true, 32}};
  Check(slots == expected, "flags_swizzles_and_fetch_words_without_addresses_are_specializable");

  const uint32_t values[] = {7, 9, 11, 13};
  const std::vector<uint32_t> specialized = Specialize(module, ranges, slots, values);
  const std::vector<Instruction> code = Decode(specialized);
  Check(CopiesConstant(code, kFlags, 7) && CopiesConstant(code, kFlagsAgain, 7), "every_load_of_a_slot_gets_its_value");
  Check(CopiesConstant(code, kSwizzle, 9), "array_element_offset_uses_the_stride");
  Check(CopiesConstant(code, kSize, 11) && CopiesConstant(code, kSecondSize, 13), "fetch_words_get_their_values");
  Check(Loads(code, kDynamic) && Loads(code, kAddress) && Loads(code, kSecondAddress) && Loads(code, kIndexed),
        "dynamic_members_addresses_and_dynamic_indices_stay_loads");
  Check(specialized[3] == kBound + 4, "new_constants_raise_the_id_bound");
  size_t first_function = 0;
  size_t last_new_constant = 0;
  for (size_t i = 0; i < code.size(); ++i) {
    if (code[i].op == spv::OpFunction && !first_function) {
      first_function = i;
    }
    if (code[i].op == spv::OpConstant && code[i].operands[1] >= kBound) {
      last_new_constant = i;
    }
  }
  Check(last_new_constant && last_new_constant < first_function, "new_constants_precede_the_functions");
  Check(SpecializableSlots(specialized, ranges).empty(), "specialized_module_has_nothing_left_to_specialize");

  const std::vector<uint32_t> contracted = AllowContraction(specialized);
  const std::vector<Instruction> contracted_code = Decode(contracted);
  Check(Decorations(code, spv::DecorationNoContraction) == 1 &&
            !Decorations(contracted_code, spv::DecorationNoContraction),
        "no_contraction_decorations_are_dropped");
  Check(Decorations(contracted_code, spv::DecorationArrayStride) == 2 && contracted.size() + 3 == specialized.size() &&
            contracted[3] == specialized[3] && CopiesConstant(contracted_code, kFlags, 7),
        "everything_else_is_kept");
  return failures ? 1 : 0;
}
