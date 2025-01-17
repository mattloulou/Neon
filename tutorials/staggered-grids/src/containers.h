#include "Neon/domain/dGrid.h"
#include "Neon/domain/details/staggeredGrid/StaggeredGrid.h"

namespace tools {

template <typename StaggeredGrid, typename T>
struct Containers
{
    static constexpr int errorCode = -11;
    static constexpr int noErrorCode = 11;

    using Self = Containers<StaggeredGrid, T>;

    using Type = T;
    using NodeField = typename StaggeredGrid::template NodeField<T, 1>;
    using VoxelField = typename StaggeredGrid::template VoxelField<T, 1>;

    static auto resetValue(NodeField  field,
                           const Type alpha)
        -> Neon::set::Container;

    static auto resetValue(VoxelField field,
                           const Type alpha)
        -> Neon::set::Container;

    static auto sumNodesOnVoxels(Self::VoxelField&      fieldVox,
                                 const Self::NodeField& fieldNode)
        -> Neon::set::Container;

    static auto sumVoxelsOnNodesAndDivideBy8(Self::NodeField&        fieldNode,
                                 const Self::VoxelField& fieldVox)
        -> Neon::set::Container;
};


extern template struct Containers<Neon::domain::details::experimental::staggeredGrid::StaggeredGrid<Neon::dGrid>, double>;
extern template struct Containers<Neon::domain::details::experimental::staggeredGrid::StaggeredGrid<Neon::dGrid>, float>;

}  // namespace tools