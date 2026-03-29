// This TU exists solely to force coverage generation for header-only templates.
// It does NOT affect production builds.

#include <pubsub_itc_fw/CoverageDummy.hpp>
#include <pubsub_itc_fw/ExpandablePoolAllocator.hpp>
#include <pubsub_itc_fw/FixedSizeMemoryPool.hpp>
#include <pubsub_itc_fw/PoolStatistics.hpp>
#include <pubsub_itc_fw/UseHugePagesFlag.hpp>

namespace pubsub_itc_fw {

// Explicit instantiation for coverage. This forces GCC/Clang to emit code
// in this TU, producing .gcno entries that reference the header files.

template class ExpandablePoolAllocator<CoverageDummy>;
template class FixedSizeMemoryPool<CoverageDummy>;

} // namespace pubsub_itc_fw

