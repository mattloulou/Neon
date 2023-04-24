#pragma once

template <typename T, int Q>
inline Neon::set::Container store(Neon::domain::mGrid&           grid,
                                  int                            level,
                                  Neon::domain::mGrid::Field<T>& postCollision)
{
    //Initiated by the coarse level (level), this function prepares and stores the fine (level - 1)
    // information for further pulling initiated by the coarse (this) level invoked by coalescence_pull
    //
    //Where a coarse cell stores its information? at itself i.e., pull
    //Where a coarse cell reads the needed info? from its children and neighbor cell's children (level -1)
    //This function only operates on a coarse cell that has children.
    //For such cell, we check its neighbor cells at the same level. If any of these neighbor has NO
    //children, then we need to prepare something for them to be read during coalescence. What
    //we prepare is some sort of averaged the data from the children (the cell's children and/or
    //its neighbor's children)

    return grid.getContainer(
        "store_" + std::to_string(level), level,
        [&, level](Neon::set::Loader& loader) {
            auto& fpost_col = postCollision.load(loader, level, Neon::MultiResCompute::STENCIL_DOWN);

            return [=] NEON_CUDA_HOST_DEVICE(const typename Neon::domain::bGrid::Cell& cell) mutable {
                //if the cell is refined, we might need to store something in it for its neighbor
                if (fpost_col.hasChildren(cell)) {

                    const int refFactor = fpost_col.getRefFactor(level);

                    bool should_accumelate = ((int(fpost_col(cell, 0)) % refFactor) != 0);

                    //fpost_col(cell, 0) += 1;
                    fpost_col(cell, 0) = (int(fpost_col(cell, 0)) + 1) % refFactor;


                    //for each direction aka for each neighbor
                    //we skip the center here
                    for (int8_t q = 1; q < Q; ++q) {
                        const Neon::int8_3d q_dir = getDir(q);

                        //check if the neighbor in this direction has children
                        auto neighborCell = fpost_col.getNghCell(cell, q_dir);
                        if (neighborCell.isActive()) {

                            if (!fpost_col.hasChildren(neighborCell)) {
                                //now, we know that there is actually something we need to store for this neighbor
                                //in cell along q (q_dir) direction
                                int num = 0;
                                T   sum = 0;


                                //for every neighbor cell including the center cell (i.e., cell)
                                for (int8_t p = 0; p < Q; ++p) {
                                    const Neon::int8_3d p_dir = getDir(p);

                                    const auto p_cell = fpost_col.getNghCell(cell, p_dir);
                                    //relative direction of q w.r.t p
                                    //i.e., in which direction we should move starting from p to land on q
                                    const Neon::int8_3d r_dir = q_dir - p_dir;

                                    //if this neighbor is refined
                                    if (fpost_col.hasChildren(cell, p_dir)) {

                                        //for each children of p
                                        for (int8_t i = 0; i < refFactor; ++i) {
                                            for (int8_t j = 0; j < refFactor; ++j) {
                                                for (int8_t k = 0; k < refFactor; ++k) {
                                                    const Neon::int8_3d c(i, j, k);

                                                    //cq is coarse neighbor (i.e., uncle) that we need to go in order to read q
                                                    //for c (this is what we do for explosion but here we do this just for the check)
                                                    const Neon::int8_3d cq = uncleOffset(c, q_dir);
                                                    if (cq == r_dir) {
                                                        auto childVal = fpost_col.childVal(p_cell, c, q, 0);
                                                        if (childVal.isValid) {
                                                            num++;
                                                            sum += childVal.value;
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                if (should_accumelate) {
                                    fpost_col(cell, q) += sum / static_cast<T>(num * refFactor);
                                } else {
                                    fpost_col(cell, q) = sum / static_cast<T>(num * refFactor);
                                }
                            }
                        }
                    }
                }
            };
        });
}