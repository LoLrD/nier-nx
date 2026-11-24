#include "spirv_specialize.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <SPIRV/spirv.hpp>

namespace nier::native {

namespace {

constexpr size_t kHeaderWords = 5;
constexpr std::string_view kSystemBlock = "xe_uniform_system_constants";
constexpr std::string_view kFetchBlock = "xe_uniform_fetch_constants";
constexpr uint32_t kFetchConstantWords = 6;

struct Pointer {
  uint32_t variable;
  uint32_t type;
  uint32_t offset;
};

struct Load {
  size_t position;
  uint32_t type;
  uint32_t result;
  UniformSlot slot;
};

class Reader {
 public:
  Reader(std::span<const uint32_t> module, std::span<const ByteRange> system_ranges)
      : module_(module), system_ranges_(system_ranges) {}

  template <typename Visit>
  void ForEachLoad(Visit visit) {
    for (size_t at = kHeaderWords; at < module_.size(); at += module_[at] >> 16) {
      const uint32_t* w = &module_[at];
      const uint32_t count = w[0] >> 16;
      switch (spv::Op(w[0] & 0xFFFF)) {
        case spv::OpName:
          names_[w[1]] = std::string_view(reinterpret_cast<const char*>(w + 2));
          break;
        case spv::OpDecorate:
          if (w[2] == spv::DecorationArrayStride) {
            strides_[w[1]] = w[3];
          }
          break;
        case spv::OpMemberDecorate:
          if (w[3] == spv::DecorationOffset) {
            member_offsets_[{w[1], w[2]}] = w[4];
          }
          break;
        case spv::OpTypeInt:
          if (w[2] == 32) {
            int_types_.insert(w[1]);
          }
          break;
        case spv::OpTypeVector:
        case spv::OpTypeArray:
          elements_[w[1]] = w[2];
          break;
        case spv::OpTypeStruct:
          members_[w[1]].assign(w + 2, w + count);
          break;
        case spv::OpTypePointer:
          pointees_[w[1]] = w[3];
          break;
        case spv::OpConstant:
          if (int_types_.contains(w[1])) {
            constants_[w[2]] = w[3];
          }
          break;
        case spv::OpVariable:
          if (const auto pointee = pointees_.find(w[1]); pointee != pointees_.end()) {
            pointers_[w[2]] = {w[2], pointee->second, 0};
          }
          break;
        case spv::OpAccessChain:
        case spv::OpInBoundsAccessChain:
          if (const auto pointer = Chain(w[3], std::span(w + 4, w + count))) {
            pointers_[w[2]] = *pointer;
          }
          break;
        case spv::OpLoad:
          if (const auto slot = SlotFor(w[1], w[3])) {
            visit(Load{at, w[1], w[2], *slot});
          }
          break;
        default:
          break;
      }
    }
  }

 private:
  std::optional<Pointer> Chain(uint32_t base, std::span<const uint32_t> indices) const {
    const auto it = pointers_.find(base);
    if (it == pointers_.end()) {
      return std::nullopt;
    }
    Pointer pointer = it->second;
    for (const uint32_t index_id : indices) {
      const auto index = constants_.find(index_id);
      if (index == constants_.end()) {
        return std::nullopt;
      }
      if (const auto members = members_.find(pointer.type); members != members_.end()) {
        const auto offset = member_offsets_.find({pointer.type, index->second});
        if (offset == member_offsets_.end() || index->second >= members->second.size()) {
          return std::nullopt;
        }
        pointer.offset += offset->second;
        pointer.type = members->second[index->second];
        continue;
      }
      const auto element = elements_.find(pointer.type);
      if (element == elements_.end()) {
        return std::nullopt;
      }
      const auto stride = strides_.find(pointer.type);
      pointer.offset += index->second * (stride != strides_.end() ? stride->second : 4);
      pointer.type = element->second;
    }
    return pointer;
  }

  std::optional<UniformSlot> SlotFor(uint32_t type, uint32_t pointer_id) const {
    const auto pointer = pointers_.find(pointer_id);
    if (pointer == pointers_.end() || pointer->second.type != type || !int_types_.contains(type)) {
      return std::nullopt;
    }
    const auto name = names_.find(pointer->second.variable);
    if (name == names_.end()) {
      return std::nullopt;
    }
    const uint32_t offset = pointer->second.offset;
    if (name->second == kSystemBlock) {
      const bool allowed = std::ranges::any_of(
          system_ranges_, [&](const ByteRange& range) { return offset >= range.begin && offset < range.end; });
      return allowed ? std::optional(UniformSlot{false, offset}) : std::nullopt;
    }
    if (name->second == kFetchBlock) {
      const uint32_t word = offset / 4 % kFetchConstantWords;
      return word == 1 || word == 5 ? std::nullopt : std::optional(UniformSlot{true, offset});
    }
    return std::nullopt;
  }

  std::span<const uint32_t> module_;
  std::span<const ByteRange> system_ranges_;
  std::unordered_map<uint32_t, std::string_view> names_;
  std::unordered_map<uint32_t, uint32_t> strides_;
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> member_offsets_;
  std::unordered_set<uint32_t> int_types_;
  std::unordered_map<uint32_t, uint32_t> elements_;
  std::unordered_map<uint32_t, std::vector<uint32_t>> members_;
  std::unordered_map<uint32_t, uint32_t> pointees_;
  std::unordered_map<uint32_t, uint32_t> constants_;
  std::unordered_map<uint32_t, Pointer> pointers_;
};

}  // namespace

std::vector<UniformSlot> SpecializableSlots(std::span<const uint32_t> module, std::span<const ByteRange> system_ranges) {
  std::vector<UniformSlot> slots;
  Reader(module, system_ranges).ForEachLoad([&](const Load& load) { slots.push_back(load.slot); });
  std::ranges::sort(slots);
  slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
  return slots;
}

std::vector<uint32_t> Specialize(std::span<const uint32_t> module, std::span<const ByteRange> system_ranges,
                                 std::span<const UniformSlot> slots, std::span<const uint32_t> values) {
  uint32_t bound = module[3];
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> constants;
  std::unordered_map<size_t, std::array<uint32_t, 3>> copies;
  Reader(module, system_ranges).ForEachLoad([&](const Load& load) {
    const auto slot = std::ranges::lower_bound(slots, load.slot);
    if (slot == slots.end() || *slot != load.slot) {
      return;
    }
    const uint32_t value = values[size_t(slot - slots.begin())];
    const auto [constant, inserted] = constants.try_emplace({load.type, value}, bound);
    bound += inserted;
    copies[load.position] = {load.type, load.result, constant->second};
  });
  std::vector<uint32_t> result(module.begin(), module.begin() + kHeaderWords);
  result[3] = bound;
  bool declared = false;
  for (size_t at = kHeaderWords; at < module.size(); at += module[at] >> 16) {
    if (!declared && spv::Op(module[at] & 0xFFFF) == spv::OpFunction) {
      for (const auto& [key, id] : constants) {
        result.insert(result.end(), {4u << 16 | spv::OpConstant, key.first, id, key.second});
      }
      declared = true;
    }
    if (const auto copy = copies.find(at); copy != copies.end()) {
      result.insert(result.end(), {4u << 16 | spv::OpCopyObject, copy->second[0], copy->second[1], copy->second[2]});
      continue;
    }
    result.insert(result.end(), module.begin() + at, module.begin() + at + (module[at] >> 16));
  }
  return result;
}

std::vector<uint32_t> AllowContraction(std::span<const uint32_t> module) {
  std::vector<uint32_t> result(module.begin(), module.begin() + kHeaderWords);
  result.reserve(module.size());
  for (size_t at = kHeaderWords; at < module.size(); at += module[at] >> 16) {
    const uint32_t count = module[at] >> 16;
    if (spv::Op(module[at] & 0xFFFF) == spv::OpDecorate && module[at + 2] == spv::DecorationNoContraction) {
      continue;
    }
    result.insert(result.end(), module.begin() + at, module.begin() + at + count);
  }
  return result;
}

std::vector<UniformSlot> TextureFetchSlots(std::vector<UniformSlot> slots,
                                           std::span<const uint32_t> texture_fetch_constants) {
  std::erase_if(slots, [&](const UniformSlot& slot) {
    return slot.fetch && std::find(texture_fetch_constants.begin(), texture_fetch_constants.end(),
                                   slot.offset / kFetchConstantBytes) == texture_fetch_constants.end();
  });
  return slots;
}

}  // namespace nier::native
