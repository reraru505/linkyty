#include "loader/redZonePatcher.h"

namespace Loader {

RedZonePatchResult PatchGuestInstructions(uint64_t /*segment_addr*/, uint64_t /*segment_size*/,
                                          std::span<const uintptr_t> /*code_pages*/,
                                          bool /*apply_live_patching*/,
                                          bool /*protect_memory_faults*/) {
	return {};
}

void RegisterRedZonePatchModule(void* /*module_ptr*/, uint64_t /*module_size*/,
                                void* /*trampoline_area_ptr*/, uint64_t /*trampoline_area_size*/) {}

void UnregisterRedZonePatchModule(void* /*module_ptr*/) {}

} // namespace Loader
