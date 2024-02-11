#pragma once

#include "Neon/domain/details/bGridDisgMgpu//bGridDisgMgpu.h"
#include "Neon/domain/details/bGridDisgMgpu/bSpan.h"

namespace Neon::domain::details::bGridDisgMgpu {

template <typename T, int C, typename SBlock>
bPartition<T, C, SBlock>::bPartition()
    : mCardinality(0),
      mMem(nullptr),
      mStencilNghIndex(),
      mBlockConnectivity(nullptr),
      mMask(nullptr),
      mOrigin(0),
      mSetIdx(0)
{
}

template <typename T, int C, typename SBlock>
bPartition<T, C, SBlock>::
    bPartition(int                                           setIdx,
               int                                           cardinality,
               T*                                            mem,
               typename Idx::DataBlockIdx*                   blockConnectivity,
               typename SBlock::BitMask const* NEON_RESTRICT mask,
               Neon::int32_3d*                               origin,
               NghIdx*                                       stencilNghIndex,
               Neon::int32_3d                                mDomainSize,
               typename Idx::DataBlockCount                  mFirstDataBUP,
               typename Idx::DataBlockCount                  mFirstDataBDW,
               typename Idx::DataBlockCount                  mFirstDataGUP,
               typename Idx::DataBlockCount                  mFirstDataGDW,
               typename Idx::DataBlockCount                  mLastDataGDW)
    : mCardinality(cardinality),
      mMem(mem),
      mStencilNghIndex(stencilNghIndex),
      mBlockConnectivity(blockConnectivity),
      mMask(mask),
      mOrigin(origin),
      mSetIdx(setIdx),
      mDomainSize(mDomainSize)
{
    mSectorFirstBlockIdx[Sectors::bUp] = mFirstDataBUP;
    mSectorFirstBlockIdx[Sectors::bDw] = mFirstDataBDW;
    mSectorFirstBlockIdx[Sectors::gUp] = mFirstDataGUP;
    mSectorFirstBlockIdx[Sectors::gDw] = mFirstDataGDW;
    mSectorFirstBlockIdx[Sectors::after] = mLastDataGDW;
}

template <typename T, int C, typename SBlock>
NEON_CUDA_HOST_DEVICE inline auto bPartition<T, C, SBlock>::
    getGlobalIndex(const Idx& gidx)
        const -> Neon::index_3d
{
    auto location = mOrigin[gidx.mDataBlockIdx];
    location.x += gidx.mInDataBlockIdx.x;
    location.y += gidx.mInDataBlockIdx.y;
    location.z += gidx.mInDataBlockIdx.z;
    if constexpr (SBlock::isMultiResMode) {
        return location * mMultiResDiscreteIdxSpacing;
    }
    return location;
}

template <typename T, int C, typename SBlock>
NEON_CUDA_HOST_DEVICE inline auto bPartition<T, C, SBlock>::
    getDomainSize()
        const -> Neon::index_3d
{
    return mDomainSize;
}

template <typename T, int C, typename SBlock>
NEON_CUDA_HOST_DEVICE inline auto bPartition<T, C, SBlock>::
    getBlockViewIdx(const Idx& gidx)
        const -> BlockViewGridIdx
{
    BlockViewGridIdx res;
    res.manualSet(gidx.getDataBlockIdx());
    return res;
}

template <typename T, int C, typename SBlock>
inline NEON_CUDA_HOST_DEVICE auto bPartition<T, C, SBlock>::
    cardinality()
        const -> int
{
    return mCardinality;
}

template <typename T, int C, typename SBlock>
inline NEON_CUDA_HOST_DEVICE auto bPartition<T, C, SBlock>::
operator()(const Idx& cell,
           int        card) -> T&
{
    return mMem[helpGetPitch(cell, card)];
}

template <typename T, int C, typename SBlock>
inline NEON_CUDA_HOST_DEVICE auto bPartition<T, C, SBlock>::
operator()(const Idx& cell,
           int        card) const -> const T&
{
    return mMem[helpGetPitch(cell, card)];
}

template <typename T, int C, typename SBlock>
NEON_CUDA_HOST_DEVICE inline auto bPartition<T, C, SBlock>::
    mem() const -> T const*
{
    return mMem;
}

template <typename T, int C, typename SBlock>
inline NEON_CUDA_HOST_DEVICE auto bPartition<T, C, SBlock>::
    helpGetPitch(const Idx& idx, int card)
        const -> uint32_t
{
    uint32_t const pitch = helpGetValidIdxPitchExplicit(idx, card);
    return pitch;
}

template <typename T, int C, typename SBlock>
inline NEON_CUDA_HOST_DEVICE auto bPartition<T, C, SBlock>::
    helpGetValidIdxPitchExplicit(const Idx& idx, int card)
        const -> uint32_t
{
    // we are in the internal sector, so we have the standard AoSoA
    if (idx.getDataBlockIdx() < mSectorFirstBlockIdx[Sectors::first]) {

        uint32_t constexpr blockPitchByCard = SBlock::memBlockSizeX * SBlock::memBlockSizeY * SBlock::memBlockSizeZ;
        uint32_t const inBlockInCardPitch = idx.mInDataBlockIdx.x +
                                            SBlock::memBlockSizeX * idx.mInDataBlockIdx.y +
                                            (SBlock::memBlockSizeX * SBlock::memBlockSizeY) * idx.mInDataBlockIdx.z;
        uint32_t const blockAdnCardPitch = (idx.mDataBlockIdx * mCardinality + card) * blockPitchByCard;
        uint32_t const pitch = blockAdnCardPitch + inBlockInCardPitch;
        return pitch;
    }

    // We switch to the other sector where we have a SoA
    int sectorLenght = 0;
    int myFirstBlock = 0;

    auto const myBlockIdx = idx.getDataBlockIdx();
    for (int mySector = Sectors::gDw; mySector >= 0; mySector--) {
        if (myBlockIdx >= mSectorFirstBlockIdx[mySector]) {
            sectorLenght = mSectorFirstBlockIdx[mySector + 1] - mSectorFirstBlockIdx[mySector];
            myFirstBlock = mSectorFirstBlockIdx[mySector];
            break;
        }
    }
    int const denseX = (myBlockIdx - myFirstBlock) * SBlock::memBlockSizeX + idx.getInDataBlockIdx().x;
    int const denseY = idx.mInDataBlockIdx.y;
    int const denseZ = idx.mInDataBlockIdx.z;

    int const xStride = 1;
    int const yStride = SBlock::memBlockSizeX * sectorLenght;
    int const zStride = SBlock::memBlockSizeY * yStride;

    int const pitch = (SBlock::memBlockSizeX * SBlock::memBlockSizeY * SBlock::memBlockSizeZ) *
                          card *
                          sectorLenght +
                      denseX * xStride +
                      denseY * yStride +
                      denseZ * zStride;

    int const fullPitch = myFirstBlock *
                              SBlock::memBlockSizeX *
                              SBlock::memBlockSizeY *
                              SBlock::memBlockSizeZ *
                              cardinality() +
                          pitch;
    return fullPitch;
}

template <typename T, int C, typename SBlock>
inline NEON_CUDA_HOST_DEVICE auto bPartition<T, C, SBlock>::
    helpNghPitch(const Idx& nghIdx, int card)
        const -> std::tuple<bool, uint32_t>
{
    if (nghIdx.mDataBlockIdx == Span::getInvalidBlockId()) {
        return {false, 0};
    }

    const bool isActive = mMask[nghIdx.mDataBlockIdx].isActive(nghIdx.mInDataBlockIdx.x, nghIdx.mInDataBlockIdx.y, nghIdx.mInDataBlockIdx.z);
    if (!isActive) {
        return {false, 0};
    }
    auto const offset = helpGetValidIdxPitchExplicit(nghIdx, card);
    return {true, offset};
}

template <typename T, int C, typename SBlock>
NEON_CUDA_HOST_DEVICE inline auto bPartition<T, C, SBlock>::
    helpGetNghIdx(const Idx&    idx,
                  const NghIdx& offset)
        const -> Idx
{
    return this->helpGetNghIdx(idx, offset, mBlockConnectivity);
}

template <typename T, int C, typename SBlock>
NEON_CUDA_HOST_DEVICE inline auto bPartition<T, C, SBlock>::
    helpGetNghIdx(const Idx&                        idx,
                  const NghIdx&                     offset,
                  const typename Idx::DataBlockIdx* blockConnectivity)
        const -> Idx
{

    typename Idx::InDataBlockIdx ngh(idx.mInDataBlockIdx.x + offset.x,
                                     idx.mInDataBlockIdx.y + offset.y,
                                     idx.mInDataBlockIdx.z + offset.z);

    /**
     * 0 if no offset on the direction
     * 1 positive offset
     * -1 negative offset
     */
    const int xFlag = ngh.x < 0 ? -1 : (ngh.x >= SBlock::memBlockSizeX ? +1 : 0);
    const int yFlag = ngh.y < 0 ? -1 : (ngh.y >= SBlock::memBlockSizeX ? +1 : 0);
    const int zFlag = ngh.z < 0 ? -1 : (ngh.z >= SBlock::memBlockSizeX ? +1 : 0);

    const bool isLocal = (xFlag | yFlag | zFlag) == 0;
    if (!(isLocal)) {
        typename Idx::InDataBlockIdx remoteInBlockOffset;
        /**
         * Example
         * - 8 block (1D case)
         * Case 1:
         * |0,1,2,3|0,1,2,3|0,1,2,3|
         *        ^     ^
         *       -3     starting point
         *
         * - idx.inBlock = 2
         * - offset = -1
         * - remote.x = (2-3) - ((-1) * 4) = -1 + 4 = 3
         * Case 2:
         * |0,1,2,3|0,1,2,3|0,1,2,3|
         *                ^     ^
         *  starting point      +3 from 3
         *
         * - idx.inBlock = 3
         * - offset = (+3,0)
         * - remote.x = (7+3) - ((+1) * 8) = 10 - 8 = 2
         *
         * |0,1,2,3|0,1,2,3|0,1,2,3|
         *  ^                   ^
         *  -3 from 0          +3 from 3
         *
         * NOTE: if in one direction the neighbour offet is zero, xFalg is 0;
         * */

        Idx remoteNghIdx;
        remoteNghIdx.mInDataBlockIdx.x = ngh.x - xFlag * SBlock::memBlockSizeX;
        remoteNghIdx.mInDataBlockIdx.y = ngh.y - yFlag * SBlock::memBlockSizeX;
        remoteNghIdx.mInDataBlockIdx.z = ngh.z - zFlag * SBlock::memBlockSizeX;

        int connectivityJump = idx.mDataBlockIdx * 27 +
                               (xFlag + 1) +
                               (yFlag + 1) * 3 +
                               (zFlag + 1) * 9;
        remoteNghIdx.mDataBlockIdx = blockConnectivity[connectivityJump];

        return remoteNghIdx;
    } else {
        Idx localNghIdx;
        localNghIdx.mDataBlockIdx = idx.mDataBlockIdx;
        localNghIdx.mInDataBlockIdx = ngh;
        return localNghIdx;
    }
}

template <typename T, int C, typename SBlock>
template <int xOff, int yOff, int zOff>
NEON_CUDA_HOST_DEVICE inline auto bPartition<T, C, SBlock>::
    helpGetNghIdx(const Idx& idx)
        const -> Idx
{
    return this->helpGetNghIdx<xOff, yOff, zOff>(idx, mBlockConnectivity);
}

template <typename T, int C, typename SBlock>
template <int xOff, int yOff, int zOff>
NEON_CUDA_HOST_DEVICE inline auto bPartition<T, C, SBlock>::
    helpGetNghIdx(const Idx& idx, const typename Idx::DataBlockIdx* blockConnectivity)
        const -> Idx
{

    typename Idx::InDataBlockIdx ngh(idx.mInDataBlockIdx.x + xOff,
                                     idx.mInDataBlockIdx.y + yOff,
                                     idx.mInDataBlockIdx.z + zOff);

    /**
     * 0 if no offset on the direction
     * 1 positive offset
     * -1 negative offset
     */
    const int xFlag = [&] {
        if constexpr (xOff == 0) {
            return 0;
        } else {
            return ngh.x < 0 ? -1 : (ngh.x >= SBlock::memBlockSizeX ? +1 : 0);
        }
    }();


    const int yFlag = [&] {
        if constexpr (yOff == 0) {
            return 0;
        } else {
            return ngh.y < 0 ? -1 : (ngh.y >= SBlock::memBlockSizeX ? +1 : 0);
        }
    }();
    const int zFlag = [&] {
        if constexpr (zOff == 0) {
            return 0;
        } else {
            return ngh.z < 0 ? -1 : (ngh.z >= SBlock::memBlockSizeX ? +1 : 0);
        }
    }();

    const bool isLocal = (xFlag | yFlag | zFlag) == 0;
    if (!(isLocal)) {
        typename Idx::InDataBlockIdx remoteInBlockOffset;
        /**
         * Example
         * - 8 block (1D case)
         * Case 1:
         * |0,1,2,3|0,1,2,3|0,1,2,3|
         *        ^     ^
         *       -3     starting point
         *
         * - idx.inBlock = 2
         * - offset = -1
         * - remote.x = (2-3) - ((-1) * 4) = -1 + 4 = 3
         * Case 2:
         * |0,1,2,3|0,1,2,3|0,1,2,3|
         *                ^     ^
         *  starting point      +3 from 3
         *
         * - idx.inBlock = 3
         * - offset = (+3,0)
         * - remote.x = (7+3) - ((+1) * 8) = 10 - 8 = 2
         *
         * |0,1,2,3|0,1,2,3|0,1,2,3|
         *  ^                   ^
         *  -3 from 0          +3 from 3
         *
         * NOTE: if in one direction the neighbour offet is zero, xFalg is 0;
         * */

        Idx remoteNghIdx;
        remoteNghIdx.mInDataBlockIdx.x = ngh.x - xFlag * SBlock::memBlockSizeX;
        remoteNghIdx.mInDataBlockIdx.y = ngh.y - yFlag * SBlock::memBlockSizeX;
        remoteNghIdx.mInDataBlockIdx.z = ngh.z - zFlag * SBlock::memBlockSizeX;

        int connectivityJump = idx.mDataBlockIdx * 27 +
                               (xFlag + 1) +
                               (yFlag + 1) * 3 +
                               (zFlag + 1) * 9;
        remoteNghIdx.mDataBlockIdx = blockConnectivity[connectivityJump];

        return remoteNghIdx;
    } else {
        Idx localNghIdx;
        localNghIdx.mDataBlockIdx = idx.mDataBlockIdx;
        localNghIdx.mInDataBlockIdx = ngh;
        return localNghIdx;
    }
}

template <typename T, int C, typename SBlock>
NEON_CUDA_HOST_DEVICE inline auto bPartition<T, C, SBlock>::
    getNghData(const Idx& eId,
               uint8_t    nghID,
               int        card)
        const -> NghData
{
    NghIdx nghOffset = mStencilNghIndex[nghID];
    return getNghData(eId, nghOffset, card);
}

template <typename T, int C, typename SBlock>
NEON_CUDA_HOST_DEVICE inline auto bPartition<T, C, SBlock>::
    getNghData(const Idx&    idx,
               const NghIdx& offset,
               const int     card)
        const -> NghData
{
    NghData result;
    bIndex  nghIdx = helpGetNghIdx(idx, offset);
    auto [isValid, pitch] = helpNghPitch(nghIdx, card);
    if (!isValid) {
        result.invalidate();
        return result;
    }
    auto const value = mMem[pitch];
    result.set(value, true);
    return result;
}

template <typename T, int C, typename SBlock>
template <int xOff, int yOff, int zOff>
NEON_CUDA_HOST_DEVICE inline auto bPartition<T, C, SBlock>::
    getNghData(const Idx& idx,
               int        card)
        const -> NghData
{
    NghData result;
    bIndex  nghIdx = helpGetNghIdx<xOff, yOff, zOff>(idx);
    auto [isValid, pitch] = helpNghPitch(nghIdx, card);
    if (!isValid) {
        result.invalidate();
        return result;
    }
    auto const value = mMem[pitch];
    result.set(value, true);
    return result;
}

template <typename T, int C, typename SBlock>
template <int xOff, int yOff, int zOff>
NEON_CUDA_HOST_DEVICE inline auto bPartition<T, C, SBlock>::
    getNghData(const Idx& idx,
               int        card,
               T          defaultValue)
        const -> NghData
{
    NghData result;
    bIndex  nghIdx = helpGetNghIdx<xOff, yOff, zOff>(idx);
    auto [isValid, pitch] = helpNghPitch(nghIdx, card);
    if (!isValid) {
        result.set(defaultValue, false);
        return result;
    }
    auto const value = mMem[pitch];
    result.set(value, true);
    return result;
}

template <typename T, int C, typename SBlock>

template <int xOff,
          int yOff,
          int zOff,
          typename LambdaVALID,
          typename LambdaNOTValid>
NEON_CUDA_HOST_DEVICE inline auto bPartition<T, C, SBlock>::
    getNghData(const Idx&     gidx,
               int            card,
               LambdaVALID    funIfValid,
               LambdaNOTValid funIfNOTValid)
        const -> std::enable_if_t<std::is_invocable_v<LambdaVALID, T> && (std::is_invocable_v<LambdaNOTValid, T> || std::is_same_v<LambdaNOTValid, void*>), void>
{
    NghData result;
    bIndex  nghIdx = helpGetNghIdx<xOff, yOff, zOff>(gidx);
    auto [isValid, pitch] = helpNghPitch(nghIdx, card);

    if (isValid) {
        auto const& value = mMem[pitch];
        funIfValid(value);
        return;
    }

    if constexpr (!std::is_same_v<LambdaNOTValid, void*>) {
        funIfNOTValid();
    }
    return;
}

template <typename T, int C, typename SBlock>
template <int xOff, int yOff, int zOff>
NEON_CUDA_HOST_DEVICE inline auto bPartition<T, C, SBlock>::
    writeNghData(const Idx& gidx,
                 int        card,
                 T          value)
        -> bool
{
    NghData result;
    bIndex  nghIdx = helpGetNghIdx<xOff, yOff, zOff>(gidx);
    auto [isValid, pitch] = helpNghPitch(nghIdx, card);
    if (!isValid) {
        return false;
    }
    mMem[pitch] = value;
    return true;
}

template <typename T, int C, typename SBlock>
NEON_CUDA_HOST_DEVICE inline auto
bPartition<T, C, SBlock>::isActive(const Idx&                      cell,
                                   const typename SBlock::BitMask* mask) const -> bool
{
    if (!mask) {
        return mMask[cell.mDataBlockIdx].isActive(cell.mInDataBlockIdx.x, cell.mInDataBlockIdx.y, cell.mInDataBlockIdx.z);
    } else {
        return mask[cell.mDataBlockIdx].isActive(cell.mInDataBlockIdx.x, cell.mInDataBlockIdx.y, cell.mInDataBlockIdx.z);
    }
}

template <typename T, int C, typename SBlock>
NEON_CUDA_HOST_DEVICE inline auto
bPartition<T, C, SBlock>::helpGetSectorFirstBlock(Sectors sector) const
    -> typename Idx::DataBlockCount
{
    return mSectorFirstBlockIdx[sector];
}

template <typename T, int C, typename SBlock>
NEON_CUDA_HOST_DEVICE inline auto
bPartition<T, C, SBlock>::helpGetSectorLength(Sectors sector) const
    -> typename Idx::DataBlockCount
{
    return mSectorFirstBlockIdx[sector + 1] - mSectorFirstBlockIdx[sector];
}
}  // namespace Neon::domain::details::bGridMgpu