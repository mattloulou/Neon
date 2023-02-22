#pragma once


#include "dGrid.h"

namespace Neon::domain::internal::exp::dGrid {

dGrid::Data::Data(const Neon::Backend& backend)
{
    partitionDims = backend.devSet().newDataSet<index_3d>({0, 0, 0});
    firstZIndex = backend.devSet().newDataSet<index_t>(0);

    halo = index_3d(0, 0, 0);
    spanTable = Neon::domain::tool::SpanTable<dSpan>(backend);
    reduceEngine = Neon::sys::patterns::Engine::cuBlas;
}


template <Neon::domain::ActiveCellLambda ActiveCellLambda>
dGrid::dGrid(const Neon::Backend&         backend,
             const Neon::int32_3d&        dimension,
             const ActiveCellLambda&      activeCellLambda,
             const Neon::domain::Stencil& stencil,
             const Vec_3d<double>&        spacing,
             const Vec_3d<double>&        origin)
{
    mData = std::make_shared<Data>(backend);
    const index_3d defaultBlockSize(256, 1, 1);

    {
        auto nElementsPerPartition = backend.devSet().template newDataSet<size_t>(0);
        // We do an initialization with nElementsPerPartition to zero,
        // then we reset to the computed number.
        dGrid::GridBase::init("dGrid",
                              backend,
                              dimension,
                              stencil,
                              nElementsPerPartition,
                              Neon::index_3d(256, 1, 1),
                              spacing,
                              origin);
    }

    const int32_t numDevices = getBackend().devSet().setCardinality();
    if (numDevices == 1) {
        // Single device
        mData->partitionDims[0] = getDimension();
    } else if (getDimension().z < numDevices) {
        NeonException exc("dGrid_t");
        exc << "The grid size in the z-direction (" << getDimension().z << ") is less the number of devices (" << numDevices
            << "). It is ambiguous how to distribute the gird";
        NEON_THROW(exc);
    } else {
        // we only partition along the z-direction. Each partition has uniform_z
        // along the z-direction. The rest is distribute to make the partitions
        // as equal as possible
        int32_t uniform_z = getDimension().z / numDevices;
        int32_t reminder = getDimension().z % numDevices;

        mData->firstZIndex[0] = 0;
        for (int32_t i = 0; i < numDevices; ++i) {
            mData->partitionDims[i].x = getDimension().x;
            mData->partitionDims[i].y = getDimension().y;
            if (i < reminder) {
                mData->partitionDims[i].z = uniform_z + 1;
            } else {
                mData->partitionDims[i].z = uniform_z;
            }
            if (i > 1) {
                mData->firstZIndex[i] = mData->firstZIndex[i - 1] +
                                        mData->partitionDims[i - 1].z;
            }
        }
    }

    {  // Computing halo size
        // we partition along z so we only need halo along z
        mData->halo = Neon::index_3d(0, 0, 0);
        for (const auto& ngh : stencil.neighbours()) {
            mData->halo.z = std::max(mData->halo.z, std::abs(ngh.z));
        }
    }

    {  // Computing halo size
        for (const auto& dw : DataViewUtil::validOptions()) {
            getDefaultLaunchParameters(dw) = getLaunchParameters(dw, defaultBlockSize, 0);
        }
    }

    {  // Initialization of the span table
        const int setCardinality = getDevSet().setCardinality();
        mData->spanTable.forEachConfiguration([&](Neon::SetIdx setIdx, Neon::DataView dw, dSpan& span) {
            span.m_dataView = dw;
            span.m_zHaloRadius = setCardinality == 1 ? 0 : mData->halo.z;
            span.m_zBoundaryRadius = mData->halo.z;
            span.m_dim = mData->partitionDims[setIdx.idx()];
        });
    }


    {  // a Grid allocation
        auto haloStatus = Neon::domain::haloStatus_et::ON;
        haloStatus = (backend.devSet().setCardinality() == 1) ? haloStatus_et::e::OFF : haloStatus;
        auto                       partitionMemoryDim = mData->partitionDims;
        Neon::set::DataSet<size_t> elementPerPartition = backend.devSet().template newDataSet<size_t>(
            [&](Neon::SetIdx setIdx, size_t& count) {
                size_3d dim = partitionMemoryDim[setIdx.idx()].newType<size_t>();
                if (haloStatus) {
                    dim.z = mData->halo.z * 2;
                }
                count = dim.rMul();
            });
        mData->memoryGrid = Neon::domain::aGrid(backend, elementPerPartition);
    }

    {  // Stencil Idx to 3d offset
        auto nPoints = backend.devSet().newDataSet<uint64_t>(stencil.nNeighbours());
        mData->stencilIdTo3dOffset = backend.devSet().template newMemSet<int8_3d>(Neon::DataUse::IO_COMPUTE,
                                                                                  1,
                                                                                  backend.getMemoryOptions(),
                                                                                  nPoints);
        for (int i = 0; i < stencil.nNeighbours(); ++i) {
            for (int devIdx = 0; devIdx < backend.devSet().setCardinality(); devIdx++) {
                index_3d      pLong = stencil.neighbours()[i];
                Neon::int8_3d pShort(pLong.x, pLong.y, pLong.z);
                mData->stencilIdTo3dOffset.eRef(devIdx, i) = pShort;
            }
        }
        mData->stencilIdTo3dOffset.updateCompute(backend, Neon::Backend::mainStreamIdx);
    }

    {  // Init base class information
        Neon::set::DataSet<size_t> nElementsPerPartition = backend.devSet().template newDataSet<size_t>([this](Neon::SetIdx idx, size_t& size) {
            size = mData->partitionDims[idx.idx()].template rMulTyped<size_t>();
        });
        dGrid::GridBase::init("dGrid",
                              backend,
                              dimension,
                              stencil,
                              nElementsPerPartition,
                              defaultBlockSize,
                              spacing,
                              origin);
    }
}


template <typename T, int C>
auto dGrid::newField(const std::string&  fieldUserName,
                     int                 cardinality,
                     [[maybe_unused]] T  inactiveValue,
                     Neon::DataUse       dataUse,
                     Neon::MemoryOptions memoryOptions) const
    -> dField<T, C>
{
    memoryOptions = getDevSet().sanitizeMemoryOption(memoryOptions);

    const auto haloStatus = Neon::domain::haloStatus_et::ON;

    if (C != 0 && cardinality != C) {
        NeonException exception("dGrid::newField Dynamic and static cardinality do not match.");
        NEON_THROW(exception);
    }

    dField<T, C> field(fieldUserName,
                       dataUse,
                       memoryOptions,
                       *this,
                       mData->partitionDims,
                       mData->halo.z,
                       haloStatus,
                       cardinality);

    return field;
}

auto dGrid::getMemoryGrid()
    const -> const Neon::domain::aGrid&
{
    return mData->memoryGrid;
}

template <typename LoadingLambda>
auto dGrid::getContainer(const std::string& name,
                         LoadingLambda      lambda)
    const
    -> Neon::set::Container
{
    const Neon::index_3d& defaultBlockSize = getDefaultBlock();
    Neon::set::Container  kContainer = Neon::set::Container::factory(name,
                                                                     Neon::set::internal::ContainerAPI::DataViewSupport::on,
                                                                     *this,
                                                                     lambda,
                                                                     defaultBlockSize,
                                                                     [](const Neon::index_3d&) { return size_t(0); });
    return kContainer;
}

template <typename LoadingLambda>
auto dGrid::getHostContainer(const std::string& name,
                             LoadingLambda      lambda)
    const
    -> Neon::set::Container
{
    const Neon::index_3d& defaultBlockSize = getDefaultBlock();
    Neon::set::Container  kContainer = Neon::set::Container::hostFactory(name,
                                                                         Neon::set::internal::ContainerAPI::DataViewSupport::on,
                                                                         *this,
                                                                         lambda,
                                                                         defaultBlockSize,
                                                                         [](const Neon::index_3d&) { return size_t(0); });
    return kContainer;
}


template <typename LoadingLambda>
auto dGrid::getContainer(const std::string& name,
                         index_3d           blockSize,
                         size_t             sharedMem,
                         LoadingLambda      lambda)
    const
    -> Neon::set::Container
{
    const Neon::index_3d& defaultBlockSize = getDefaultBlock();
    Neon::set::Container  kContainer = Neon::set::Container::factory(name,
                                                                     Neon::set::internal::ContainerAPI::DataViewSupport::on,
                                                                     *this,
                                                                     lambda,
                                                                     blockSize,
                                                                     [sharedMem](const Neon::index_3d&) { return sharedMem; });
    return kContainer;
}

template <typename T>
auto dGrid::newPatternScalar() const -> Neon::template PatternScalar<T>
{
    auto pattern = Neon::PatternScalar<T>(getBackend(), mData->reduceEngine);

    if (mData->reduceEngine == Neon::sys::patterns::Engine::CUB) {
        for (auto& dataview : {Neon::DataView::STANDARD,
                               Neon::DataView::INTERNAL,
                               Neon::DataView::BOUNDARY}) {
            auto launchParam = getLaunchParameters(dataview, getDefaultBlock(), 0);
            for (SetIdx id = 0; id < launchParam.cardinality(); id++) {
                uint32_t numBlocks = launchParam[id].cudaGrid().x *
                                     launchParam[id].cudaGrid().y *
                                     launchParam[id].cudaGrid().z;
                pattern.getBlasSet(dataview).getBlas(id.idx()).setNumBlocks(numBlocks);
            }
        }
    }
    return pattern;
}

template <typename T>
auto dGrid::dot(const std::string&               name,
                dField<T>&                       input1,
                dField<T>&                       input2,
                Neon::template PatternScalar<T>& scalar) const -> Neon::set::Container
{
    if (mData->reduceEngine == Neon::sys::patterns::Engine::cuBlas || getBackend().devType() == Neon::DeviceType::CPU) {
        return Neon::set::Container::factoryOldManaged(
            name,
            Neon::set::internal::ContainerAPI::DataViewSupport::on,
            Neon::set::ContainerPatternType::reduction,
            *this, [&](Neon::set::Loader& loader) {
                loader.load(input1);
                if (input1.getUid() != input2.getUid()) {
                    loader.load(input2);
                }
                loader.load(scalar);
                return [&](int streamIdx, Neon::DataView dataView) mutable -> void {
                    if (dataView != Neon::DataView::STANDARD && getBackend().devSet().setCardinality() == 1) {
                        NeonException exc("dGrid_t");
                        exc << "Reduction operation can only run on standard data view when the number of partitions/GPUs is 1";
                        NEON_THROW(exc);
                    }
                    scalar.setStream(streamIdx, dataView);
                    scalar(dataView) = input1.dot(scalar.getBlasSet(dataView),
                                                  input2, scalar.getTempMemory(dataView), dataView);
                    if (dataView == Neon::DataView::BOUNDARY) {
                        scalar(Neon::DataView::STANDARD) =
                            scalar(Neon::DataView::BOUNDARY) + scalar(Neon::DataView::INTERNAL);
                    }
                };
            });
    } else if (mData->reduceEngine == Neon::sys::patterns::Engine::CUB) {

        return Neon::set::Container::factoryOldManaged(
            name,
            Neon::set::internal::ContainerAPI::DataViewSupport::on,
            Neon::set::ContainerPatternType::reduction,
            *this, [&](Neon::set::Loader& loader) {
                loader.load(input1);
                if (input1.getUid() != input2.getUid()) {
                    loader.load(input2);
                }
                loader.load(scalar);

                return [&](int streamIdx, Neon::DataView dataView) mutable -> void {
                    if (dataView != Neon::DataView::STANDARD && getBackend().devSet().setCardinality() == 1) {
                        NeonException exc("dGrid_t");
                        exc << "Reduction operation can only run on standard data view when the number of partitions/GPUs is 1";
                        NEON_THROW(exc);
                    }
                    scalar.setStream(streamIdx, dataView);

                    // calc dot product and store results on device
                    input1.dotCUB(scalar.getBlasSet(dataView),
                                  input2,
                                  scalar.getTempMemory(dataView, Neon::DeviceType::CUDA),
                                  dataView);

                    // move to results to host
                    scalar.getTempMemory(dataView,
                                         Neon::DeviceType::CPU)
                        .template updateFrom<Neon::run_et::et::async>(
                            scalar.getBlasSet(dataView).getStream(),
                            scalar.getTempMemory(dataView, Neon::DeviceType::CUDA));

                    // sync
                    scalar.getBlasSet(dataView).getStream().sync();

                    // read the results
                    scalar(dataView) = 0;
                    int nGpus = getBackend().devSet().setCardinality();
                    for (int idx = 0; idx < nGpus; idx++) {
                        scalar(dataView) += scalar.getTempMemory(dataView, Neon::DeviceType::CPU).elRef(idx, 0, 0);
                    }

                    if (dataView == Neon::DataView::BOUNDARY) {
                        scalar(Neon::DataView::STANDARD) =
                            scalar(Neon::DataView::BOUNDARY) + scalar(Neon::DataView::INTERNAL);
                    }
                };
            });


    } else {
        NeonException exc("dGrid_t");
        exc << "Unsupported reduction engine";
        NEON_THROW(exc);
    }
}

template <typename T>
auto dGrid::norm2(const std::string&               name,
                  dField<T>&                       input,
                  Neon::template PatternScalar<T>& scalar) const -> Neon::set::Container
{
    if (mData->reduceEngine == Neon::sys::patterns::Engine::cuBlas || getBackend().devType() == Neon::DeviceType::CPU) {
        return Neon::set::Container::factoryOldManaged(
            name,
            Neon::set::internal::ContainerAPI::DataViewSupport::on,
            Neon::set::ContainerPatternType::reduction,
            *this, [&](Neon::set::Loader& loader) {
                loader.load(input);

                return [&](int streamIdx, Neon::DataView dataView) mutable -> void {
                    if (dataView != Neon::DataView::STANDARD && getBackend().devSet().setCardinality() == 1) {
                        NeonException exc("dGrid_t");
                        exc << "Reduction operation can only run on standard data view when the number of partitions/GPUs is 1";
                        NEON_THROW(exc);
                    }
                    scalar.setStream(streamIdx, dataView);
                    scalar(dataView) = input.norm2(scalar.getBlasSet(dataView),
                                                   scalar.getTempMemory(dataView), dataView);
                    if (dataView == Neon::DataView::BOUNDARY) {
                        scalar(Neon::DataView::STANDARD) =
                            std::sqrt(scalar(Neon::DataView::BOUNDARY) * scalar(Neon::DataView::BOUNDARY) +
                                      scalar(Neon::DataView::INTERNAL) * scalar(Neon::DataView::INTERNAL));
                    }
                };
            });
    } else if (mData->reduceEngine == Neon::sys::patterns::Engine::CUB) {
        return Neon::set::Container::factoryOldManaged(
            name,
            Neon::set::internal::ContainerAPI::DataViewSupport::on,
            Neon::set::ContainerPatternType::reduction,
            *this, [&](Neon::set::Loader& loader) {
                loader.load(input);

                return [&](int streamIdx, Neon::DataView dataView) mutable -> void {
                    if (dataView != Neon::DataView::STANDARD && getBackend().devSet().setCardinality() == 1) {
                        NeonException exc("dGrid_t");
                        exc << "Reduction operation can only run on standard data view when the number of partitions/GPUs is 1";
                        NEON_THROW(exc);
                    }
                    scalar.setStream(streamIdx, dataView);

                    // calc dot product and store results on device
                    input.norm2CUB(scalar.getBlasSet(dataView),
                                   scalar.getTempMemory(dataView, Neon::DeviceType::CUDA),
                                   dataView);

                    // move to results to host
                    scalar.getTempMemory(dataView,
                                         Neon::DeviceType::CPU)
                        .template updateFrom<Neon::run_et::et::async>(
                            scalar.getBlasSet(dataView).getStream(),
                            scalar.getTempMemory(dataView, Neon::DeviceType::CUDA));

                    // sync
                    scalar.getBlasSet(dataView).getStream().sync();

                    // read the results
                    scalar(dataView) = 0;
                    int nGpus = getBackend().devSet().setCardinality();
                    for (int idx = 0; idx < nGpus; idx++) {
                        scalar(dataView) += scalar.getTempMemory(dataView, Neon::DeviceType::CPU).elRef(idx, 0, 0);
                    }
                    scalar(dataView) = std::sqrt(scalar());

                    if (dataView == Neon::DataView::BOUNDARY) {
                        scalar(Neon::DataView::STANDARD) =
                            std::sqrt(scalar(Neon::DataView::BOUNDARY) * scalar(Neon::DataView::BOUNDARY) +
                                      scalar(Neon::DataView::INTERNAL) * scalar(Neon::DataView::INTERNAL));
                    }
                };
            });

    } else {
        NeonException exc("dGrid_t");
        exc << "Unsupported reduction engine";
        NEON_THROW(exc);
    }
}

}  // namespace Neon::domain::internal::exp::dGrid