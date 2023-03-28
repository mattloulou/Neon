#pragma once
#include <assert.h>

#include "Neon/core/core.h"
#include "Neon/core/types/DataUse.h"
#include "Neon/core/types/Macros.h"

#include "Neon/set/BlockConfig.h"
#include "Neon/set/Containter.h"
#include "Neon/set/DevSet.h"
#include "Neon/set/MemoryOptions.h"

#include "Neon/sys/memory/MemDevice.h"

#include "Neon/domain/aGrid.h"

#include "Neon/domain/interface/GridBaseTemplate.h"
#include "Neon/domain/interface/GridConcept.h"
#include "Neon/domain/interface/KernelConfig.h"
#include "Neon/domain/interface/LaunchConfig.h"
#include "Neon/domain/interface/Stencil.h"
#include "Neon/domain/interface/common.h"

#include "Neon/domain/tools/GridTransformer.h"
#include "Neon/domain/tools/SpanTable.h"

#include "Neon/domain/details/eGrid/eGrid.h"
#include "Neon/domain/patterns/PatternScalar.h"

#include "BlockViewPartition.h"

namespace Neon::domain::details::bGrid {

namespace details {
struct GridTransformation
{
    template <typename T, int C>
    using Partition = BlockViewPartition<T, C>;
    using Span = Neon::domain::details::eGrid::eSpan;
    using FoundationGrid = Neon::domain::details::eGrid::eGrid;

    static auto initSpan(FoundationGrid& foundationGrid, Neon::domain::tool::SpanTable<Span>& spanTable) -> void
    {
        spanTable.forEachConfiguration([&](Neon::SetIdx   setIdx,
                                           Neon::DataView dw,
                                           Span&          span) {
            span = foundationGrid.getSpan(setIdx, dw);
        });
    }

    static auto initLaunchParameters(FoundationGrid&       foundationGrid,
                                     Neon::DataView        dataView,
                                     const Neon::index_3d& blockSize,
                                     const size_t&         shareMem) -> Neon::set::LaunchParameters
    {
        return foundationGrid.getLaunchParameters(dataView, blockSize, shareMem);
    }

    template <typename T, int C>
    static auto initFieldPartition(FoundationGrid::Field<T, C>&                         foundationField,
                                   Neon::domain::tool::PartitionTable<Partition<T, C>>& partitionTable) -> void
    {
        partitionTable.forEachConfiguration(
            [&](Neon::Execution  execution,
                Neon::SetIdx     setIdx,
                Neon::DataView   dw,
                Partition<T, C>& partition) {
                auto& foundationPartition = foundationField.getPartition(execution, setIdx, dw);
                partition = Partition<T, C>(foundationPartition);
            });
    }
};
}  // namespace details
using BlockViewGrid = Neon::domain::tool::GridTransformer<details::GridTransformation>::Grid;

}  // namespace Neon::domain::details::bGrid