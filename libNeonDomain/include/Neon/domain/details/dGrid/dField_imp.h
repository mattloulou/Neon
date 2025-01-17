#pragma once
#include "dField.h"

namespace Neon::domain::details::dGrid {

template <typename T, int C>
dField<T, C>::dField()
{
    mData = std::make_shared<Data>();
}

template <typename T, int C>
dField<T, C>::dField(const std::string&                        fieldUserName,
                     Neon::DataUse                             dataUse,
                     const Neon::MemoryOptions&                memoryOptions,
                     const Grid&                               grid,
                     const Neon::set::DataSet<Neon::index_3d>& dims,
                     int                                       zHaloRadius,
                     Neon::domain::haloStatus_et::e            haloStatus,
                     int                                       cardinality,
                     Neon::set::MemSet<Neon::int8_3d>&         stencilIdTo3dOffset)
    : Neon::domain::interface::FieldBaseTemplate<T, C, Grid, Partition, int>(&grid,
                                                                             fieldUserName,
                                                                             "dField",
                                                                             cardinality,
                                                                             T(0),
                                                                             dataUse,
                                                                             memoryOptions,
                                                                             haloStatus) {

    // only works if dims in x and y direction for all partitions match
    for (int i = 0; i < dims.size() - 1; ++i) {
        for (int j = i + 1; j < dims.size(); ++j) {
            if (dims[i].x != dims[j].x || dims[i].y != dims[j].y) {
                NeonException exc("dField_t");
                exc << "New dField only works on partitioning along z axis.";
                NEON_THROW(exc);
            }
        }
    }

    mData = std::make_shared<Data>(grid.getBackend());
    mData->dataUse = dataUse;
    mData->memoryOptions = memoryOptions;
    mData->cardinality = cardinality;
    mData->memoryOptions = memoryOptions;
    mData->grid = std::make_shared<Grid>(grid);
    mData->haloStatus = (mData->grid->getDevSet().setCardinality() == 1)
                            ? haloStatus_et::e::OFF
                            : haloStatus;
    const int haloRadius = mData->haloStatus == Neon::domain::haloStatus_et::ON ? zHaloRadius : 0;
    mData->zHaloDim = zHaloRadius;

    Neon::set::DataSet<index_3d> origins = this->getGrid().getBackend().template newDataSet<index_3d>({0, 0, 0});
    {  // Computing origins
        origins.forEachSeq(
            [&](Neon::SetIdx setIdx, Neon::index_3d& val) {
                if (setIdx == 0) {
                    val.z = 0;
                    return;
                }
                const Neon::SetIdx proceedingIdx = setIdx - 1;
                val.z = origins[proceedingIdx].z + dims[proceedingIdx].z;
            });
    }

    {  // Computing Pitch
        mData->pitch.forEachSeq(
            [&](Neon::SetIdx setIdx, Neon::size_4d& pitch) {
                switch (mData->memoryOptions.getOrder()) {
                    case MemoryLayout::structOfArrays: {
                        pitch.x = 1;
                        pitch.y = pitch.x * dims[setIdx.idx()].x;
                        pitch.z = pitch.y * dims[setIdx.idx()].y;
                        pitch.w = pitch.z * (dims[setIdx.idx()].z + 2 * haloRadius);
                        break;
                    }
                    case MemoryLayout::arrayOfStructs: {
                        pitch.x = mData->cardinality;
                        pitch.y = pitch.x * dims[setIdx.idx()].x;
                        pitch.z = pitch.y * dims[setIdx.idx()].y;
                        pitch.w = 1;
                        break;
                    }
                }
            });
    }

    {  // Setting up partitions
        Neon::aGrid const& aGrid = mData->grid->helpFieldMemoryAllocator();
        mData->memoryField = aGrid.newField<T,C>(fieldUserName + "-storage", cardinality, T(), dataUse, memoryOptions);
        // const int setCardinality = mData->grid->getBackend().getDeviceCount();
        mData->partitionTable.forEachConfiguration(
            [&](Neon::Execution           execution,
                Neon::SetIdx              setIdx,
                Neon::DataView            dw,
                typename Self::Partition& partition) {
                auto memoryFieldPartition = mData->memoryField.getPartition(execution, setIdx, Neon::DataView::STANDARD);

                partition = dPartition<T, C>(dw,
                                             memoryFieldPartition.mem(),
                                             dims[setIdx],
                                             haloRadius,
                                             mData->zHaloDim,
                                             mData->pitch[setIdx],
                                             setIdx.idx(),
                                             origins[setIdx],
                                             mData->cardinality,
                                             mData->grid->getDimension(),
                                             stencilIdTo3dOffset.rawMem(execution, setIdx));
            });
    }

    {  // Setting Reduction information
        mData->partitionTable.forEachConfigurationWithUserData(
            [&](Neon::Execution,
                Neon::SetIdx   setIdx,
                Neon::DataView dw,
                typename Self::Partition&,
                typename Data::ReductionInformation& reductionInfo) {
                switch (dw) {
                    case Neon::DataView::STANDARD: {
                        // old structure [dv_id][c][i]
                        if (grid.getBackend().devSet().setCardinality() == 1) {
                            // As the number of devices is 1, we don't have halos.
                            reductionInfo.startIDByView.push_back(0);
                            reductionInfo.nElementsByView.push_back(int(dims[setIdx.idx()].rMul()));
                        } else {
                            switch (mData->memoryOptions.getOrder()) {
                                case MemoryLayout::structOfArrays: {
                                    for (int c = 0; c < mData->cardinality; ++c) {
                                        // To compute the start point we need to
                                        // jump the previous cardinalities -> c * dims[setIdx].x * dims[setIdx].y * (dims[setIdx].z + 2 * haloRadius)
                                        // jump one halo -> dims[setIdx].x * dims[setIdx].y * haloRadius
                                        int const startPoint = c * dims[setIdx].x * dims[setIdx].y * (dims[setIdx].z + 2 * haloRadius) +
                                                               dims[setIdx].x * dims[setIdx].y * haloRadius;
                                        int const nElements = dims[setIdx].rMul();

                                        reductionInfo.startIDByView.push_back(startPoint);
                                        reductionInfo.nElementsByView.push_back(nElements);
                                    }
                                    break;
                                }
                                case MemoryLayout::arrayOfStructs: {
                                    int const startPoint = dims[setIdx].x * dims[setIdx].y * haloRadius * mData->cardinality;
                                    int const nElements = dims[setIdx].x * dims[setIdx].y * dims[setIdx].z * mData->cardinality;

                                    reductionInfo.startIDByView.push_back(startPoint);
                                    reductionInfo.nElementsByView.push_back(nElements);

                                    break;
                                }
                            }
                        }
                        break;
                    }
                    case Neon::DataView::INTERNAL: {
                        if (grid.getBackend().devSet().setCardinality() > 1) {
                            switch (mData->memoryOptions.getOrder()) {
                                case MemoryLayout::structOfArrays: {
                                    for (int c = 0; c < mData->cardinality; ++c) {

                                        auto const boundaryRadius = mData->zHaloDim;
                                        int const  startPoint = c * dims[setIdx].x * dims[setIdx].y * (dims[setIdx].z + 2 * haloRadius) +
                                                               dims[setIdx].x * dims[setIdx].y * (haloRadius + boundaryRadius);

                                        int const nElements = dims[setIdx].x * dims[setIdx].y * (dims[setIdx].z - 2 * haloRadius);

                                        reductionInfo.startIDByView.push_back(startPoint);
                                        reductionInfo.nElementsByView.push_back(nElements);
                                    }
                                    break;
                                }
                                case MemoryLayout::arrayOfStructs: {
                                    auto const boundaryRadius = mData->zHaloDim;
                                    int const  startPoint = dims[setIdx].x * dims[setIdx].y * (haloRadius + boundaryRadius) * mData->cardinality;
                                    int const  nElements = dims[setIdx].x * dims[setIdx].y * (dims[setIdx].z - 2 * haloRadius) * mData->cardinality;

                                    reductionInfo.startIDByView.push_back(startPoint);
                                    reductionInfo.nElementsByView.push_back(nElements);

                                    break;
                                }
                            }
                        }
                        break;
                    }
                    case Neon::DataView::BOUNDARY: {
                        if (grid.getBackend().devSet().setCardinality() > 1) {
                            switch (mData->memoryOptions.getOrder()) {
                                case MemoryLayout::structOfArrays: {
                                    for (int c = 0; c < mData->cardinality; ++c) {
                                        {  // up
                                            auto const boundaryRadius = mData->zHaloDim;
                                            int const  startPoint = c * dims[setIdx].x * dims[setIdx].y * (dims[setIdx].z + 2 * haloRadius) +
                                                                   dims[setIdx].x * dims[setIdx].y * haloRadius;
                                            int const nElements = dims[setIdx].x * dims[setIdx].y * boundaryRadius;

                                            reductionInfo.startIDByView.push_back(startPoint);
                                            reductionInfo.nElementsByView.push_back(nElements);
                                        }

                                        {  // down
                                            auto const boundaryRadius = mData->zHaloDim;
                                            int const  startPoint = c * dims[setIdx].x * dims[setIdx].y * (dims[setIdx].z + 2 * haloRadius) +
                                                                   dims[setIdx].x * dims[setIdx].y * (dims[setIdx].z + haloRadius - boundaryRadius);
                                            int const nElements = dims[setIdx].x * dims[setIdx].y * boundaryRadius;

                                            reductionInfo.startIDByView.push_back(startPoint);
                                            reductionInfo.nElementsByView.push_back(nElements);
                                        }
                                    }
                                    break;
                                }
                                case MemoryLayout::arrayOfStructs: {
                                    {  // up
                                        auto const boundaryRadius = mData->zHaloDim;
                                        int const  startPoint = dims[setIdx].x * dims[setIdx].y * haloRadius * mData->cardinality;
                                        int const  nElements = dims[setIdx].x * dims[setIdx].y * boundaryRadius * mData->cardinality;

                                        reductionInfo.startIDByView.push_back(startPoint);
                                        reductionInfo.nElementsByView.push_back(nElements);
                                    }
                                    {  // down
                                        auto const boundaryRadius = mData->zHaloDim;
                                        int const  startPoint = dims[setIdx].x * dims[setIdx].y * (dims[setIdx].z + haloRadius - boundaryRadius) * mData->cardinality;
                                        int const  nElements = dims[setIdx].x * dims[setIdx].y * boundaryRadius * mData->cardinality;

                                        reductionInfo.startIDByView.push_back(startPoint);
                                        reductionInfo.nElementsByView.push_back(nElements);
                                    }
                                    break;
                                }
                            }
                        }
                        break;
                    }
                    default: {
                        NeonException exp("dFieldDev_t");
                        exp << " Invalid DataView";
                        NEON_THROW(exp);
                    }
                }
            });

        this->initHaloUpdateTable();
    }
}


template <typename T, int C>
auto dField<T, C>::updateDeviceData(int streamSetId)
    -> void
{
    mData->memoryField.updateDeviceData(streamSetId);
}

template <typename T, int C>
auto dField<T, C>::updateHostData(int streamSetId)
    -> void
{
    mData->memoryField.updateHostData(streamSetId);
}

template <typename T, int C>
auto dField<T, C>::getPartition(Neon::Execution       execution,
                                Neon::SetIdx          setIdx,
                                const Neon::DataView& dataView)
    const
    -> const Partition&
{
    const Neon::DataUse dataUse = this->getDataUse();
    bool                isOk = Neon::ExecutionUtils::checkCompatibility(dataUse, execution);
    if (isOk) {
        Partition const& result = mData->partitionTable.getPartition(execution, setIdx, dataView);
        return result;
    }
    std::stringstream message;
    message << "The requested execution mode ( " << execution << " ) is not compatible with the field DataUse (" << dataUse << ")";
    NEON_THROW_UNSUPPORTED_OPERATION(message.str());
}

template <typename T, int C>
auto dField<T, C>::getPartition(Neon::Execution       execution,
                                Neon::SetIdx          setIdx,
                                const Neon::DataView& dataView)
    -> Partition&
{
    const auto dataUse = this->getDataUse();
    bool       isOk = Neon::ExecutionUtils::checkCompatibility(dataUse, execution);
    if (isOk) {
        Partition& result = mData->partitionTable.getPartition(execution, setIdx, dataView);
        return result;
    }
    std::stringstream message;
    message << "The requested execution mode ( " << execution << " ) is not compatible with the field DataUse (" << dataUse << ")";
    NEON_THROW_UNSUPPORTED_OPERATION(message.str());
}

template <typename T, int C>
auto dField<T, C>::operator()(const Neon::index_3d& idxGlobal,
                              const int&            cardinality) const
    -> Type
{
    auto [localIDx, partitionIdx] = helpGlobalIdxToPartitionIdx(idxGlobal);
    auto& partition = mData->partitionTable.getPartition(Neon::Execution::host,
                                                         partitionIdx,
                                                         Neon::DataView::STANDARD);
    auto& span = mData->grid->getSpan(Neon::Execution::host,partitionIdx, Neon::DataView::STANDARD);
    Idx   idx;
    bool  isOk = span.setAndValidate(idx, localIDx.x, localIDx.y, localIDx.z);
    if (!isOk) {
#pragma omp barrier
        NEON_THROW_UNSUPPORTED_OPERATION("");
    }
    auto& result = partition(idx, cardinality);
    return result;
}

template <typename T, int C>
auto dField<T, C>::getReference(const Neon::index_3d& idxGlobal,
                                const int&            cardinality)
    -> Type&
{
    auto [localIDx, partitionIdx] = helpGlobalIdxToPartitionIdx(idxGlobal);
    auto& partition = mData->partitionTable.getPartition(Neon::Execution::host,
                                                         partitionIdx,
                                                         Neon::DataView::STANDARD);
    auto& span = mData->grid->getSpan(Neon::Execution::host,partitionIdx, Neon::DataView::STANDARD);
    Idx   idx;
    bool  isOk = span.setAndValidate(idx, localIDx.x, localIDx.y, localIDx.z);
    if (!isOk) {
#pragma omp barrier
        NEON_THROW_UNSUPPORTED_OPERATION("");
    }
    auto& result = partition(idx, cardinality);
    return result;
}

template <typename T, int C>
auto dField<T, C>::initHaloUpdateTable()
    -> void
{
    auto& grid = this->getGrid();
    auto  bk = grid.getBackend();
    auto  getNghSetIdx = [&](SetIdx setIdx, Neon::domain::tool::partitioning::ByDirection direction) {
        int res;
        if (direction == Neon::domain::tool::partitioning::ByDirection::up) {
            res = (setIdx + 1) % bk.getDeviceCount();
        } else {
            res = (setIdx + bk.getDeviceCount() - 1) % bk.getDeviceCount();
        }
        return res;
    };

    mData->soaHaloUpdateTable.forEachPutConfiguration(
        bk, [&](Neon::SetIdx                                  setIdxSrc,
                Execution                                     execution,
                Neon::domain::tool::partitioning::ByDirection byDirection,
                std::vector<Neon::set::MemoryTransfer>&       transfersVec) {
            {
                using namespace Neon::domain::tool::partitioning;

                Neon::SetIdx setIdxDst = getNghSetIdx(setIdxSrc, byDirection);

                int r = grid.getStencil().getRadius();

                std::array<Partition*, Data::EndPointsUtils::nConfigs>                                  partitions;
                std::array<std::array<int, ByDirectionUtils::nConfigs>, Data::EndPointsUtils::nConfigs> ghostZBeginIdx;
                std::array<std::array<int, ByDirectionUtils::nConfigs>, Data::EndPointsUtils::nConfigs> boundaryZBeginIdx;
                std::array<Neon::size_4d, Data::EndPointsUtils::nConfigs>                               memPhyDim;

                partitions[Data::EndPoints::dst] = &this->getPartition(execution, setIdxDst, Neon::DataView::STANDARD);
                partitions[Data::EndPoints::src] = &this->getPartition(execution, setIdxSrc, Neon::DataView::STANDARD);

                for (auto endPoint : {Data::EndPoints::dst, Data::EndPoints::src}) {
                    ghostZBeginIdx[endPoint][static_cast<int>(ByDirection::down)] = 0;
                    boundaryZBeginIdx[endPoint][static_cast<int>(ByDirection::down)] = r;
                    boundaryZBeginIdx[endPoint][static_cast<int>(ByDirection::up)] = partitions[endPoint]->dim().z;
                    ghostZBeginIdx[endPoint][static_cast<int>(ByDirection::up)] = partitions[endPoint]->dim().z + r;

                    memPhyDim[endPoint] = Neon::size_4d(
                        1,
                        size_t(partitions[endPoint]->dim().x),
                        size_t(partitions[endPoint]->dim().x) * partitions[endPoint]->dim().y,
                        size_t(partitions[endPoint]->dim().x) * partitions[endPoint]->dim().y * (partitions[endPoint]->dim().z + 2 * r));
                }

                for (int j = 0; j < this->getCardinality(); j++) {

                    T* srcMem = partitions[Data::EndPoints::src]->mem();
                    T* dstMem = partitions[Data::EndPoints::dst]->mem();

                    Neon::size_4d srcBoundaryBuff(0, 0, boundaryZBeginIdx[Data::EndPoints::src][static_cast<int>(byDirection)], j);
                    Neon::size_4d dstGhostBuff(0, 0, ghostZBeginIdx[Data::EndPoints::dst][static_cast<int>(ByDirectionUtils::invert(byDirection))], j);

                    //                    std::cout << "To  " << dstGhostBuff << " prt " << partitions[Data::EndPoints::dst]->prtID() << " From  " << srcBoundaryBuff << "(src dim" << partitions[Data::EndPoints::src]->dim() << ")" << std::endl;
                    //                    std::cout << "dst mem " << partitions[Data::EndPoints::dst]->mem() << " " << std::endl;
                    //                    std::cout << "dst pitch " << (dstGhostBuff * memPhyDim[Data::EndPoints::dst]).rSum() << " " << std::endl;
                    //                    std::cout << "dst dstGhostBuff " << dstGhostBuff << " " << std::endl;
                    //                    std::cout << "dst pitch all" << memPhyDim[Data::EndPoints::dst] << " " << std::endl;

                    Neon::set::MemoryTransfer transfer({setIdxDst, dstMem + (dstGhostBuff * memPhyDim[Data::EndPoints::dst]).rSum(), dstGhostBuff},
                                                       {setIdxSrc, srcMem + (srcBoundaryBuff * memPhyDim[Data::EndPoints::src]).rSum(), srcBoundaryBuff},
                                                       sizeof(T) *
                                                           r *
                                                           partitions[Data::EndPoints::src]->dim().x *
                                                           partitions[Data::EndPoints::src]->dim().y);
                    if (ByDirection::up == byDirection && bk.isLastDevice(setIdxSrc)) {
                        return;
                    }

                    if (ByDirection::down == byDirection && bk.isFirstDevice(setIdxSrc)) {
                        return;
                    }

                    // std::cout << transfer.toString() << std::endl;
                    transfersVec.push_back(transfer);
                }
            }
        });

    mData->aosHaloUpdateTable.forEachPutConfiguration(
        bk, [&](Neon::SetIdx                                  setIdxSrc,
                Execution                                     execution,
                Neon::domain::tool::partitioning::ByDirection byDirection,
                std::vector<Neon::set::MemoryTransfer>&       transfersVec) {
            {
                using namespace Neon::domain::tool::partitioning;

                Neon::SetIdx setIdxDst = getNghSetIdx(setIdxSrc, byDirection);

                int r = grid.getStencil().getRadius();

                std::array<Partition*, Data::EndPointsUtils::nConfigs>                                  partitions;
                std::array<std::array<int, ByDirectionUtils::nConfigs>, Data::EndPointsUtils::nConfigs> ghostZBeginIdx;
                std::array<std::array<int, ByDirectionUtils::nConfigs>, Data::EndPointsUtils::nConfigs> boundaryZBeginIdx;
                std::array<Neon::size_4d, Data::EndPointsUtils::nConfigs>                               memPhyDim;

                partitions[Data::EndPoints::dst] = &this->getPartition(execution, setIdxDst, Neon::DataView::STANDARD);
                partitions[Data::EndPoints::src] = &this->getPartition(execution, setIdxSrc, Neon::DataView::STANDARD);

                for (auto endPoint : {Data::EndPoints::dst, Data::EndPoints::src}) {
                    ghostZBeginIdx[endPoint][static_cast<int>(ByDirection::down)] = 0;
                    boundaryZBeginIdx[endPoint][static_cast<int>(ByDirection::down)] = r;
                    boundaryZBeginIdx[endPoint][static_cast<int>(ByDirection::up)] = partitions[endPoint]->dim().z;
                    ghostZBeginIdx[endPoint][static_cast<int>(ByDirection::up)] = partitions[endPoint]->dim().z + r;

                    memPhyDim[endPoint] = Neon::size_4d(
                        this->getCardinality(),
                        size_t(partitions[endPoint]->dim().x * this->getCardinality()),
                        size_t(partitions[endPoint]->dim().x * this->getCardinality()) * partitions[endPoint]->dim().y,
                        1);
                }


                T* srcMem = partitions[Data::EndPoints::src]->mem();
                T* dstMem = partitions[Data::EndPoints::dst]->mem();

                Neon::size_4d srcBoundaryBuff(0, 0, boundaryZBeginIdx[Data::EndPoints::src][static_cast<int>(byDirection)], 0);
                Neon::size_4d dstGhostBuff(0, 0, ghostZBeginIdx[Data::EndPoints::dst][static_cast<int>(ByDirectionUtils::invert(byDirection))], 0);

                //                    std::cout << "To  " << dstGhostBuff << " prt " << partitions[Data::EndPoints::dst]->prtID() << " From  " << srcBoundaryBuff << "(src dim" << partitions[Data::EndPoints::src]->dim() << ")" << std::endl;
                //                    std::cout << "dst mem " << partitions[Data::EndPoints::dst]->mem() << " " << std::endl;
                //                    std::cout << "dst pitch " << (dstGhostBuff * memPhyDim[Data::EndPoints::dst]).rSum() << " " << std::endl;
                //                    std::cout << "dst dstGhostBuff " << dstGhostBuff << " " << std::endl;
                //                    std::cout << "dst pitch all" << memPhyDim[Data::EndPoints::dst] << " " << std::endl;

                Neon::set::MemoryTransfer transfer({setIdxDst, dstMem + (dstGhostBuff * memPhyDim[Data::EndPoints::dst]).rSum(), dstGhostBuff},
                                                   {setIdxSrc, srcMem + (srcBoundaryBuff * memPhyDim[Data::EndPoints::src]).rSum(), srcBoundaryBuff},
                                                   sizeof(T) *
                                                       r * this->getCardinality() *
                                                       partitions[Data::EndPoints::src]->dim().x *
                                                       partitions[Data::EndPoints::src]->dim().y);
                if (ByDirection::up == byDirection && bk.isLastDevice(setIdxSrc)) {
                    return;
                }

                if (ByDirection::down == byDirection && bk.isFirstDevice(setIdxSrc)) {
                    return;
                }

                // std::cout << transfer.toString() << std::endl;
                transfersVec.push_back(transfer);
            }
        });
    //
    //    mData->latticeHaloUpdateTable.forEachPutConfiguration(
    //        bk, [&](Neon::SetIdx                                  setIdxSrc,
    //                Execution                                     execution,
    //                Neon::domain::tool::partitioning::ByDirection byDirection,
    //                std::vector<Neon::set::MemoryTransfer>&       transfersVec) {
    //            {
    //                using namespace Neon::domain::tool::partitioning;
    //
    //                Neon::SetIdx setIdxDst = getNghSetIdx(setIdxSrc, byDirection);
    //                auto&        srcPartition = this->getPartition(execution, setIdxSrc, Neon::DataView::STANDARD);
    //                auto&        dstPartition = this->getPartition(execution, setIdxDst, Neon::DataView::STANDARD);
    //
    //                int r = grid.getStencil().getRadius();
    //
    //                int ghostZBeginIdx[2];
    //                int boundaryZBeginIdx[2];
    //
    //                ghostZBeginIdx[static_cast<int>(ByDirection::down)] = 0;
    //                ghostZBeginIdx[static_cast<int>(ByDirection::up)] = grid.getDimension().z + r;
    //
    //                boundaryZBeginIdx[static_cast<int>(ByDirection::down)] = r;
    //                boundaryZBeginIdx[static_cast<int>(ByDirection::up)] = grid.getDimension().z;
    //
    //                Neon::size_4d memPitch(1,
    //                                       grid.getDimension().x,
    //                                       grid.getDimension().x * grid.getDimension().y,
    //                                       grid.getDimension().x * grid.getDimension().y * (grid.getDimension().z + 2 * r));
    //
    //                for (int j = 0; j < this->getCardinality(); j++) {
    //
    //                    T* srcMem = srcPartition.mem();
    //                    T* dstMem = dstPartition.mem();
    //
    //                    Neon::size_4d srcBoundaryBuff(0, 0, boundaryZBeginIdx[static_cast<int>(byDirection)], j);
    //                    Neon::size_4d dstGhostBuff(0, 0, ghostZBeginIdx[static_cast<int>(byDirection)], j);
    //
    //                    Neon::set::MemoryTransfer transfer({setIdxDst, dstMem + dstGhostBuff.mPitch(memPitch)},
    //                                                       {setIdxSrc, srcMem + srcBoundaryBuff.mPitch(memPitch)},
    //                                                       grid.getDimension().x * grid.getDimension().y * sizeof(T));
    //
    //
    //                    transfersVec.push_back(transfer);
    //                }
    //            }
    //        });
}


template <typename T, int C>
auto dField<T, C>::ioToVtiPartitions(std::string const& fname) const -> void
{
    auto bk = mData->grid->getBackend();
    bk.forEachDeviceSeq([&](Neon::SetIdx setIdx) {
        auto partition = this->getPartition(Neon::Execution::device, setIdx, Neon::DataView::STANDARD);
        partition.ioToVti(fname, "sdfsd");
    });
}

template <typename T, int C>
auto dField<T, C>::
    newHaloUpdate(Neon::set::StencilSemantic stencilSemantic,
                  Neon::set::TransferMode    transferMode,
                  Neon::Execution            execution)
        const -> Neon::set::Container
{


    // We need to define a graph of Containers
    // One for the actual memory transfer
    // One for the synchronization
    // The order depends on the transfer mode: put or get
    Neon::set::Container dataTransferContainer;
    auto const&          bk = this->getGrid().getBackend();

    if (stencilSemantic == Neon::set::StencilSemantic::standard) {
        auto transfers = bk.template newDataSet<std::vector<Neon::set::MemoryTransfer>>();

        if (this->getMemoryOptions().getOrder() == Neon::MemoryLayout::structOfArrays) {
            for (auto byDirection : {tool::partitioning::ByDirection::up,
                                     tool::partitioning::ByDirection::down}) {

                auto const& tableEntryByDir = mData->soaHaloUpdateTable.get(transferMode,
                                                                            execution,
                                                                            byDirection);

                tableEntryByDir.forEachSeq([&](SetIdx setIdx, auto const& tableEntryByDirBySetIdx) {
                    transfers[setIdx].insert(std::end(transfers[setIdx]),
                                             std::begin(tableEntryByDirBySetIdx),
                                             std::end(tableEntryByDirBySetIdx));
                });
            }
            dataTransferContainer =
                Neon::set::Container::factoryDataTransfer(
                    *this,
                    transferMode,
                    stencilSemantic,
                    transfers,
                    execution);


        } else {
            for (auto byDirection : {tool::partitioning::ByDirection::up,
                                     tool::partitioning::ByDirection::down}) {

                auto const& tableEntryByDir = mData->aosHaloUpdateTable.get(transferMode,
                                                                            execution,
                                                                            byDirection);

                tableEntryByDir.forEachSeq([&](SetIdx setIdx, auto const& tableEntryByDirBySetIdx) {
                    transfers[setIdx].insert(std::end(transfers[setIdx]),
                                             std::begin(tableEntryByDirBySetIdx),
                                             std::end(tableEntryByDirBySetIdx));
                });
            }
            dataTransferContainer =
                Neon::set::Container::factoryDataTransfer(
                    *this,
                    transferMode,
                    stencilSemantic,
                    transfers,
                    execution);
        }
    } else {
        NEON_DEV_UNDER_CONSTRUCTION("");
    }
    Neon::set::Container SyncContainer =
        Neon::set::Container::factorySynchronization(
            *this,
            Neon::set::SynchronizationContainerType::hostOmpBarrier);

    Neon::set::container::Graph graph(this->getBackend());
    const auto&                 dataTransferNode = graph.addNode(dataTransferContainer);
    const auto&                 syncNode = graph.addNode(SyncContainer);

    switch (transferMode) {
        case Neon::set::TransferMode::put:
            graph.addDependency(dataTransferNode, syncNode, Neon::GraphDependencyType::data);
            break;
        case Neon::set::TransferMode::get:
            graph.addDependency(syncNode, dataTransferNode, Neon::GraphDependencyType::data);
            break;
        default:
            NEON_THROW_UNSUPPORTED_OPTION();
            break;
    }

    graph.removeRedundantDependencies();

    Neon::set::Container output =
        Neon::set::Container::factoryGraph("dGrid-Halo-Update",
                                           graph,
                                           [](Neon::SetIdx, Neon::set::Loader&) {});
    return output;
}

template <typename T, int C>
auto dField<T, C>::self() -> dField::Self&
{
    return *this;
}

template <typename T, int C>
auto dField<T, C>::self() const -> const dField::Self&
{
    return *this;
}

template <typename T, int C>
auto dField<T, C>::constSelf() const -> const dField::Self&
{
    return *this;
}

template <typename T, int C>
auto dField<T, C>::swap(dField::Field& A, dField::Field& B) -> void
{
    Neon::domain::interface::FieldBaseTemplate<T, C, Grid, Partition, int>::swapUIDBeforeFullSwap(A, B);
    std::swap(A, B);
}

template <typename T, int C>
auto dField<T, C>::getData()
    -> Data&
{
    return *(mData.get());
}

template <typename T, int C>
auto dField<T, C>::helpGlobalIdxToPartitionIdx(Neon::index_3d const& index)
    const -> std::pair<Neon::index_3d, int>
{
    Neon::index_3d result = index;

    // since we partition along the z-axis, only the z-component of index will change
    const int32_t setCardinality = mData->grid->getBackend().devSet().setCardinality();
    if (setCardinality == 1) {
        return {result, 0};
    }

    Neon::set::DataSet<int> firstZindex = mData->grid->helpGetFirstZindex();

    for (int i = 0; i < setCardinality - 1; i++) {
        if (index.z < firstZindex[i + 1]) {
            result.z -= firstZindex[i];
            return {result, i};
        }
    }
    if (index.z < this->getGrid().getDimension().z) {
        result.z -= firstZindex[setCardinality - 1];
        return {result, setCardinality - 1};
    }

    NeonException exc("dField");
    exc << "Data inconsistency was detected";
    NEON_THROW(exc);
}

}  // namespace Neon::domain::details::dGrid
