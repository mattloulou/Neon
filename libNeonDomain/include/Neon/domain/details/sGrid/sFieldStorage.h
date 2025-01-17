#pragma once

#include "Neon/core/core.h"
#include "Neon/core/types/Macros.h"


#include "Neon/set/DevSet.h"
#include "Neon/set/memory/memSet.h"

#include "Neon/domain/details/sGrid/sPartition.h"
#include "Neon/set/MemoryOptions.h"


namespace Neon::domain::details::sGrid {

template <typename OuterGridT>
class sGrid /** Forward declaration for aField */;

template <typename OuterGridT, typename T, int C = 0>
class sFieldStorage
{
   public:
    sFieldStorage();
    sFieldStorage(const Neon::domain::interface::GridBase&);

    using Self = sFieldStorage;
    using Partition = sPartition<OuterGridT, T, C>;
    using Grid = sGrid<OuterGridT>;

    Neon::set::MemSet<T> rawMem;

    auto getPartition(Neon::Execution, Neon::DataView, Neon::SetIdx) -> Partition&;

    auto getPartition(Neon::Execution, Neon::DataView, Neon::SetIdx) const-> const Partition&;

    auto getPartitionSet(Neon::Execution, Neon::DataView) -> Neon::set::DataSet<Partition>&;

   private:
    /**
     * This multi-level array returns a DataSet of Partition
     * given a DataView and an Execution types
     */
    std::array<std::array<Neon::set::DataSet<Partition>,
                          Neon::DataViewUtil::nConfig>,
               Neon::ExecutionUtils::numConfigurations>
        partitions;
};
}  // namespace Neon::domain::array
