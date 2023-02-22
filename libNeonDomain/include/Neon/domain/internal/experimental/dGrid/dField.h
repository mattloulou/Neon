#pragma once

#include "Neon/core/core.h"
#include "Neon/core/types/Macros.h"

#include "Neon/set/DataConfig.h"
#include "Neon/set/DevSet.h"
#include "Neon/set/HuOptions.h"


#include "Neon/domain/aGrid.h"
#include "Neon/domain/interface/FieldBaseTemplate.h"
#include "Neon/domain/tools/PartitionTable.h"

#include "dPartition.h"

namespace Neon::domain::internal::exp::dGrid {

class dGrid;

/**
 * Create and manage a dense field on both GPU and CPU. dField also manages updating
 * the GPU->CPU and CPU-GPU as well as updating the halo. User can use dField to populate
 * the field with data as well was exporting it to VTI. To create a new dField,
 * use the newField function in dGrid.
 */
template <typename T, int C = 0>
class dField : public Neon::domain::interface::FieldBaseTemplate<T,
                                                                 C,
                                                                 dGrid,
                                                                 dPartition<T, C>,
                                                                 int>
{
    friend dGrid;

   public:
    static constexpr int Cardinality = C;
    using Type = T;
    using Self = dField<Type, Cardinality>;
    using Grid = dGrid;
    using Field = dField;
    using Partition = dPartition<T, C>;
    using Voxel = typename Partition::Voxel;
    using NghIdx = typename Partition::NghIdx;


    dField();
    virtual ~dField() = default;

    auto self() -> Self&;
    auto self() const -> const Self&;

    /**
     * Returns the metadata associated with the element in location idx.
     * If the element is not active (it does not belong to the voxelized domain),
     * then the default outside value is returned.
     */
    auto operator()(const Neon::index_3d& idx,
                    const int&            cardinality) const
        -> Type final;

    auto newHaloUpdateContainer(int                        streamSetIdx,
                                Neon::set::StencilSemantic semantic,
                                Neon::set::TransferMode    transferMode,
                                Neon::Execution            execution)
        const -> Neon::set::Container;

    virtual auto getReference(const Neon::index_3d& idx,
                              const int&            cardinality)
        -> Type& final;

    auto updateDeviceData(int streamSetId)
        -> void;

    auto updateHostData(int streamSetId)
        -> void;

    /**
     * Return a constant reference to a specific partition based on a set of parameters:
     * execution type, target device, dataView
     */
    auto getPartition(Neon::Execution       execution,
                      Neon::SetIdx          setIdx,
                      const Neon::DataView& dataView = Neon::DataView::STANDARD) const
        -> const Partition& final;
    /**
     * Return a reference to a specific partition based on a set of parameters:
     * execution type, target device, dataView
     */
    auto getPartition(Neon::Execution       execution,
                      Neon::SetIdx          setIdx,
                      const Neon::DataView& dataView = Neon::DataView::STANDARD)
        -> Partition& final;

    auto dot(Neon::set::patterns::BlasSet<T>& blasSet,
             const dField<T>&                 input,
             Neon::set::MemDevSet<T>&         output,
             const Neon::DataView&            dataView = Neon::DataView::STANDARD)
        -> T;

    auto dotCUB(Neon::set::patterns::BlasSet<T>& blasSet,
                const dField<T>&                 input,
                Neon::set::MemDevSet<T>&         output,
                const Neon::DataView&            dataView = Neon::DataView::STANDARD)
        -> void;

    auto norm2(Neon::set::patterns::BlasSet<T>& blasSet,
               Neon::set::MemDevSet<T>&         output,
               const Neon::DataView&            dataView = Neon::DataView::STANDARD)
        -> T;

    auto norm2CUB(Neon::set::patterns::BlasSet<T>& blasSet,
                  Neon::set::MemDevSet<T>&         output,
                  const Neon::DataView&            dataView = Neon::DataView::STANDARD)
        -> void;

    static auto swap(Field& A, Field& B)
        -> void;

   private:
    auto haloUpdate(SetIdx                     setIdx,
                    int                        streamSetIdx,
                    Neon::set::StencilSemantic semantic,
                    Neon::set::TransferMode    transferMode,
                    Neon::Execution            execution) const
        -> void;

    /** Convert a global 3d index into a Partition local offset */
    auto helpGlobalIdxToPartitionIdx(Neon::index_3d const& index)
        const -> std::pair<Neon::index_3d, int>;

    auto getLaunchInfo(const Neon::DataView dataView)
        const -> Neon::set::LaunchParameters;

    auto haloUpdate(int                     streamSetIdx,
                    Neon::set::TransferMode transferMode,
                    Neon::Execution         execution)
        -> void;

    dField(const std::string&                        fieldUserName,
           Neon::DataUse                             dataUse,
           const Neon::MemoryOptions&                memoryOptions,
           const Grid&                               grid,
           const Neon::set::DataSet<Neon::index_3d>& dims,
           int                                       zHaloDim,
           Neon::domain::haloStatus_et::e            haloStatus,
           int                                       cardinality,
           const Neon::set::MemSet<Neon::int8_3d>&   stencilIdTo3dOffset);

    struct Data
    {
        struct ReductionInformation
        {
            std::vector<int> startIDByView /* one entry for each cardinality */;
            std::vector<int> nElementsByView /* one entry for each cardinality */;
        };

        Neon::domain::tool::PartitionTable<Partition, ReductionInformation> partitionTable;
        Neon::domain::aGrid::Field<T, C>                                    memoryField;

        Neon::DataUse                     dataUse;
        Neon::MemoryOptions               memoryOptions;
        int                               cardinality;
        Neon::set::DataSet<Neon::size_4d> pitch;

        std::shared_ptr<Grid>          grid;
        int                            zHaloDim;
        Neon::domain::haloStatus_et::e haloStatus;
        bool                           periodic_z;

        Neon::set::MemSet<NghIdx> stencilNghIndex;
        // Neon::set::DataSet<element_t*>                  userPointersSet;

    } mData;

    auto getData() -> Data&;
};


}  // namespace Neon::domain::internal::exp::dGrid
