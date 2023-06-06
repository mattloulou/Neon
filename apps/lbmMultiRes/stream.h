#pragma once
#include "coalescence.h"
#include "explosion.h"

template <typename T, int Q>
inline Neon::set::Container stream(Neon::domain::mGrid&                        grid,
                                   const int                                   level,
                                   const Neon::domain::mGrid::Field<CellType>& cellType,
                                   const Neon::domain::mGrid::Field<T>&        fout,
                                   Neon::domain::mGrid::Field<T>&              fin)
{
    //regular Streaming of the normal voxels at level L which are not interfaced with L+1 and L-1 levels.
    //This is "pull" stream

    return grid.getContainer(
        "stream_" + std::to_string(level), level,
        [&, level](Neon::set::Loader& loader) {
            const auto& type = cellType.load(loader, level, Neon::MultiResCompute::STENCIL);
            const auto& pout = fout.load(loader, level, Neon::MultiResCompute::STENCIL);
            auto        pin = fin.load(loader, level, Neon::MultiResCompute::MAP);

            return [=] NEON_CUDA_HOST_DEVICE(const typename Neon::domain::bGrid::Cell& cell) mutable {
                if (type(cell, 0) == CellType::bulk) {
                    //If this cell has children i.e., it is been refined, than we should not work on it
                    //because this cell is only there to allow query and not to operate on
                    if (!pin.hasChildren(cell)) {

                        for (int8_t q = 0; q < Q; ++q) {
                            const Neon::int8_3d dir = -getDir(q);

                            //if the neighbor cell has children, then this 'cell' is interfacing with L-1 (fine) along q direction
                            if (!pin.hasChildren(cell, dir)) {
                                auto nghType = type.nghVal(cell, dir, 0, CellType::undefined);

                                if (nghType.isValid) {
                                    if (nghType.value == CellType::bulk) {
                                        pin(cell, q) = pout.nghVal(cell, dir, q, T(0)).value;
                                    } else {
                                        const int8_t opposte_q = latticeOppositeID[q];
                                        pin(cell, q) = pout(cell, opposte_q) + pout.nghVal(cell, dir, opposte_q, T(0)).value;
                                    }
                                }
                            }
                        }
                    }
                }
            };
        });
}


template <typename T, int Q>
inline Neon::set::Container streamFusedExplosion(Neon::domain::mGrid&                        grid,
                                                 const int                                   level,
                                                 const int                                   numLevels,
                                                 const Neon::domain::mGrid::Field<CellType>& cellType,
                                                 const Neon::domain::mGrid::Field<T>&        fout,
                                                 Neon::domain::mGrid::Field<T>&              fin)
{
    //regular Streaming of the normal voxels at level L which are not interfaced with L+1 and L-1 levels.
    //This is "pull" stream

    return grid.getContainer(
        "streamFusedExplosion_" + std::to_string(level), level,
        [&, level, numLevels](Neon::set::Loader& loader) {
            const auto& type = cellType.load(loader, level, Neon::MultiResCompute::STENCIL);

            auto pin = fin.load(loader, level, Neon::MultiResCompute::MAP);

            const auto& pout = fout.load(loader, level, Neon::MultiResCompute::STENCIL);

            if (level != numLevels - 1) {
                fout.load(loader, level, Neon::MultiResCompute::STENCIL_UP);
            }

            return [=] NEON_CUDA_HOST_DEVICE(const typename Neon::domain::bGrid::Cell& cell) mutable {
                //We only do streaming in the bulk i.e., non-boundary condition voxels. Since we only allow grid
                //transition on bulk, then it is okay to do explosion inside this condition because
                //if the voxel is not bulk then all its neighbours are on the same level and no explosion is needed
                if (type(cell, 0) == CellType::bulk) {
                    //If this cell has children i.e., it is been refined, than we should not work on it
                    //because this cell is only there to allow query and not to operate on
                    if (!pin.hasChildren(cell)) {

                        for (int8_t q = 0; q < Q; ++q) {
                            const Neon::int8_3d dir = -getDir(q);

                            //if the neighbor cell has children, then this 'cell' is interfacing with L-1 (fine) along q direction
                            if (!pin.hasChildren(cell, dir)) {
                                auto neighborCell = pout.getNghCell(cell, dir);

                                if (neighborCell.isActive()) {
                                    auto nghType = type.nghVal(cell, dir, 0, CellType::undefined);
                                    assert(nghType.isValid);
                                    if (nghType.value == CellType::bulk) {
                                        pin(cell, q) = pout.nghVal(cell, dir, q, T(0)).value;
                                    } else {
                                        const int8_t opposte_q = latticeOppositeID[q];
                                        pin(cell, q) = pout(cell, opposte_q) + pout.nghVal(cell, dir, opposte_q, T(0)).value;
                                    }
                                } else if (level != numLevels - 1) {
                                    //only if we are not on the coarsest level and
                                    //only if we can not do normal streaming, then we may have a coarser neighbor from which
                                    //we can read this pop

                                    //get the uncle direction/offset i.e., the neighbor of the cell's parent
                                    //this direction/offset is wrt to the cell's parent
                                    Neon::int8_3d uncleDir = uncleOffset(cell.mLocation, dir);

                                    auto uncleLoc = pout.getUncle(cell, uncleDir);

                                    auto uncle = pout.uncleVal(cell, uncleDir, q, T(0));
                                    if (uncle.isValid) {
                                        pin(cell, q) = uncle.value;
                                    }
                                }
                            }
                        }
                    }
                }
            };
        });
}

template <typename T, int Q>
inline Neon::set::Container streamFusedCoalescence(Neon::domain::mGrid&                        grid,
                                                   const bool                                  fineInitStore,
                                                   const int                                   level,
                                                   const Neon::domain::mGrid::Field<int>&      sumStore,
                                                   const Neon::domain::mGrid::Field<CellType>& cellType,
                                                   const Neon::domain::mGrid::Field<T>&        fout,
                                                   Neon::domain::mGrid::Field<T>&              fin)
{
    return grid.getContainer(
        "streamFusedCoalescence_" + std::to_string(level), level,
        [&, level, fineInitStore](Neon::set::Loader& loader) {
            const auto& type = cellType.load(loader, level, Neon::MultiResCompute::STENCIL);
            const auto& pout = fout.load(loader, level, Neon::MultiResCompute::STENCIL);
            auto        pin = fin.load(loader, level, Neon::MultiResCompute::MAP);
            const auto& ss = sumStore.load(loader, level, Neon::MultiResCompute::STENCIL);

            return [=] NEON_CUDA_HOST_DEVICE(const typename Neon::domain::bGrid::Cell& cell) mutable {
                if (type(cell, 0) == CellType::bulk) {
                    //If this cell has children i.e., it is been refined, than we should not work on it
                    //because this cell is only there to allow query and not to operate on
                    if (!pin.hasChildren(cell)) {

                        const int refFactor = pout.getRefFactor(level);

                        for (int8_t q = 0; q < Q; ++q) {
                            const Neon::int8_3d dir = -getDir(q);

                            auto nghType = type.nghVal(cell, dir, 0, CellType::undefined);
                            if (nghType.isValid) {
                                if (nghType.value == CellType::bulk) {
                                    if (!pin.hasChildren(cell, dir)) {
                                        //if the neighbor cell has children, then this 'cell' is interfacing with L-1 (fine) along q direction
                                        pin(cell, q) = pout.nghVal(cell, dir, q, T(0)).value;
                                    } else if (!(dir.x == 0 && dir.y == 0 && dir.z == 0)) {
                                        //if we have a neighbor at the same level that has been refined, then cell is on
                                        //the interface and this is where we should do the coalescence. Only if it is not the center
                                        //We only do coalescence if the neighbour is bulk since the transition from one level to
                                        //another happens in the bulk (not at the boundary condition)
                                        if (fineInitStore) {
                                            auto ssVal = ss.nghVal(cell, dir, q, T(0));
                                            assert(ssVal.value != 0);
                                            pin(cell, q) = pout.nghVal(cell, dir, q, T(0)).value / static_cast<T>(ssVal.value * refFactor);
                                        } else {
                                            pin(cell, q) = pout.nghVal(cell, dir, q, T(0)).value / static_cast<T>(refFactor);
                                        }
                                    }
                                } else {
                                    assert(!pin.hasChildren(cell, dir));
                                    const int8_t opposte_q = latticeOppositeID[q];
                                    pin(cell, q) = pout(cell, opposte_q) + pout.nghVal(cell, dir, opposte_q, T(0)).value;
                                }
                            }
                        }
                    }
                }
            };
        });
}

template <typename T, int Q>
inline void stream(Neon::domain::mGrid&                        grid,
                   const bool                                  fineInitStore,
                   const int                                   level,
                   const int                                   numLevels,
                   const Neon::domain::mGrid::Field<CellType>& cellType,
                   const Neon::domain::mGrid::Field<int>&      sumStore,
                   const Neon::domain::mGrid::Field<T>&        fout,
                   Neon::domain::mGrid::Field<T>&              fin,
                   std::vector<Neon::set::Container>&          containers)
{
    containers.push_back(stream<T, Q>(grid, level, cellType, fout, fin));

    /*
    * Streaming for interface voxels that have
    *  (i) coarser or (ii) finer neighbors at level+1 and level-1 and hence require
    *  (i) "explosion" or (ii) coalescence
    */
    if (level != numLevels - 1) {
        /* Explosion: pull missing populations from coarser neighbors by copying coarse (level+1) to fine (level) 
        * neighbors, initiated by the fine level ("Pull").
        */
        containers.push_back(explosion<T, Q>(grid, level, fout, fin));
    }

    if (level != 0) {
        /* Coalescence: pull missing populations from finer neighbors by "smart" averaging fine (level-1) 
        * to coarse (level) communication, initiated by the coarse level ("Pull").
        */
        containers.push_back(coalescence<T, Q>(grid, fineInitStore, level, sumStore, fout, fin));
    }
}


template <typename T, int Q>
inline void streamFusedExplosion(Neon::domain::mGrid&                        grid,
                                 const bool                                  fineInitStore,
                                 const int                                   level,
                                 const int                                   numLevels,
                                 const Neon::domain::mGrid::Field<CellType>& cellType,
                                 const Neon::domain::mGrid::Field<int>&      sumStore,
                                 const Neon::domain::mGrid::Field<T>&        fout,
                                 Neon::domain::mGrid::Field<T>&              fin,
                                 std::vector<Neon::set::Container>&          containers)
{
    /*
    * Streaming for interface voxels that have
    *  (i) coarser or (ii) finer neighbors at level+1 and level-1 and hence require
    *  (i) "explosion" or (ii) coalescence
    * Explosion: pull missing populations from coarser neighbors by copying coarse (level+1) to fine (level) 
    * neighbors, initiated by the fine level ("Pull").
    * Here we fuse the explosion with stream and do the check if we need explosion (since we don't 
    * need explosion on the coarsest level) inside the container. 
    */
    containers.push_back(streamFusedExplosion<T, Q>(grid, level, numLevels, cellType, fout, fin));

    if (level != 0) {
        /* Coalescence: pull missing populations from finer neighbors by "smart" averaging fine (level-1) 
        * to coarse (level) communication, initiated by the coarse level ("Pull").
        */
        containers.push_back(coalescence<T, Q>(grid, fineInitStore, level, sumStore, fout, fin));
    }
}


template <typename T, int Q>
inline void streamFusedCoalescence(Neon::domain::mGrid&                        grid,
                                   const bool                                  fineInitStore,
                                   const int                                   level,
                                   const int                                   numLevels,
                                   const Neon::domain::mGrid::Field<CellType>& cellType,
                                   const Neon::domain::mGrid::Field<int>&      sumStore,
                                   const Neon::domain::mGrid::Field<T>&        fout,
                                   Neon::domain::mGrid::Field<T>&              fin,
                                   std::vector<Neon::set::Container>&          containers)
{
    containers.push_back(streamFusedCoalescence<T, Q>(grid, fineInitStore, level, sumStore, cellType, fout, fin));

    /*
    * Streaming for interface voxels that have
    *  (i) coarser or (ii) finer neighbors at level+1 and level-1 and hence require
    *  (i) "explosion" or (ii) coalescence
    */
    if (level != numLevels - 1) {
        /* Explosion: pull missing populations from coarser neighbors by copying coarse (level+1) to fine (level) 
        * neighbors, initiated by the fine level ("Pull").
        */
        containers.push_back(explosion<T, Q>(grid, level, fout, fin));
    }
}