/*****************************************************************************

YASK: Yet Another Stencil Kernel
Copyright (c) 2014-2016, Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to
deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

* The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.

*****************************************************************************/

// Stencil types.
#include "stencil.hpp"

// Base classes for stencil code.
#include "stencil_calc.hpp"

#include <sstream>
using namespace std;

namespace yask {

    ///// Top-level methods for evaluating reference and optimized stencils.

    // Eval stencil equation group(s) over grid(s) using scalar code.
    void StencilEqs::calc_rank_ref(StencilContext& context)
    {
        init(context);
        idx_t begin_dt = context.begin_dt;
        idx_t end_dt = begin_dt + context.dt;
        TRACE_MSG("calc_rank_ref(%ld..%ld)", begin_dt, end_dt-1);
        
        // Time steps.
        // TODO: check that scalar version actually does CPTS_T time steps.
        // (At this point, CPTS_T == 1 for all existing stencil examples.)
        for(idx_t t = context.begin_dt; t < context.begin_dt + context.dt; t += CPTS_T) {

            // equations to evaluate (only one in most stencils).
            for (auto eg : eqGroups) {

                // Halo+shadow exchange for grid(s) updated by this equation.
                eg->exchange_halos(context, t, t + CPTS_T);

                // Loop through 4D space within the bounding-box of this
                // equation set.
#pragma omp parallel for collapse(4)
                for (idx_t n = eg->begin_bbn; n < eg->end_bbn; n++)
                    for (idx_t x = eg->begin_bbx; x < eg->end_bbx; x++)
                        for (idx_t y = eg->begin_bby; y < eg->end_bby; y++)
                            for (idx_t z = eg->begin_bbz; z < eg->end_bbz; z++) {

                                // Update only if point in domain for this eq group.
                                // NB: this isn't actually needed for rectangular BBs.
                                if (eg->is_in_valid_domain(context, t, n, x, y, z)) {
                                    
                                    TRACE_MSG("%s.calc_scalar(%ld, %ld, %ld, %ld, %ld)", 
                                              eg->get_name().c_str(), t, n, x, y, z);
                                        
                                    // Evaluate the reference scalar code.
                                    eg->calc_scalar(context, t, n, x, y, z);
                                }
                            }
            }
        } // iterations.
    }


    // Eval equation group(s) over grid(s) using optimized code.
    void StencilEqs::calc_rank_opt(StencilContext& context)
    {
        init(context);
        idx_t begin_dt = context.begin_dt;
        idx_t end_dt = begin_dt + context.dt;
        idx_t step_dt = context.rt;
        TRACE_MSG("calc_rank_opt(%ld..%ld by %ld)", begin_dt, end_dt-1, step_dt);

        // Problem begin points.
        idx_t begin_dn = context.begin_bbn;
        idx_t begin_dx = context.begin_bbx;
        idx_t begin_dy = context.begin_bby;
        idx_t begin_dz = context.begin_bbz;
    
        // Problem end-points.
        idx_t end_dn = context.end_bbn;
        idx_t end_dx = context.end_bbx;
        idx_t end_dy = context.end_bby;
        idx_t end_dz = context.end_bbz;

        // Steps are based on region sizes.
        idx_t step_dn = context.rn;
        idx_t step_dx = context.rx;
        idx_t step_dy = context.ry;
        idx_t step_dz = context.rz;

        // Groups in rank loops are set to smallest size.
        const idx_t group_size_dn = 1;
        const idx_t group_size_dx = 1;
        const idx_t group_size_dy = 1;
        const idx_t group_size_dz = 1;

        // Determine spatial skewing angles for temporal wavefronts based on the
        // halos.  This assumes the smallest granularity of calculation is
        // CPTS_* in each dim.
        // We only need non-zero angles if the region size is less than the rank size,
        // i.e., if the region covers the whole rank in a given dimension, no wave-front
        // is needed in thar dim.
        // TODO: make this grid-specific.
        context.angle_n = (context.rn < context.len_bbn) ? ROUND_UP(context.hn, CPTS_N) : 0;
        context.angle_x = (context.rx < context.len_bbx) ? ROUND_UP(context.hx, CPTS_X) : 0;
        context.angle_y = (context.ry < context.len_bby) ? ROUND_UP(context.hy, CPTS_Y) : 0;
        context.angle_z = (context.rz < context.len_bbz) ? ROUND_UP(context.hz, CPTS_Z) : 0;
        TRACE_MSG("wavefront angles: %ld, %ld, %ld, %ld",
                  context.angle_n, context.angle_x, context.angle_y, context.angle_z);
    
        // Extend end points for overlapping regions due to wavefront angle.
        // For each subsequent time step in a region, the spatial location of
        // each block evaluation is shifted by the angle for each stencil. So,
        // the total shift in a region is the angle * num stencils * num
        // timesteps. Thus, the number of overlapping regions is ceil(total
        // shift / region size).  This assumes stencils are inter-dependent.
        // TODO: calculate stencil inter-dependency in the foldBuilder for each
        // dimension.
        idx_t nshifts = (idx_t(eqGroups.size()) * context.rt) - 1;
        end_dn += context.angle_n * nshifts;
        end_dx += context.angle_x * nshifts;
        end_dy += context.angle_y * nshifts;
        end_dz += context.angle_z * nshifts;
        TRACE_MSG("extended domain after wavefront adjustment: %ld..%ld, %ld..%ld, %ld..%ld, %ld..%ld, %ld..%ld", 
                  begin_dt, end_dt-1,
                  begin_dn, end_dn-1,
                  begin_dx, end_dx-1,
                  begin_dy, end_dy-1,
                  begin_dz, end_dz-1);

        // Number of iterations to get from begin_dt to (but not including) end_dt,
        // stepping by step_dt.
        const idx_t num_dt = ((end_dt - begin_dt) + (step_dt - 1)) / step_dt;
        for (idx_t index_dt = 0; index_dt < num_dt; index_dt++)
        {
            // This value of index_dt covers dt from start_dt to stop_dt-1.
            const idx_t start_dt = begin_dt + (index_dt * step_dt);
            const idx_t stop_dt = min(start_dt + step_dt, end_dt);

            // FIXME: halo exchange with conditional equations is broken.
            
            // If doing only one time step in a region (default), loop through equations here,
            // and do only one equation group at a time in calc_region().
            if (step_dt == 1) {

                for (auto eqGroup : eqGroups) {

                    // Halo+shadow exchange for grid(s) updated by this equation.
                    eqGroup->exchange_halos(context, start_dt, stop_dt);

                    // Eval this stencil in calc_region().
                    EqGroupSet eqGroup_set;
                    eqGroup_set.insert(eqGroup);

                    // Include automatically-generated loop code that calls calc_region() for each region.
#include "stencil_rank_loops.hpp"
                }
            }

            // If doing more than one time step in a region (temporal wave-front),
            // must do all equations in calc_region().
            // TODO: allow doing all equations in region even with one time step for testing.
            else {

                EqGroupSet eqGroup_set;
                for (auto eqGroup : eqGroups) {

                    // Halo+shadow exchange for grid(s) updated by this equation.
                    eqGroup->exchange_halos(context, start_dt, stop_dt);
                    
                    // Make set of all equations.
                    eqGroup_set.insert(eqGroup);
                }
            
                // Include automatically-generated loop code that calls calc_region() for each region.
#include "stencil_rank_loops.hpp"
            }

        }
    }

    // Calculate results within a region.
    // Each region is typically computed in a separate OpenMP 'for' region.
    // In it, we loop over the time steps and the stencil
    // equations and evaluate the blocks in the region.
    void StencilEqs::
    calc_region(StencilContext& context, idx_t start_dt, idx_t stop_dt,
                EqGroupSet& eqGroup_set,
                idx_t start_dn, idx_t start_dx, idx_t start_dy, idx_t start_dz,
                idx_t stop_dn, idx_t stop_dx, idx_t stop_dy, idx_t stop_dz)
    {
        TRACE_MSG("calc_region(%ld..%ld, %ld..%ld, %ld..%ld, %ld..%ld, %ld..%ld)", 
                  start_dt, stop_dt-1,
                  start_dn, stop_dn-1,
                  start_dx, stop_dx-1,
                  start_dy, stop_dy-1,
                  start_dz, stop_dz-1);

        // Steps within a region are based on block sizes.
        const idx_t step_rt = context.bt;
        const idx_t step_rn = context.bn;
        const idx_t step_rx = context.bx;
        const idx_t step_ry = context.by;
        const idx_t step_rz = context.bz;

        // Groups in region loops are based on group sizes.
        const idx_t group_size_rn = context.gn;
        const idx_t group_size_rx = context.gx;
        const idx_t group_size_ry = context.gy;
        const idx_t group_size_rz = context.gz;

        // Not yet supporting temporal blocking.
        if (step_rt != 1) {
            cerr << "Error: temporal blocking not yet supported." << endl;
            assert(step_rt == 1);
            exit_yask(1);                // in case assert() is not active.
        }

        // Number of iterations to get from start_dt to (but not including) stop_dt,
        // stepping by step_rt.
        const idx_t num_rt = ((stop_dt - start_dt) + (step_rt - 1)) / step_rt;
    
        // Step through time steps in this region.
        for (idx_t index_rt = 0; index_rt < num_rt; index_rt++) {
        
            // This value of index_rt covers rt from start_rt to stop_rt-1.
            const idx_t start_rt = start_dt + (index_rt * step_rt);
            const idx_t stop_rt = min (start_rt + step_rt, stop_dt);

            // TODO: remove this when temporal blocking is implemented.
            assert(stop_rt == start_rt + 1);
            const idx_t rt = start_rt; // only one time value needed for block.

            // equations to evaluate at this time step.
            for (auto eg : eqGroups) {
                if (eqGroup_set.count(eg)) {

                    // Actual region boundaries must stay within BB for this eq group.
                    idx_t begin_rn = max<idx_t>(start_dn, eg->begin_bbn);
                    idx_t end_rn = min<idx_t>(stop_dn, eg->end_bbn);
                    idx_t begin_rx = max<idx_t>(start_dx, eg->begin_bbx);
                    idx_t end_rx = min<idx_t>(stop_dx, eg->end_bbx);
                    idx_t begin_ry = max<idx_t>(start_dy, eg->begin_bby);
                    idx_t end_ry = min<idx_t>(stop_dy, eg->end_bby);
                    idx_t begin_rz = max<idx_t>(start_dz, eg->begin_bbz);
                    idx_t end_rz = min<idx_t>(stop_dz, eg->end_bbz);

                    // Only need to loop through the region if any of its blocks are
                    // at least partly inside the domain. For overlapping regions,
                    // they may start outside the domain but enter the domain as
                    // time progresses and their boundaries shift. So, we don't want
                    // to return if this condition isn't met.
                    if (end_rn > begin_rn &&
                        end_rx > begin_rx &&
                        end_ry > begin_ry &&
                        end_rz > begin_rz) {

                        // Set number of threads for a region.
                        context.set_region_threads();

                        // Include automatically-generated loop code that calls
                        // calc_block() for each block in this region.  Loops
                        // through n from begin_rn to end_rn-1; similar for x, y,
                        // and z.  This code typically contains OpenMP loop(s).
#include "stencil_region_loops.hpp"

                        // Reset threads back to max.
                        context.set_max_threads();
                    }
            
                    // Shift spatial region boundaries for next iteration to
                    // implement temporal wavefront.  We only shift backward, so
                    // region loops must increment. They may do so in any order.
                    start_dn -= context.angle_n;
                    stop_dn -= context.angle_n;
                    start_dx -= context.angle_x;
                    stop_dx -= context.angle_x;
                    start_dy -= context.angle_y;
                    stop_dy -= context.angle_y;
                    start_dz -= context.angle_z;
                    stop_dz -= context.angle_z;

                }            
            } // stencil equations.
        } // time.
    }

    // Initialize some data structures.
    // Must be called after the context grids are allocated.
    void StencilEqs::init(StencilContext& context,
                          idx_t* sum_points,
                          idx_t* sum_fpops) {
        for (auto eg : eqGroups)
            eg->init(context);

        if (context.bb_valid == true) return;
        find_bounding_boxes(context);

        idx_t npoints = 0, nfpops = 0;
        ostream& os = *(context.ostr);
        os << "Num stencil equation-groups: " << eqGroups.size() << endl;
        for (auto eg : eqGroups) {
            idx_t updates1 = eg->get_scalar_points_updated();
            idx_t updates_domain = updates1 * eg->bb_size;
            idx_t fpops1 = eg->get_scalar_fp_ops();
            idx_t fpops_domain = fpops1 * eg->bb_size;
            npoints += updates_domain;
            nfpops += fpops_domain;
            os << "Stats for equation-group '" << eg->get_name() << "':\n" <<
                " sub-domain-size:            " <<
                eg->len_bbn << '*' << eg->len_bbx << '*' << eg->len_bby << '*' << eg->len_bbz << endl <<
                " points-in-sub-domain:       " << printWithPow10Multiplier(eg->bb_size) << endl <<
                " grid-updates-per-point:     " << updates1 << endl <<
                " grid-updates-in-sub-domain: " << printWithPow10Multiplier(updates_domain) << endl <<
                " est-FP-ops-per-point:       " << fpops1 << endl <<
                " est-FP-ops-in-sub-domain:   " << printWithPow10Multiplier(fpops_domain) << endl;
        }
        if (sum_points)
            *sum_points = npoints;
        if (sum_fpops)
            *sum_fpops = nfpops;
    }
    
    // Set the bounding-box vars for all eq groups.
    void StencilEqs::find_bounding_boxes(StencilContext& context) {
        if (context.bb_valid == true) return;

        // Init overall BB.
        // Init min vars w/max val and vice-versa.
        context.begin_bbn = idx_max; context.end_bbn = idx_min;
        context.begin_bbx = idx_max; context.end_bbx = idx_min;
        context.begin_bby = idx_max; context.end_bby = idx_min;
        context.begin_bbz = idx_max; context.end_bbz = idx_min;
        context.bb_size = 0;
        
        // Find BB for each eq group and update context.
        for (auto eg : eqGroups) {
            eg->find_bounding_box(context);

            context.begin_bbn = min(context.begin_bbn, eg->begin_bbn);
            context.begin_bbx = min(context.begin_bbx, eg->begin_bbx);
            context.begin_bby = min(context.begin_bby, eg->begin_bby);
            context.begin_bbz = min(context.begin_bbz, eg->begin_bbz);
            context.end_bbn = max(context.end_bbn, eg->end_bbn);
            context.end_bbx = max(context.end_bbx, eg->end_bbx);
            context.end_bby = max(context.end_bby, eg->end_bby);
            context.end_bbz = max(context.end_bbz, eg->end_bbz);
            context.bb_size += eg->bb_size;
        }

        context.len_bbn = context.end_bbn - context.begin_bbn;
        context.len_bbx = context.end_bbx - context.begin_bbx;
        context.len_bby = context.end_bby - context.begin_bby;
        context.len_bbz = context.end_bbz - context.begin_bbz;
        context.bb_valid = true;

        // Special case: if region sizes are equal to domain size (defaul
        // setting), change them to the BB size.
        if (context.rn == context.dn) context.rn = context.len_bbn;
        if (context.rx == context.dx) context.rx = context.len_bbx;
        if (context.ry == context.dy) context.ry = context.len_bby;
        if (context.rz == context.dz) context.rz = context.len_bbz;
    }

    // Set the bounding-box vars for this eq group.
    void EqGroupBase::find_bounding_box(StencilContext& context) {
        if (bb_valid) return;

        // Init min vars w/max val and vice-versa.
        idx_t minn = idx_max, maxn = idx_min;
        idx_t minx = idx_max, maxx = idx_min;
        idx_t miny = idx_max, maxy = idx_min;
        idx_t minz = idx_max, maxz = idx_min;
        idx_t npts = 0;
        
        // Assume bounding-box is same for all time steps.
        // TODO: consider adding time to domain.
        idx_t t = 0;

        // Loop through 4D space.
        // Find the min and max valid points in this space.
        // FIXME: use global indices for >1 rank.
#pragma omp parallel for collapse(4)            \
    reduction(min:minn,minx,miny,minz)          \
    reduction(max:maxn,maxx,maxy,maxz)          \
    reduction(+:npts)
        for (idx_t n = 0; n < context.dn; n++)
            for(idx_t x = 0; x < context.dx; x++)
                for(idx_t y = 0; y < context.dy; y++)
                    for(idx_t z = 0; z < context.dz; z++) {

                        // Update only if point in domain for this eq group.
                        if (is_in_valid_domain(context, t, n, x, y, z)) {
                            minn = min(minn, n);
                            maxn = max(maxn, n);
                            minx = min(minx, x);
                            maxx = max(maxx, x);
                            miny = min(miny, y);
                            maxy = max(maxy, y);
                            minz = min(minz, z);
                            maxz = max(maxz, z);
                            npts++;
                        }
                    }

        // Set begin vars to min indices and end vars to one beyond max indices.
        if (npts) {
            begin_bbn = minn;
            end_bbn = maxn + 1;
            begin_bbx = minx;
            end_bbx = maxx + 1;
            begin_bby = miny;
            end_bby = maxy + 1;
            begin_bbz = minz;
            end_bbz = maxz + 1;
        } else {
            begin_bbn = end_bbn = len_bbn = 0;
            begin_bbx = end_bbx = len_bbx = 0;
            begin_bby = end_bby = len_bby = 0;
            begin_bbz = end_bbz = len_bbz = 0;
        }
        len_bbn = end_bbn - begin_bbn;
        len_bbx = end_bbx - begin_bbx;
        len_bby = end_bby - begin_bby;
        len_bbz = end_bbz - begin_bbz;
        bb_size = npts;

        // Only supporting solid rectangles at this time.
        idx_t r_size = len_bbn * len_bbx * len_bby * len_bbz;
        if (r_size != bb_size) {
            cerr << "error: domain for equation-group '" << get_name() << "' contains " <<
                bb_size << " points, but " << r_size << " were expected for a rectangular solid. " <<
                "Non-rectangular domains are not supported at this time." << endl;
            exit_yask(1);
        }

        // Only supporting full-cluster BBs at this time.
        // TODO: handle partial clusters.
        if (len_bbn % CLEN_N ||
            len_bbx % CLEN_X ||
            len_bby % CLEN_Y ||
            len_bbz % CLEN_Z) {
            cerr << "error: each domain length must be a multiple of the cluster size." << endl;
            exit_yask(1);
        }

        bb_valid = true;
    }
    
    // Exchange halo and shadow data for the given time.
    void EqGroupBase::exchange_halos(StencilContext& context, idx_t start_dt, idx_t stop_dt)
    {
        TRACE_MSG("exchange_halos(%ld..%ld)", start_dt, stop_dt);

        // List of grids updated by this equation.
        // These are the grids that need exchanges.
        // FIXME: does not work w/conditions.
        auto eqGridPtrs = get_eq_grid_ptrs();

        // TODO: clean up all the shadow code. Either do something useful with it, or get rid of it.
        
        // Time to copy to shadow?
        if (context.shadow_out_freq && abs(start_dt - context.begin_dt) % context.shadow_out_freq == 0) {
            TRACE_MSG("copying to shadows at time %ld", start_dt);

            double start_time = getTimeInSecs();
            idx_t t = start_dt;

            for (size_t gi = 0; gi < eqGridPtrs.size(); gi++) {

                // Get pointer to generic grid and derived type.
                // TODO: Make this more general.
                auto gp = eqGridPtrs[gi];
#if USING_DIM_N
                auto gpd = dynamic_cast<Grid_TNXYZ*>(gp);
#else
                auto gpd = dynamic_cast<Grid_TXYZ*>(gp);
#endif
                assert(gpd);

                // Get pointer to shadow.
                auto sp = context.shadowGrids[gp];
                assert(sp);

                // Copy from grid to shadow.
                // Shadows are *inside* the halo regions, e.g., indices 0..dx for dimension x.
                for (idx_t n = 0; n < context.dn; n++) {

#pragma omp parallel for
                    for(idx_t x = 0; x < context.dx; x++) {

                        CREW_FOR_LOOP
                            for(idx_t y = 0; y < context.dy; y++) {

#pragma simd
#pragma vector nontemporal
                                for(idx_t z = 0; z < context.dz; z++) {

                                    // Copy one element.
                                    real_t val = gpd->readElem(t, ARG_N(nv)
                                                               x, y, z, __LINE__);
                                    (*sp)(n, x, y, z) = val;
                                }
                            }
                    }
                }
            }            

            // In a real application, some processing on the shadow
            // grid would be done here.
            double end_time = getTimeInSecs();
            context.shadow_time += end_time - start_time;
        }

        // Time to copy from shadow?
        if (context.shadow_in_freq && abs(start_dt - context.begin_dt) % context.shadow_in_freq == 0) {
            TRACE_MSG("copying from shadows at time %ld", start_dt);

            double start_time = getTimeInSecs();
            idx_t t = start_dt;

            for (size_t gi = 0; gi < eqGridPtrs.size(); gi++) {

                // Get pointer to generic grid and derived type.
                // TODO: Make this more general.
                auto gp = eqGridPtrs[gi];
#if USING_DIM_N
                auto gpd = dynamic_cast<Grid_TNXYZ*>(gp);
#else
                auto gpd = dynamic_cast<Grid_TXYZ*>(gp);
#endif
                assert(gpd);

                // Get pointer to shadow.
                auto sp = context.shadowGrids[gp];
                assert(sp);

                // Copy from shadow to grid.
                for (idx_t n = 0; n < context.dn; n++) {

#pragma omp parallel for
                    for(idx_t x = 0; x < context.dx; x++) {

                        CREW_FOR_LOOP
                            for(idx_t y = 0; y < context.dy; y++) {
                            
#pragma simd
#pragma vector nontemporal
                                for(idx_t z = 0; z < context.dz; z++) {

                                    // Copy one element.
                                    real_t val = (*sp)(n, x, y, z);
                                    gpd->writeElem(val, t, ARG_N(nv)
                                                   x, y, z, __LINE__);
                                }
                            }
                    }
                }
            }            
            double end_time = getTimeInSecs();
            context.shadow_time += end_time - start_time;
        }

#ifdef USE_MPI
        double start_time = getTimeInSecs();

        // These vars control blocking within halo packing.
        // Currently, only zv has a loop in the calc_halo macros below.
        // Thus, step_{n,x,y}v must be 1.
        // TODO: make step_zv a parameter.
        const idx_t step_nv = 1;
        const idx_t step_xv = 1;
        const idx_t step_yv = 1;
        const idx_t step_zv = 4;

        // Groups in halo loops are set to smallest size.
        const idx_t group_size_nv = 1;
        const idx_t group_size_xv = 1;
        const idx_t group_size_yv = 1;
        const idx_t group_size_zv = 1;

        // TODO: put this loop inside visitNeighbors.
        for (size_t gi = 0; gi < eqGridPtrs.size(); gi++) {

            // Get pointer to generic grid and derived type.
            // TODO: Make this more general.
            auto gp = eqGridPtrs[gi];
#if USING_DIM_N
            auto gpd = dynamic_cast<Grid_TNXYZ*>(gp);
#else
            auto gpd = dynamic_cast<Grid_TXYZ*>(gp);
#endif
            assert(gpd);

            // Determine halo sizes to be exchanged for this grid;
            // context.h* contains the max value across all grids.  The grid
            // contains the halo+pad size actually allocated.
            // Since neither of these is exactly what we want, we use
            // the minimum of these values as a conservative value. TODO:
            // Store the actual halo needed in each grid and use this.
#if USING_DIM_N
            idx_t hn = min(context.hn, gpd->get_pn());
#else
            idx_t hn = 0;
#endif
            idx_t hx = min(context.hx, gpd->get_px());
            idx_t hy = min(context.hy, gpd->get_py());
            idx_t hz = min(context.hz, gpd->get_pz());
            
            // Array to store max number of request handles.
            MPI_Request reqs[MPIBufs::nBufDirs * MPIBufs::neighborhood_size];
            int nreqs = 0;

            // Pack data and initiate non-blocking send/receive to/from all neighbors.
            TRACE_MSG("rank %i: exchange_halos: packing data for grid '%s'...",
                      context.my_rank, gp->get_name().c_str());
            context.mpiBufs[gp].visitNeighbors
                (context,
                 [&](idx_t nn, idx_t nx, idx_t ny, idx_t nz,
                     int neighbor_rank,
                     Grid_NXYZ* sendBuf,
                     Grid_NXYZ* rcvBuf)
                 {
                     // Pack and send data if buffer exists.
                     if (sendBuf) {

                         // Set begin/end vars to indicate what part
                         // of main grid to read from.
                         // Init range to whole rank size (inside halos).
                         idx_t begin_n = 0;
                         idx_t begin_x = 0;
                         idx_t begin_y = 0;
                         idx_t begin_z = 0;
                         idx_t end_n = context.dn;
                         idx_t end_x = context.dx;
                         idx_t end_y = context.dy;
                         idx_t end_z = context.dz;

                         // Modify begin and/or end based on direction.
                         if (nn == idx_t(MPIBufs::rank_prev)) // neighbor is prev N.
                             end_n = hn; // read first halo-width only.
                         if (nn == idx_t(MPIBufs::rank_next)) // neighbor is next N.
                             begin_n = context.dn - hn; // read last halo-width only.
                         if (nx == idx_t(MPIBufs::rank_prev)) // neighbor is on left.
                             end_x = hx;
                         if (nx == idx_t(MPIBufs::rank_next)) // neighbor is on right.
                             begin_x = context.dx - hx;
                         if (ny == idx_t(MPIBufs::rank_prev)) // neighbor is in front.
                             end_y = hy;
                         if (ny == idx_t(MPIBufs::rank_next)) // neighbor is in back.
                             begin_y = context.dy - hy;
                         if (nz == idx_t(MPIBufs::rank_prev)) // neighbor is above.
                             end_z = hz;
                         if (nz == idx_t(MPIBufs::rank_next)) // neighbor is below.
                             begin_z = context.dz - hz;

                         // Divide indices by vector lengths.
                         // Begin/end vars shouldn't be negative, so '/' is ok.
                         idx_t begin_nv = begin_n / VLEN_N;
                         idx_t begin_xv = begin_x / VLEN_X;
                         idx_t begin_yv = begin_y / VLEN_Y;
                         idx_t begin_zv = begin_z / VLEN_Z;
                         idx_t end_nv = end_n / VLEN_N;
                         idx_t end_xv = end_x / VLEN_X;
                         idx_t end_yv = end_y / VLEN_Y;
                         idx_t end_zv = end_z / VLEN_Z;

                         // TODO: fix this when MPI + wave-front is enabled.
                         idx_t t = start_dt;
                         
                         // Define calc_halo() to copy a vector from main grid to sendBuf.
                         // Index sendBuf using index_* vars because they are zero-based.
#define calc_halo(context, t,                                           \
                  start_nv, start_xv, start_yv, start_zv,               \
                  stop_nv, stop_xv, stop_yv, stop_zv)  do {             \
                         idx_t nv = start_nv;                           \
                         idx_t xv = start_xv;                           \
                         idx_t yv = start_yv;                           \
                         idx_t izv = index_zv * step_zv;                \
                         for (idx_t zv = start_zv; zv < stop_zv; zv++) { \
                             real_vec_t hval = gpd->readVecNorm(t, ARG_N(nv) \
                                                                xv, yv, zv, __LINE__); \
                             sendBuf->writeVecNorm(hval, index_nv,      \
                                                   index_xv, index_yv, izv++, __LINE__); \
                         } } while(0)
                         
                         // Include auto-generated loops to invoke calc_halo() from
                         // begin_*v to end_*v;
#include "stencil_halo_loops.hpp"
#undef calc_halo

                         // Send filled buffer to neighbor.
                         const void* buf = (const void*)(sendBuf->getRawData());
                         MPI_Isend(buf, sendBuf->get_num_bytes(), MPI_BYTE,
                                   neighbor_rank, int(gi), context.comm, &reqs[nreqs++]);
                         
                     }

                     // Receive data from same neighbor if buffer exists.
                     if (rcvBuf) {
                         void* buf = (void*)(rcvBuf->getRawData());
                         MPI_Irecv(buf, rcvBuf->get_num_bytes(), MPI_BYTE,
                                   neighbor_rank, int(gi), context.comm, &reqs[nreqs++]);
                     }
                     
                 } );

            // Wait for all to complete.
            // TODO: process each buffer asynchronously immediately upon completion.
            TRACE_MSG("rank %i: exchange_halos: waiting for %i MPI request(s)...",
                      context.my_rank, nreqs);
            MPI_Waitall(nreqs, reqs, MPI_STATUS_IGNORE);
            TRACE_MSG("rank %i: exchange_halos: done waiting for %i MPI request(s).",
                      context.my_rank, nreqs);

            // Unpack received data from all neighbors.
            context.mpiBufs[gp].visitNeighbors
                (context,
                 [&](idx_t nn, idx_t nx, idx_t ny, idx_t nz,
                     int neighbor_rank,
                     Grid_NXYZ* sendBuf,
                     Grid_NXYZ* rcvBuf)
                 {
                     // Unpack data if buffer exists.
                     if (rcvBuf) {

                         // Set begin/end vars to indicate what part
                         // of main grid's halo to write to.
                         // Init range to whole rank size (inside halos).
                         idx_t begin_n = 0;
                         idx_t begin_x = 0;
                         idx_t begin_y = 0;
                         idx_t begin_z = 0;
                         idx_t end_n = context.dn;
                         idx_t end_x = context.dx;
                         idx_t end_y = context.dy;
                         idx_t end_z = context.dz;
                         
                         // Modify begin and/or end based on direction.
                         if (nn == idx_t(MPIBufs::rank_prev)) { // neighbor is prev N.
                             begin_n = -hn; // begin at outside of halo.
                             end_n = 0;     // end at inside of halo.
                         }
                         if (nn == idx_t(MPIBufs::rank_next)) { // neighbor is next N.
                             begin_n = context.dn; // begin at inside of halo.
                             end_n = context.dn + hn; // end of outside of halo.
                         }
                         if (nx == idx_t(MPIBufs::rank_prev)) { // neighbor is on left.
                             begin_x = -hx;
                             end_x = 0;
                         }
                         if (nx == idx_t(MPIBufs::rank_next)) { // neighbor is on right.
                             begin_x = context.dx;
                             end_x = context.dx + hx;
                         }
                         if (ny == idx_t(MPIBufs::rank_prev)) { // neighbor is in front.
                             begin_y = -hy;
                             end_y = 0;
                         }
                         if (ny == idx_t(MPIBufs::rank_next)) { // neighbor is in back.
                             begin_y = context.dy;
                             end_y = context.dy + hy;
                         }
                         if (nz == idx_t(MPIBufs::rank_prev)) { // neighbor is above.
                             begin_z = -hz;
                             end_z = 0;
                         }
                         if (nz == idx_t(MPIBufs::rank_next)) { // neighbor is below.
                             begin_z = context.dz;
                             end_z = context.dz + hz;
                         }

                         // Divide indices by vector lengths.
                         // Begin/end vars shouldn't be negative, so '/' is ok.
                         idx_t begin_nv = begin_n / VLEN_N;
                         idx_t begin_xv = begin_x / VLEN_X;
                         idx_t begin_yv = begin_y / VLEN_Y;
                         idx_t begin_zv = begin_z / VLEN_Z;
                         idx_t end_nv = end_n / VLEN_N;
                         idx_t end_xv = end_x / VLEN_X;
                         idx_t end_yv = end_y / VLEN_Y;
                         idx_t end_zv = end_z / VLEN_Z;

                         // TODO: fix this when MPI + wave-front is enabled.
                         idx_t t = start_dt;
                         
                         // Define calc_halo to copy data from rcvBuf into main grid.
#define calc_halo(context, t,                                           \
                  start_nv, start_xv, start_yv, start_zv,               \
                  stop_nv, stop_xv, stop_yv, stop_zv)  do {             \
                             idx_t nv = start_nv;                       \
                             idx_t xv = start_xv;                       \
                             idx_t yv = start_yv;                       \
                             idx_t izv = index_zv * step_zv;            \
                             for (idx_t zv = start_zv; zv < stop_zv; zv++) { \
                                 real_vec_t hval =                      \
                                     rcvBuf->readVecNorm(index_nv,      \
                                                         index_xv, index_yv, izv++, __LINE__); \
                                 gpd->writeVecNorm(hval, t, ARG_N(nv)   \
                                                   xv, yv, zv, __LINE__); \
                     } } while(0)

                         // Include auto-generated loops to invoke calc_halo() from
                         // begin_*v to end_*v;
#include "stencil_halo_loops.hpp"
#undef calc_halo
                     }
                 } );

        } // grids.

        double end_time = getTimeInSecs();
        context.mpi_time += end_time - start_time;
#endif
    }
                         
            
    ///// StencilContext functions:

    // Init MPI-related vars.
    void StencilContext::setupMPI(bool findLocation) {

        // Determine my position in 4D.
        if (findLocation) {
            Layout_4321 rank_layout(nrn, nrx, nry, nrz);
            rank_layout.unlayout((idx_t)my_rank, rin, rix, riy, riz);
        }
        *ostr << "Logical coordinates of rank " << my_rank << ": " <<
            rin << ", " << rix << ", " << riy << ", " << riz << endl;

        // A table of coordinates for everyone.
        const int num_dims = 4;
        idx_t coords[num_ranks][num_dims];
        coords[my_rank][0] = rin;
        coords[my_rank][1] = rix;
        coords[my_rank][2] = riy;
        coords[my_rank][3] = riz;

#ifdef USE_MPI
        // Exchange coordinate info between all ranks.
        for (int rn = 0; rn < num_ranks; rn++) {
            MPI_Bcast(&coords[rn][0], num_dims, MPI_INTEGER8,
                      rn, comm);
        }
#endif
        
        // Determine who my neighbors are.
        int num_neighbors = 0;
        for (int rn = 0; rn < num_ranks; rn++) {

            // Get coordinates of rn.
            idx_t rnn = coords[rn][0];
            idx_t rnx = coords[rn][1];
            idx_t rny = coords[rn][2];
            idx_t rnz = coords[rn][3];

            // Distance from me: prev => -1, self => 0, next => +1.
            idx_t rdn = rnn - rin;
            idx_t rdx = rnx - rix;
            idx_t rdy = rny - riy;
            idx_t rdz = rnz - riz;

            // Manhattan distance.
            int mdist = abs(rdn) + abs(rdx) + abs(rdy) + abs(rdz);
            
            // Myself.
            if (rn == my_rank) {
                if (mdist != 0) {
                    cerr << "internal error: distance to own rank == " << mdist << endl;
                    exit_yask(1);
                }
                continue; // nothing else to do for self.
            }

            // Someone else.
            else {
                if (mdist == 0) {
                    cerr << "error: distance to rank " << rn << " == " << mdist << endl;
                    exit_yask(1);
                }
            }
            
            // Rank rn is my neighbor if its distance <= 1 in every dim.
            if (abs(rdn) > 1 || abs(rdx) > 1 || abs(rdy) > 1 || abs(rdz) > 1)
                continue;

            // Check against max dist needed.
            // TODO: determine max dist automatically from stencil equations.
#ifndef MAX_EXCH_DIST
#define MAX_EXCH_DIST 4
#endif
            if (mdist > MAX_EXCH_DIST)
                continue;

            num_neighbors++;
            *ostr << "Neighbor #" << num_neighbors << " at " <<
                rnn << ", " << rnx << ", " << rny << ", " << rnz <<
                " is rank " << rn << endl;
                    
            // Size of buffer in each direction:
            // if dist to neighbor is zero (i.e., is self), use full size,
            // otherwise, use halo size.
            // TODO: use per-grid halo size instead of global max.
            idx_t rsn = (rdn == 0) ? dn : hn;
            idx_t rsx = (rdx == 0) ? dx : hx;
            idx_t rsy = (rdy == 0) ? dy : hy;
            idx_t rsz = (rdz == 0) ? dz : hz;

            // TODO: only alloc buffers in directions actually needed, e.g.,
            // many simple stencils don't need diagonals.
                    
            // Is buffer needed?
            if (rsn * rsx * rsy * rsz == 0) {
                *ostr << "No halo exchange needed between ranks " << my_rank <<
                    " and " << rn << '.' << endl;
                continue;
            }

            // Add one to -1..+1 dist to get 0..2 range for my_neighbors indices.
            rdn++; rdx++; rdy++; rdz++;

            // Save rank of this neighbor.
            my_neighbors[rdn][rdx][rdy][rdz] = rn;
                    
            // Alloc MPI buffers between rn and me.
            // Need send and receive for each updated grid.
            for (auto gp : eqGridPtrs) {
                for (int bd = 0; bd < MPIBufs::nBufDirs; bd++) {
                    ostringstream oss;
                    oss << gp->get_name();
                    if (bd == MPIBufs::bufSend)
                        oss << "_send_halo_from_" << my_rank << "_to_" << rn;
                    else
                        oss << "_get_halo_by_" << my_rank << "_from_" << rn;

                    mpiBufs[gp].allocBuf(bd, rdn, rdx, rdy, rdz,
                                         rsn, rsx, rsy, rsz,
                                         oss.str(), *ostr);
                }
            }
        }
    }

    // Get total size.
    idx_t StencilContext::get_num_bytes() {
        idx_t nbytes = 0;

        // Grids.
        for (auto gp : gridPtrs)
            nbytes += gp->get_num_bytes();

        // Params.
        for (auto pp : paramPtrs)
            nbytes += pp->get_num_bytes();

        // MPI buffers.
        for (auto gp : eqGridPtrs) {
            mpiBufs[gp].visitNeighbors
                (*this,
                 [&](idx_t nn, idx_t nx, idx_t ny, idx_t nz,
                     int rank,
                     Grid_NXYZ* sendBuf,
                     Grid_NXYZ* rcvBuf)
                 {
                     if (sendBuf)
                         nbytes += sendBuf->get_num_bytes();
                     if (rcvBuf)
                         nbytes += rcvBuf->get_num_bytes();
                 } );
        }

        // Shadow buffers.
        for (auto gp : eqGridPtrs) {
            if (shadowGrids.count(gp) && shadowGrids[gp])
                nbytes += shadowGrids[gp]->get_num_bytes();
        }
        
        return nbytes;
    }

    // Alloc shadow grids.
    void StencilContext::allocShadowGrids() {
        for (auto gp : eqGridPtrs) {
            if (shadowGrids[gp])
                delete shadowGrids[gp];
            shadowGrids[gp] = new RealGrid_NXYZ(dn, dx, dy, dz,
                                                GRID_ALIGNMENT);
            shadowGrids[gp]->print_info(string("shadow-") + gp->get_name(), *ostr);
        }        
    }

    // Allocate grids, params, and MPI bufs.
    // Returns num bytes.
    idx_t StencilContext::allocAll(bool findRankLocation)
    {
        *ostr << "Allocating grids..." << endl;
        allocGrids();
        *ostr << "Allocating parameters..." << endl;
        allocParams();
#ifdef USE_MPI
        *ostr << "Allocating MPI buffers..." << endl;
        setupMPI(findRankLocation);
#endif
        if (shadow_in_freq || shadow_out_freq) {
            *ostr << "Allocating shadow grids..." << endl;
            allocShadowGrids();
        }

        const idx_t num_eqGrids = eqGridPtrs.size();
        *ostr << "Num grids: " << gridPtrs.size() << endl;
        *ostr << "Num grids to be updated: " << num_eqGrids << endl;

        idx_t nbytes = get_num_bytes();
        *ostr << "Total allocation in this rank (bytes): " <<
            printWithPow2Multiplier(nbytes) << endl;
        return nbytes;
    }

    // Init all grids & params by calling initFn.
    void StencilContext::initValues(function<void (RealVecGridBase* gp, 
                                                   real_t seed)> realVecInitFn,
                                    function<void (RealGrid* gp,
                                                   real_t seed)> realInitFn)
    {
        real_t v = 0.1;
        *ostr << "Initializing grids..." << endl;
        for (auto gp : gridPtrs) {
            realVecInitFn(gp, v);
            v += 0.01;
        }
        if (shadowGrids.size()) {
            *ostr << "Initializing shadow grids..." << endl;
            for (auto gp : eqGridPtrs) {
                if (shadowGrids.count(gp) && shadowGrids[gp]) {
                    realInitFn(shadowGrids[gp], v);
                    v += 0.01;
                }
            }
        }
        if (paramPtrs.size()) {
            *ostr << "Initializing parameters..." << endl;
            for (auto pp : paramPtrs) {
                realInitFn(pp, v);
                v += 0.01;
            }
        }
    }

    // Compare grids in contexts.
    // Return number of mis-compares.
    idx_t StencilContext::compare(const StencilContext& ref) const {

        *ostr << "Comparing grid(s) in '" << name << "' to '" << ref.name << "'..." << endl;
        if (gridPtrs.size() != ref.gridPtrs.size()) {
            cerr << "** number of grids not equal." << endl;
            return 1;
        }
        idx_t errs = 0;
        for (size_t gi = 0; gi < gridPtrs.size(); gi++) {
            *ostr << "Grid '" << ref.gridPtrs[gi]->get_name() << "'..." << endl;
            errs += gridPtrs[gi]->compare(*ref.gridPtrs[gi]);
        }

        *ostr << "Comparing parameter(s) in '" << name << "' to '" << ref.name << "'..." << endl;
        if (paramPtrs.size() != ref.paramPtrs.size()) {
            cerr << "** number of params not equal." << endl;
            return 1;
        }
        for (size_t pi = 0; pi < paramPtrs.size(); pi++) {
            errs += paramPtrs[pi]->compare(ref.paramPtrs[pi], EPSILON);
        }

        return errs;
    }
    
    // Apply a function to each neighbor rank and/or buffer.
    void MPIBufs::visitNeighbors(StencilContext& context,
                                 std::function<void (idx_t nn, idx_t nx, idx_t ny, idx_t nz,
                                                     int rank,
                                                     Grid_NXYZ* sendBuf,
                                                     Grid_NXYZ* rcvBuf)> visitor)
    {
        for (idx_t nn = 0; nn < num_neighbors; nn++)
            for (idx_t nx = 0; nx < num_neighbors; nx++)
                for (idx_t ny = 0; ny < num_neighbors; ny++)
                    for (idx_t nz = 0; nz < num_neighbors; nz++)
                        visitor(nn, nx, ny, nz,
                                context.my_neighbors[nn][nx][ny][nz],
                                bufs[0][nn][nx][ny][nz],
                                bufs[1][nn][nx][ny][nz]);
    }

    // Allocate new buffer in given direction and size.
    Grid_NXYZ* MPIBufs::allocBuf(int bd,
                                 idx_t nn, idx_t nx, idx_t ny, idx_t nz,
                                 idx_t dn, idx_t dx, idx_t dy, idx_t dz,
                                 const std::string& name,
                                 std::ostream& os)
    {
        auto gp = (*this)(bd, nn, nx, ny, nz);
        assert(gp == NULL); // not already allocated.
        gp = new Grid_NXYZ(dn, dx, dy, dz, 0, 0, 0, 0, name, true, os);
        assert(gp);
        bufs[bd][nn][nx][ny][nz] = gp;
        return gp;
    }
    
}
