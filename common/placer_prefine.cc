/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019  David Shah <david@symbioticeda.com>
 *
 *  Parallelised SA-based placement refiner
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "placer1.h"
#include "placer_prefine.h"
#include <algorithm>
#include <atomic>
#include <boost/optional.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <ostream>
#include <queue>
#include <set>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>
#include "log.h"
#include "place_common.h"
#include "timing.h"
#include "util.h"

namespace std {
template <> struct hash<std::pair<NEXTPNR_NAMESPACE_PREFIX IdString, std::size_t>>
{
    std::size_t operator()(const std::pair<NEXTPNR_NAMESPACE_PREFIX IdString, std::size_t> &idp) const noexcept
    {
        std::size_t seed = 0;
        boost::hash_combine(seed, hash<NEXTPNR_NAMESPACE_PREFIX IdString>()(idp.first));
        boost::hash_combine(seed, hash<std::size_t>()(idp.second));
        return seed;
    }
};
} // namespace std

NEXTPNR_NAMESPACE_BEGIN

class ParallelRefinementPlacer
{
  private:
    struct BoundingBox
    {
        int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        bool is_inside_inc(int x, int y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
        bool touches_bounds(int x, int y) const { return x == x0 || x == x1 || y == y0 || y == y1; }
        wirelen_t hpwl() const { return wirelen_t((x1 - x0) + (y1 - y0)); }
    };

  public:
    ParallelRefinementPlacer(Context *ctx, Placer1Cfg cfg) : ctx(ctx), cfg(cfg)
    {
        int num_bel_types = 0;
        for (auto bel : ctx->getBels()) {
            IdString type = ctx->getBelType(bel);
            if (bel_types.find(type) == bel_types.end()) {
                bel_types[type] = std::tuple<int, int>(num_bel_types++, 1);
            } else {
                std::get<1>(bel_types.at(type))++;
            }
        }
        for (auto bel : ctx->getBels()) {
            Loc loc = ctx->getBelLocation(bel);
            IdString type = ctx->getBelType(bel);
            int type_idx = std::get<0>(bel_types.at(type));
            int type_cnt = std::get<1>(bel_types.at(type));
            if (type_cnt < cfg.minBelsForGridPick)
                loc.x = loc.y = 0;
            if (int(fast_bels.size()) < type_idx + 1)
                fast_bels.resize(type_idx + 1);
            if (int(fast_bels.at(type_idx).size()) < (loc.x + 1))
                fast_bels.at(type_idx).resize(loc.x + 1);
            if (int(fast_bels.at(type_idx).at(loc.x).size()) < (loc.y + 1))
                fast_bels.at(type_idx).at(loc.x).resize(loc.y + 1);
            max_x = std::max(max_x, loc.x);
            max_y = std::max(max_y, loc.y);
            fast_bels.at(type_idx).at(loc.x).at(loc.y).push_back(bel);
        }
        diameter = std::max(max_x, max_y) + 1;

        net_bounds.resize(ctx->nets.size());
        net_arc_tcost.resize(ctx->nets.size());
        moveChange.already_bounds_changed.resize(ctx->nets.size());
        moveChange.already_changed_arcs.resize(ctx->nets.size());
        old_udata.reserve(ctx->nets.size());
        net_by_udata.reserve(ctx->nets.size());
        decltype(NetInfo::udata) n = 0;
        for (auto &net : ctx->nets) {
            old_udata.emplace_back(net.second->udata);
            net_arc_tcost.at(n).resize(net.second->users.size());
            moveChange.already_changed_arcs.at(n).resize(net.second->users.size());
            net.second->udata = n++;
            net_by_udata.push_back(net.second.get());
        }
        for (auto &region : sorted(ctx->region)) {
            Region *r = region.second;
            BoundingBox bb;
            if (r->constr_bels) {
                bb.x0 = std::numeric_limits<int>::max();
                bb.x1 = std::numeric_limits<int>::min();
                bb.y0 = std::numeric_limits<int>::max();
                bb.y1 = std::numeric_limits<int>::min();
                for (auto bel : r->bels) {
                    Loc loc = ctx->getBelLocation(bel);
                    bb.x0 = std::min(bb.x0, loc.x);
                    bb.x1 = std::max(bb.x1, loc.x);
                    bb.y0 = std::min(bb.y0, loc.y);
                    bb.y1 = std::max(bb.y1, loc.y);
                }
            } else {
                bb.x0 = 0;
                bb.y0 = 0;
                bb.x1 = max_x;
                bb.y1 = max_y;
            }
            region_bounds[r->name] = bb;
        }
        build_port_index();
    }

    ~ParallelRefinementPlacer()
    {
        for (auto &net : ctx->nets)
            net.second->udata = old_udata[net.second->udata];
    }
    std::vector<CellInfo *> autoplaced;

    bool place(bool refine = false)
    {
        log_break();
        ctx->lock();

        size_t placed_cells = 0;
        std::vector<CellInfo *> chain_basis;
        if (!refine) {
            // Initial constraints placer
            for (auto &cell_entry : ctx->cells) {
                CellInfo *cell = cell_entry.second.get();
                auto loc = cell->attrs.find(ctx->id("BEL"));
                if (loc != cell->attrs.end()) {
                    std::string loc_name = loc->second;
                    BelId bel = ctx->getBelByName(ctx->id(loc_name));
                    if (bel == BelId()) {
                        log_error("No Bel named \'%s\' located for "
                                  "this chip (processing BEL attribute on \'%s\')\n",
                                  loc_name.c_str(), cell->name.c_str(ctx));
                    }

                    IdString bel_type = ctx->getBelType(bel);
                    if (bel_type != cell->type) {
                        log_error("Bel \'%s\' of type \'%s\' does not match cell "
                                  "\'%s\' of type \'%s\'\n",
                                  loc_name.c_str(), bel_type.c_str(ctx), cell->name.c_str(ctx), cell->type.c_str(ctx));
                    }
                    if (!ctx->isValidBelForCell(cell, bel)) {
                        log_error("Bel \'%s\' of type \'%s\' is not valid for cell "
                                  "\'%s\' of type \'%s\'\n",
                                  loc_name.c_str(), bel_type.c_str(ctx), cell->name.c_str(ctx), cell->type.c_str(ctx));
                    }

                    auto bound_cell = ctx->getBoundBelCell(bel);
                    if (bound_cell) {
                        log_error(
                                "Cell \'%s\' cannot be bound to bel \'%s\' since it is already bound to cell \'%s\'\n",
                                cell->name.c_str(ctx), loc_name.c_str(), bound_cell->name.c_str(ctx));
                    }

                    ctx->bindBel(bel, cell, STRENGTH_USER);
                    locked_bels.insert(bel);
                    placed_cells++;
                }
            }
            int constr_placed_cells = placed_cells;
            log_info("Placed %d cells based on constraints.\n", int(placed_cells));
            ctx->yield();

            // Sort to-place cells for deterministic initial placement

            for (auto &cell : ctx->cells) {
                CellInfo *ci = cell.second.get();
                if (ci->bel == BelId()) {
                    autoplaced.push_back(cell.second.get());
                }
            }
            std::sort(autoplaced.begin(), autoplaced.end(), [](CellInfo *a, CellInfo *b) { return a->name < b->name; });
            ctx->shuffle(autoplaced);
            auto iplace_start = std::chrono::high_resolution_clock::now();
            // Place cells randomly initially
            log_info("Creating initial placement for remaining %d cells.\n", int(autoplaced.size()));

            for (auto cell : autoplaced) {
                place_initial(cell);
                placed_cells++;
                if ((placed_cells - constr_placed_cells) % 500 == 0)
                    log_info("  initial placement placed %d/%d cells\n", int(placed_cells - constr_placed_cells),
                             int(autoplaced.size()));
            }
            if ((placed_cells - constr_placed_cells) % 500 != 0)
                log_info("  initial placement placed %d/%d cells\n", int(placed_cells - constr_placed_cells),
                         int(autoplaced.size()));
            if (cfg.budgetBased && ctx->slack_redist_iter > 0)
                assign_budget(ctx);
            ctx->yield();
            auto iplace_end = std::chrono::high_resolution_clock::now();
            log_info("Initial placement time %.02fs\n",
                     std::chrono::duration<float>(iplace_end - iplace_start).count());
            log_info("Running simulated annealing placer.\n");
        } else {
            for (auto &cell : ctx->cells) {
                CellInfo *ci = cell.second.get();
                if (ci->belStrength > STRENGTH_STRONG)
                    continue;
                else if (ci->constr_parent != nullptr)
                    continue;
                else if (!ci->constr_children.empty() || ci->constr_z != ci->UNCONSTR)
                    chain_basis.push_back(ci);
                else
                    autoplaced.push_back(ci);
            }
            require_legal = false;
            diameter = 3;
        }
        auto saplace_start = std::chrono::high_resolution_clock::now();

        // Invoke timing analysis to obtain criticalities
        if (!cfg.budgetBased)
            get_criticalities(ctx, &net_crit);

        // Calculate costs after initial placement
        setup_costs();
        curr_wirelen_cost = total_wirelen_cost();
        curr_timing_cost = total_timing_cost();
        last_wirelen_cost = curr_wirelen_cost;
        last_timing_cost = curr_timing_cost;

        wirelen_t avg_wirelen = curr_wirelen_cost;
        wirelen_t min_wirelen = curr_wirelen_cost;

        int n_no_progress = 0;
        temp = refine ? 1e-7 : cfg.startTemp;
        create_threadpool(8);
        // Main simulated annealing loop
        for (int iter = 1;; iter++) {
            n_move = n_accept = 0;
            improved = false;

            if (iter % 5 == 0 || iter == 1)
                log_info("  at iteration #%d: temp = %f, timing cost = "
                         "%.0f, wirelen = %.0f\n",
                         iter, temp, double(curr_timing_cost), double(curr_wirelen_cost));

            for (int m = 0; m < 15; ++m) {
                // Loop through all automatically placed cells
                run_threadpool();
                // Also try swapping chains, if applicable
                for (auto cb : chain_basis) {
                    Loc chain_base_loc = ctx->getBelLocation(cb->bel);
                    BelId try_base = random_bel_for_cell(cb,  [&](int n){return ctx->rng(n); }, chain_base_loc.z);
                    if (try_base != BelId() && try_base != cb->bel)
                        try_swap_chain(cb, try_base);
                }
            }

            if (curr_wirelen_cost < min_wirelen) {
                min_wirelen = curr_wirelen_cost;
                improved = true;
            }

            // Heuristic to improve placement on the 8k
            if (improved)
                n_no_progress = 0;
            else
                n_no_progress++;

            if (temp <= 1e-7 && n_no_progress >= (refine ? 1 : 5)) {
                log_info("  at iteration #%d: temp = %f, timing cost = "
                         "%.0f, wirelen = %.0f \n",
                         iter, temp, double(curr_timing_cost), double(curr_wirelen_cost));
                break;
            }

            double Raccept = double(n_accept) / double(n_move);

            int M = std::max(max_x, max_y) + 1;

            if (ctx->verbose)
                log("iter #%d: temp = %f, timing cost = "
                    "%.0f, wirelen = %.0f, dia = %d, Ra = %.02f \n",
                    iter, temp, double(curr_timing_cost), double(curr_wirelen_cost), diameter, Raccept);

            if (curr_wirelen_cost < 0.95 * avg_wirelen) {
                avg_wirelen = 0.8 * avg_wirelen + 0.2 * curr_wirelen_cost;
            } else {
                double diam_next = diameter * (1.0 - 0.44 + Raccept);
                diameter = std::max<int>(1, std::min<int>(M, int(diam_next + 0.5)));
                if (Raccept > 0.96) {
                    temp *= 0.5;
                } else if (Raccept > 0.8) {
                    temp *= 0.9;
                } else if (Raccept > 0.15 && diameter > 1) {
                    temp *= 0.95;
                } else {
                    temp *= 0.8;
                }
            }
            // Once cooled below legalise threshold, run legalisation and start requiring
            // legal moves only
            if (diameter < legalise_dia && require_legal) {
                if (legalise_relative_constraints(ctx)) {
                    // Only increase temperature if something was moved
                    autoplaced.clear();
                    chain_basis.clear();
                    for (auto cell : sorted(ctx->cells)) {
                        if (cell.second->belStrength <= STRENGTH_STRONG && cell.second->constr_parent == nullptr &&
                            !cell.second->constr_children.empty())
                            chain_basis.push_back(cell.second);
                        else if (cell.second->belStrength < STRENGTH_STRONG)
                            autoplaced.push_back(cell.second);
                    }
                    // temp = post_legalise_temp;
                    // diameter = std::min<int>(M, diameter * post_legalise_dia_scale);
                    ctx->shuffle(autoplaced);

                    // Legalisation is a big change so force a slack redistribution here
                    if (ctx->slack_redist_iter > 0 && cfg.budgetBased)
                        assign_budget(ctx, true /* quiet */);
                }
                require_legal = false;
            } else if (cfg.budgetBased && ctx->slack_redist_iter > 0 && iter % ctx->slack_redist_iter == 0) {
                assign_budget(ctx, true /* quiet */);
            }

            // Invoke timing analysis to obtain criticalities
            if (!cfg.budgetBased)
                get_criticalities(ctx, &net_crit);
            // Need to rebuild costs after criticalities change
            setup_costs();
            // Recalculate total metric entirely to avoid rounding errors
            // accumulating over time
            curr_wirelen_cost = total_wirelen_cost();
            curr_timing_cost = total_timing_cost();
            last_wirelen_cost = curr_wirelen_cost;
            last_timing_cost = curr_timing_cost;
            // Let the UI show visualization updates.
            ctx->yield();
        }
        kill_threadpool();
        auto saplace_end = std::chrono::high_resolution_clock::now();
        log_info("SA placement time %.02fs\n", std::chrono::duration<float>(saplace_end - saplace_start).count());

        // Final post-pacement validitiy check
        ctx->yield();
        for (auto bel : ctx->getBels()) {
            CellInfo *cell = ctx->getBoundBelCell(bel);
            if (!ctx->isBelLocationValid(bel)) {
                std::string cell_text = "no cell";
                if (cell != nullptr)
                    cell_text = std::string("cell '") + ctx->nameOf(cell) + "'";
                if (ctx->force) {
                    log_warning("post-placement validity check failed for Bel '%s' "
                                "(%s)\n",
                                ctx->getBelName(bel).c_str(ctx), cell_text.c_str());
                } else {
                    log_error("post-placement validity check failed for Bel '%s' "
                              "(%s)\n",
                              ctx->getBelName(bel).c_str(ctx), cell_text.c_str());
                }
            }
        }
        for (auto cell : sorted(ctx->cells))
            if (get_constraints_distance(ctx, cell.second) != 0)
                log_error("constraint satisfaction check failed for cell '%s' at Bel '%s'\n", cell.first.c_str(ctx),
                          ctx->getBelName(cell.second->bel).c_str(ctx));
        timing_analysis(ctx);
        ctx->unlock();
        return true;
    }

  private:
    // Initial random placement
    void place_initial(CellInfo *cell)
    {
        bool all_placed = false;
        int iters = 25;
        while (!all_placed) {
            BelId best_bel = BelId();
            uint64_t best_score = std::numeric_limits<uint64_t>::max(),
                     best_ripup_score = std::numeric_limits<uint64_t>::max();
            CellInfo *ripup_target = nullptr;
            BelId ripup_bel = BelId();
            if (cell->bel != BelId()) {
                ctx->unbindBel(cell->bel);
            }
            IdString targetType = cell->type;

            auto proc_bel = [&](BelId bel) {
                if (ctx->getBelType(bel) == targetType && ctx->isValidBelForCell(cell, bel)) {
                    if (ctx->checkBelAvail(bel)) {
                        uint64_t score = ctx->rng64();
                        if (score <= best_score) {
                            best_score = score;
                            best_bel = bel;
                        }
                    } else {
                        uint64_t score = ctx->rng64();
                        CellInfo *bound_cell = ctx->getBoundBelCell(bel);
                        if (score <= best_ripup_score && bound_cell->belStrength < STRENGTH_STRONG) {
                            best_ripup_score = score;
                            ripup_target = bound_cell;
                            ripup_bel = bel;
                        }
                    }
                }
            };

            if (cell->region != nullptr && cell->region->constr_bels) {
                for (auto bel : cell->region->bels) {
                    proc_bel(bel);
                }
            } else {
                for (auto bel : ctx->getBels()) {
                    proc_bel(bel);
                }
            }

            if (best_bel == BelId()) {
                if (iters == 0 || ripup_bel == BelId())
                    log_error("failed to place cell '%s' of type '%s'\n", cell->name.c_str(ctx), cell->type.c_str(ctx));
                --iters;
                ctx->unbindBel(ripup_target->bel);
                best_bel = ripup_bel;
            } else {
                all_placed = true;
            }
            ctx->bindBel(best_bel, cell, STRENGTH_WEAK);

            // Back annotate location
            cell->attrs[ctx->id("BEL")] = ctx->getBelName(cell->bel).str(ctx);
            cell = ripup_target;
        }
    }



    // Attempt a SA position swap "for real"
    bool try_swap_position(CellInfo *cell, BelId newBel)
    {
        static const double epsilon = 1e-20;
        moveChange.reset();
        if (is_constrained(cell))
            return false;
        BelId oldBel = cell->bel;
        CellInfo *other_cell = ctx->getBoundBelCell(newBel);
        if (other_cell != nullptr && (is_constrained(other_cell) || other_cell->belStrength > STRENGTH_WEAK)) {
            return false;
        }
        int old_dist = get_constraints_distance(ctx, cell);
        int new_dist;
        if (other_cell != nullptr)
            old_dist += get_constraints_distance(ctx, other_cell);
        double delta = 0;
        ctx->unbindBel(oldBel);
        if (other_cell != nullptr) {
            ctx->unbindBel(newBel);
        }

        ctx->bindBel(newBel, cell, STRENGTH_WEAK);

        if (other_cell != nullptr) {
            ctx->bindBel(oldBel, other_cell, STRENGTH_WEAK);
        }

        add_move_cell(moveChange, cell, oldBel);

        if (other_cell != nullptr) {
            add_move_cell(moveChange, other_cell, newBel);
        }

        if (!ctx->isBelLocationValid(newBel) || ((other_cell != nullptr && !ctx->isBelLocationValid(oldBel)))) {
            ctx->unbindBel(newBel);
            if (other_cell != nullptr)
                ctx->unbindBel(oldBel);
            goto swap_fail;
        }

        // Recalculate metrics for all nets touched by the peturbation
        compute_cost_changes(moveChange);

        new_dist = get_constraints_distance(ctx, cell);
        if (other_cell != nullptr)
            new_dist += get_constraints_distance(ctx, other_cell);
        delta = lambda * (moveChange.timing_delta / std::max<double>(last_timing_cost, epsilon)) +
                (1 - lambda) * (double(moveChange.wirelen_delta) / std::max<double>(last_wirelen_cost, epsilon));
        delta += (cfg.constraintWeight / temp) * (new_dist - old_dist) / last_wirelen_cost;
        // SA acceptance criterea
        if (delta < 0 || (temp > 1e-8 && (ctx->rng() / float(0x3fffffff)) <= std::exp(-delta / temp))) {
        } else {
            if (other_cell != nullptr)
                ctx->unbindBel(oldBel);
            ctx->unbindBel(newBel);
            goto swap_fail;
        }
        commit_cost_changes(moveChange);
#if 0
        log_info("swap %s -> %s\n", cell->name.c_str(ctx), ctx->getBelName(newBel).c_str(ctx));
        if (other_cell != nullptr)
            log_info("swap %s -> %s\n", other_cell->name.c_str(ctx), ctx->getBelName(oldBel).c_str(ctx));
#endif
        return true;
    swap_fail:
        ctx->bindBel(oldBel, cell, STRENGTH_WEAK);
        if (other_cell != nullptr) {
            ctx->bindBel(newBel, other_cell, STRENGTH_WEAK);
        }
        return false;
    }

    inline bool is_constrained(CellInfo *cell)
    {
        return cell->constr_parent != nullptr || !cell->constr_children.empty();
    }

    // Swap the Bel of a cell with another, return the original location
    BelId swap_cell_bels(CellInfo *cell, BelId newBel)
    {
        BelId oldBel = cell->bel;
#if 0
        log_info("%s old: %s new: %s\n", cell->name.c_str(ctx), ctx->getBelName(cell->bel).c_str(ctx), ctx->getBelName(newBel).c_str(ctx));
#endif
        CellInfo *bound = ctx->getBoundBelCell(newBel);
        if (bound != nullptr)
            ctx->unbindBel(newBel);
        ctx->unbindBel(oldBel);
        ctx->bindBel(newBel, cell, is_constrained(cell) ? STRENGTH_STRONG : STRENGTH_WEAK);
        if (bound != nullptr)
            ctx->bindBel(oldBel, bound, is_constrained(bound) ? STRENGTH_STRONG : STRENGTH_WEAK);
        return oldBel;
    }

    // Discover the relative positions of all cells in a chain
    void discover_chain(Loc baseLoc, CellInfo *cell, std::vector<std::pair<CellInfo *, Loc>> &cell_rel)
    {
        Loc cellLoc = ctx->getBelLocation(cell->bel);
        Loc rel{cellLoc.x - baseLoc.x, cellLoc.y - baseLoc.y, cellLoc.z};
        cell_rel.emplace_back(std::make_pair(cell, rel));
        for (auto child : cell->constr_children)
            discover_chain(baseLoc, child, cell_rel);
    }

    // Attempt to swap a chain with a non-chain
    bool try_swap_chain(CellInfo *cell, BelId newBase)
    {
        std::vector<std::pair<CellInfo *, Loc>> cell_rel;
        std::unordered_set<IdString> cells;
        std::vector<std::pair<CellInfo *, BelId>> moves_made;
        std::vector<std::pair<CellInfo *, BelId>> dest_bels;
        double delta = 0;
        moveChange.reset();
        if (ctx->debug)
            log_info("finding cells for chain swap %s\n", cell->name.c_str(ctx));

        Loc baseLoc = ctx->getBelLocation(cell->bel);
        discover_chain(baseLoc, cell, cell_rel);
        Loc newBaseLoc = ctx->getBelLocation(newBase);
        NPNR_ASSERT(newBaseLoc.z == baseLoc.z);
        for (const auto &cr : cell_rel)
            cells.insert(cr.first->name);

        for (const auto &cr : cell_rel) {
            Loc targetLoc = {newBaseLoc.x + cr.second.x, newBaseLoc.y + cr.second.y, cr.second.z};
            BelId targetBel = ctx->getBelByLocation(targetLoc);
            if (targetBel == BelId())
                return false;
            if (ctx->getBelType(targetBel) != cell->type)
                return false;
            CellInfo *bound = ctx->getBoundBelCell(targetBel);
            // We don't consider swapping chains with other chains, at least for the time being - unless it is
            // part of this chain
            if (bound != nullptr && !cells.count(bound->name) &&
                (bound->belStrength >= STRENGTH_STRONG || is_constrained(bound)))
                return false;
            dest_bels.emplace_back(std::make_pair(cr.first, targetBel));
        }
        if (ctx->debug)
            log_info("trying chain swap %s\n", cell->name.c_str(ctx));
        // <cell, oldBel>
        for (const auto &db : dest_bels) {
            BelId oldBel = swap_cell_bels(db.first, db.second);
            moves_made.emplace_back(std::make_pair(db.first, oldBel));
        }
        for (const auto &mm : moves_made) {
            if (!ctx->isBelLocationValid(mm.first->bel) || !check_cell_bel_region(mm.first, mm.first->bel))
                goto swap_fail;
            if (!ctx->isBelLocationValid(mm.second))
                goto swap_fail;
            CellInfo *bound = ctx->getBoundBelCell(mm.second);
            if (bound && !check_cell_bel_region(bound, bound->bel))
                goto swap_fail;
            add_move_cell(moveChange, mm.first, mm.second);
            if (bound != nullptr)
                add_move_cell(moveChange, bound, mm.first->bel);
        }
        compute_cost_changes(moveChange);
        delta = lambda * (moveChange.timing_delta / last_timing_cost) +
                (1 - lambda) * (double(moveChange.wirelen_delta) / last_wirelen_cost);
        n_move++;
        // SA acceptance criterea
        if (delta < 0 || (temp > 1e-9 && (ctx->rng() / float(0x3fffffff)) <= std::exp(-delta / temp))) {
            n_accept++;
            if (ctx->debug)
                log_info("accepted chain swap %s\n", cell->name.c_str(ctx));
        } else {
            goto swap_fail;
        }
        commit_cost_changes(moveChange);
        return true;
    swap_fail:
        for (const auto &entry : boost::adaptors::reverse(moves_made))
            swap_cell_bels(entry.first, entry.second);
        return false;
    }

    // Find a random Bel of the correct type for a cell, within the specified.//
    // diameter
    template <typename Trng>
    BelId random_bel_for_cell(CellInfo *cell, Trng custom_rng, int force_z = -1)
    {
        IdString targetType = cell->type;
        Loc curr_loc = ctx->getBelLocation(cell->bel);
        int count = 0;

        int dx = diameter, dy = diameter;
        if (cell->region != nullptr && cell->region->constr_bels) {
            dx = std::min(diameter, (region_bounds[cell->region->name].x1 - region_bounds[cell->region->name].x0) + 1);
            dy = std::min(diameter, (region_bounds[cell->region->name].y1 - region_bounds[cell->region->name].y0) + 1);
            // Clamp location to within bounds
            curr_loc.x = std::max(region_bounds[cell->region->name].x0, curr_loc.x);
            curr_loc.x = std::min(region_bounds[cell->region->name].x1, curr_loc.x);
            curr_loc.y = std::max(region_bounds[cell->region->name].y0, curr_loc.y);
            curr_loc.y = std::min(region_bounds[cell->region->name].y1, curr_loc.y);
        }

        while (true) {
            int nx = custom_rng(2 * dx + 1) + std::max(curr_loc.x - dx, 0);
            int ny = custom_rng(2 * dy + 1) + std::max(curr_loc.y - dy, 0);
            int beltype_idx, beltype_cnt;
            std::tie(beltype_idx, beltype_cnt) = bel_types.at(targetType);
            if (beltype_cnt < cfg.minBelsForGridPick)
                nx = ny = 0;
            if (nx >= int(fast_bels.at(beltype_idx).size()))
                continue;
            if (ny >= int(fast_bels.at(beltype_idx).at(nx).size()))
                continue;
            const auto &fb = fast_bels.at(beltype_idx).at(nx).at(ny);
            if (fb.size() == 0)
                continue;
            BelId bel = fb.at(ctx->rng(int(fb.size())));
            if (force_z != -1) {
                Loc loc = ctx->getBelLocation(bel);
                if (loc.z != force_z)
                    continue;
            }
            if (!check_cell_bel_region(cell, bel))
                continue;
            if (locked_bels.find(bel) != locked_bels.end())
                continue;
            count++;
            return bel;
        }
    }

    // Return true if a net is to be entirely ignored
    inline bool ignore_net(NetInfo *net)
    {
        return net->driver.cell == nullptr || net->driver.cell->bel == BelId() ||
               ctx->getBelGlobalBuf(net->driver.cell->bel);
    }

    // Get the bounding box for a net
    inline BoundingBox get_net_bounds(NetInfo *net, const std::unordered_map<IdString, BelId> &movedCells = {})
    {
        BoundingBox bb;
        NPNR_ASSERT(net->driver.cell != nullptr);
        Loc dloc = ctx->getBelLocation(cell_bel(net->driver.cell, movedCells));
        bb.x0 = dloc.x;
        bb.x1 = dloc.x;
        bb.y0 = dloc.y;
        bb.y1 = dloc.y;

        for (auto user : net->users) {
            if (user.cell->bel == BelId())
                continue;
            Loc uloc = ctx->getBelLocation(cell_bel(user.cell, movedCells));
            bb.x0 = std::min(bb.x0, uloc.x);
            bb.x1 = std::max(bb.x1, uloc.x);
            bb.y0 = std::min(bb.y0, uloc.y);
            bb.y1 = std::max(bb.y1, uloc.y);
        }

        return bb;
    }

    // Get the timing cost for an arc of a net
    inline double get_timing_cost(NetInfo *net, size_t user, const std::unordered_map<IdString, BelId> &movedCells = {})
    {
        int cc;
        if (net->driver.cell == nullptr)
            return 0;
        if (ctx->getPortTimingClass(net->driver.cell, net->driver.port, cc) == TMG_IGNORE)
            return 0;
        if (cfg.budgetBased) {
            double delay = ctx->getDelayNS(ctx->predictDelay(net, net->users.at(user)));
            return std::min(10.0, std::exp(delay - ctx->getDelayNS(net->users.at(user).budget)));
        } else {
            auto crit = net_crit.find(net->name);
            if (crit == net_crit.end() || crit->second.criticality.empty())
                return 0;
            double delay;
            if (movedCells.count(net->driver.cell->name) || movedCells.count(net->users.at(user).cell->name)) {
                // Have to use estimateDelay here
                BelId src = cell_bel(net->driver.cell, movedCells), dest = cell_bel(net->users.at(user).cell, movedCells);
                delay = ctx->getDelayNS(ctx->estimateDelay(ctx->getBelPinWire(src, net->driver.port), ctx->getBelPinWire(dest, net->users.at(user).port)));
            } else {
               delay = ctx->getDelayNS(ctx->predictDelay(net, net->users.at(user)));
            }
            return delay * std::pow(crit->second.criticality.at(user), crit_exp);
        }
    }

    // Set up the cost maps
    void setup_costs()
    {
        for (auto net : sorted(ctx->nets)) {
            NetInfo *ni = net.second;
            if (ignore_net(ni))
                continue;
            net_bounds[ni->udata] = get_net_bounds(ni);
            if (ctx->timing_driven && int(ni->users.size()) < cfg.timingFanoutThresh)
                for (size_t i = 0; i < ni->users.size(); i++)
                    net_arc_tcost[ni->udata][i] = get_timing_cost(ni, i);
        }
    }

    // Get the total wiring cost for the design
    wirelen_t total_wirelen_cost()
    {
        wirelen_t cost = 0;
        for (const auto &net : net_bounds)
            cost += net.hpwl();
        return cost;
    }

    // Get the total timing cost for the design
    double total_timing_cost()
    {
        double cost = 0;
        for (const auto &net : net_arc_tcost) {
            for (auto arc_cost : net) {
                cost += arc_cost;
            }
        }
        return cost;
    }

    // Cost-change-related data for a move
    struct MoveChangeData
    {
        std::vector<decltype(NetInfo::udata)> bounds_changed_nets;
        std::vector<std::pair<decltype(NetInfo::udata), size_t>> changed_arcs;

        std::vector<bool> already_bounds_changed;
        std::vector<std::vector<bool>> already_changed_arcs;

        std::vector<std::pair<decltype(NetInfo::udata), BoundingBox>> new_net_bounds;
        std::vector<std::pair<std::pair<decltype(NetInfo::udata), size_t>, double>> new_arc_costs;

        wirelen_t wirelen_delta = 0;
        double timing_delta = 0;

        void reset()
        {
            for (auto bc : bounds_changed_nets)
                already_bounds_changed[bc] = false;
            for (const auto &tc : changed_arcs)
                already_changed_arcs[tc.first][tc.second] = false;
            bounds_changed_nets.clear();
            changed_arcs.clear();
            new_net_bounds.clear();
            new_arc_costs.clear();
            wirelen_delta = 0;
            timing_delta = 0;
        }

    } moveChange;

    BelId cell_bel(CellInfo *cell, const std::unordered_map<IdString, BelId> &movedCells) {
        auto mc = movedCells.find(cell->name);
        if (mc != movedCells.end())
            return mc->second;
        return cell->bel;
    }

    void add_move_cell(MoveChangeData &mc, CellInfo *cell, BelId old_bel, const std::unordered_map<IdString, BelId> &movedCells = {})
    {
        Loc curr_loc = ctx->getBelLocation(cell_bel(cell, movedCells));
        Loc old_loc = ctx->getBelLocation(old_bel);
        // Check net bounds
        for (const auto &port : cell->ports) {
            NetInfo *pn = port.second.net;
            if (pn == nullptr)
                continue;
            if (ignore_net(pn))
                continue;
            const BoundingBox &curr_bounds = net_bounds[pn->udata];
            // If the old location was at the edge of the bounds, or the new location exceeds the bounds,
            // an update is needed
            if (curr_bounds.touches_bounds(old_loc.x, old_loc.y) || !curr_bounds.is_inside_inc(curr_loc.x, curr_loc.y))
                if (!mc.already_bounds_changed[pn->udata]) {
                    mc.bounds_changed_nets.push_back(pn->udata);
                    mc.already_bounds_changed[pn->udata] = true;
                }
            if (ctx->timing_driven && int(pn->users.size()) < cfg.timingFanoutThresh) {
                // Output ports - all arcs change timing
                if (port.second.type == PORT_OUT) {
                    int cc;
                    TimingPortClass cls = ctx->getPortTimingClass(cell, port.first, cc);
                    if (cls != TMG_IGNORE)
                        for (size_t i = 0; i < pn->users.size(); i++)
                            if (!mc.already_changed_arcs[pn->udata][i]) {
                                mc.changed_arcs.emplace_back(std::make_pair(pn->udata, i));
                                mc.already_changed_arcs[pn->udata][i] = true;
                            }
                } else if (port.second.type == PORT_IN) {
                    auto usr = fast_port_to_user.at(&port.second);
                    if (!mc.already_changed_arcs[pn->udata][usr]) {
                        mc.changed_arcs.emplace_back(std::make_pair(pn->udata, usr));
                        mc.already_changed_arcs[pn->udata][usr] = true;
                    }
                }
            }
        }
    }

    void compute_cost_changes(MoveChangeData &md, const std::unordered_map<IdString, BelId> &movedCells = {})
    {
        for (const auto &bc : md.bounds_changed_nets) {
            wirelen_t old_hpwl = net_bounds.at(bc).hpwl();
            auto bounds = get_net_bounds(net_by_udata.at(bc), movedCells);
            md.new_net_bounds.emplace_back(std::make_pair(bc, bounds));
            md.wirelen_delta += (bounds.hpwl() - old_hpwl);
            md.already_bounds_changed[bc] = false;
        }
        if (ctx->timing_driven) {
            for (const auto &tc : md.changed_arcs) {
                double old_cost = net_arc_tcost.at(tc.first).at(tc.second);
                double new_cost = get_timing_cost(net_by_udata.at(tc.first), tc.second, movedCells);
                md.new_arc_costs.emplace_back(std::make_pair(tc, new_cost));
                md.timing_delta += (new_cost - old_cost);
                md.already_changed_arcs[tc.first][tc.second] = false;
            }
        }
    }

    void commit_cost_changes(MoveChangeData &md, const std::unordered_map<IdString, BelId> &movedCells = {})
    {
        for (const auto &bc : md.new_net_bounds)
            net_bounds[bc.first] = bc.second;
        for (const auto &tc : md.new_arc_costs)
            net_arc_tcost[tc.first.first].at(tc.first.second) = tc.second;
        curr_wirelen_cost += md.wirelen_delta;
        curr_timing_cost += md.timing_delta;
    }


    // Context for a thread finding and evaluating moves
    struct MoveEvaluatorData {
        MoveChangeData moveChange;
        // We use this structure to store changes in a thread-local way, rather than attempting to
        // make changes to the global netlist
        // FIXME: faster data structures here?
        std::unordered_map<IdString, BelId> movedCells;

        // This starts with cell -> BelId()
        // and ends with cell -> newBel if a possible move is found,
        // or remains as cell -> BelId() otherwise
        std::vector<std::pair<CellInfo*, BelId>> evalCells;

        // Carefully controlled seed for determinism
        uint64_t seed;

        // For thread pool management
        int workerid;

        std::thread t;
        std::mutex m;
        std::condition_variable cv;
        bool ready = false, processed = false, die = false;
        int moves = 0, accepted = 0;
    };

    std::vector<MoveEvaluatorData*> threadpool;

    void move_evaluator_thread(int k) {
        MoveEvaluatorData &d = *(threadpool.at(k));
        while (true) {
            std::unique_lock<std::mutex> lk(d.m);
            if (!d.ready)
                d.cv.wait(lk, [&]{return d.ready;});
            if (d.die)
                return;
            d.ready = false;
            d.moves = 0;
            d.accepted = 0;
            for (auto &cell : d.evalCells) {
                CellInfo *ci = cell.first;

                // Use a rng seeded with only attributes of the thread, to ensure
                // determinism regardless of thread configuration
                uint64_t rngstate = d.seed;
                rngstate ^= uint64_t(ci->name.index);
                rngstate ^= uint64_t(ctx->getBelChecksum(ci->bel)) << 32;

                auto rng64 = [&]() {
                    uint64_t retval = rngstate * 0x2545F4914F6CDD1D;

                    rngstate ^= rngstate >> 12;
                    rngstate ^= rngstate << 25;
                    rngstate ^= rngstate >> 27;

                    return retval;
                };
                for (int i = 0; i < 5; i++)
                    rng64();


                auto rng = [&](int n) {
                    assert(n > 0);

                    // round up to power of 2
                    int m = n - 1;
                    m |= (m >> 1);
                    m |= (m >> 2);
                    m |= (m >> 4);
                    m |= (m >> 8);
                    m |= (m >> 16);
                    m += 1;

                    while (1) {
                        int x = rng64() & (m - 1);
                        if (x < n)
                            return x;
                    }
                };

                // Number of bels to explore
                int M = 1;
                BelId best_bel;
                double best_cost_delta = std::numeric_limits<double>::max();
                for (int i = 0; i < M; i++) {
                    BelId old_bel = ci->bel;
                    BelId try_bel = random_bel_for_cell(ci, rng);
                    if (try_bel == BelId() || try_bel == old_bel)
                        continue;
                    CellInfo *bound = ctx->getBoundBelCell(try_bel);
                    if (bound != nullptr) {
                        if (bound->belStrength >= STRENGTH_STRONG || is_constrained(bound))
                            continue;
                    }
                    d.movedCells[ci->name] = try_bel;
                    add_move_cell(d.moveChange, ci, old_bel, d.movedCells);
                    if (bound != nullptr) {
                        d.movedCells[bound->name] = old_bel;
                        add_move_cell(d.moveChange, bound, try_bel, d.movedCells);
                    }
                    compute_cost_changes(d.moveChange, d.movedCells);
                    double cost_delta = lambda * (d.moveChange.timing_delta / last_timing_cost) +
                                        (1 - lambda) * (double(d.moveChange.wirelen_delta) / double(last_wirelen_cost));
                    if (cost_delta < best_cost_delta) {
                        best_cost_delta = cost_delta;
                        best_bel = try_bel;
                    }
                    d.movedCells.clear();
                    d.moveChange.reset();
                }

                if (best_bel != BelId()) {
                    d.moves++;
                    if (best_cost_delta < 0 || (temp > 1e-9 && (rng(0x3fffffff) / float(0x3fffffff)) <=
                                                               std::exp(-best_cost_delta / temp))) {
                        log_info("%f %d\n", best_cost_delta, rng(10));
                        cell.second = best_bel;
                        d.accepted++;
                    }
                }
            }
            d.processed = true;
            lk.unlock();
            d.cv.notify_one();
        }
    }


    void create_threadpool(int n) {
        threadpool.resize(n);
        for (int i = 0; i < n; i++) {
            MoveEvaluatorData *p = new MoveEvaluatorData();
            threadpool.at(i) = p;
            p->moveChange.already_bounds_changed.resize(ctx->nets.size());
            p->moveChange.already_changed_arcs.resize(ctx->nets.size());
            for (auto &net : ctx->nets) {
                NetInfo *ni = net.second.get();
                p->moveChange.already_changed_arcs.at(ni->udata).resize(ni->users.size());
            }
            p->t = std::thread([this, i]() { move_evaluator_thread(i); });
            p->workerid = i;
        }
    }

    void run_threadpool() {
        // Split all the cells up into batches N cells, which are then split evenly between threads
        // This is a balance between QoR, and overhead dispatching work to threads
        const size_t N = 32;
        ctx->shuffle(autoplaced);
        for (size_t i = 0; i < autoplaced.size(); i += N) {
            uint64_t seed = ctx->rng64();
            size_t lb = i, ub = std::min(i + N, autoplaced.size());
            for (size_t j = 0; j < threadpool.size(); j++) {
                auto &p = *threadpool.at(j);
                {
                    std::lock_guard<std::mutex> lk(p.m);
                    size_t jlb = lb + (j * (ub - lb)) / threadpool.size(), jub =
                            lb + ((j + 1) * (ub - lb)) / threadpool.size();
                    p.seed = seed;
                    p.evalCells.clear();
                    for (size_t k = jlb; k < jub; k++) {
                        p.evalCells.emplace_back(autoplaced.at(k), BelId());
                    }
                    p.processed = false;
                    p.ready = true;
                }
            }
            for (auto &p : threadpool)
                p->cv.notify_one();
            // Wait for all threads to finish
            for (auto &p : threadpool) {
                //if (p->processed)
                //    continue;
                std::unique_lock<std::mutex> lk(p->m);
                p->cv.wait(lk, [&]() { return p->processed; });
                lk.unlock();
            }
            // Apply proposed changes from workers for real
            for (auto &p : threadpool) {
                n_accept += p->accepted;
                n_move += p->moves;
                for (auto &ec : p->evalCells) {
                    if (ec.second != BelId() && ec.second != ec.first->bel) {
                        try_swap_position(ec.first, ec.second);
                    }
                }
            }
        }
    }

    void kill_threadpool() {
        for (auto &p : threadpool) {
            p->die = true;
            p->ready = true;
            p->cv.notify_one();
            p->t.join();
            delete p;
            p = nullptr;
        }
        threadpool.clear();
    }

    // Build the cell port -> user index
    void build_port_index()
    {
        for (auto net : sorted(ctx->nets)) {
            NetInfo *ni = net.second;
            for (size_t i = 0; i < ni->users.size(); i++) {
                auto &usr = ni->users.at(i);
                fast_port_to_user[&(usr.cell->ports.at(usr.port))] = i;
            }
        }
    }

    // Get the combined wirelen/timing metric
    inline double curr_metric() { return lambda * curr_timing_cost + (1 - lambda) * curr_wirelen_cost; }

    // Map nets to their bounding box (so we can skip recompute for moves that do not exceed the bounds
    std::vector<BoundingBox> net_bounds;
    // Map net arcs to their timing cost (criticality * delay ns)
    std::vector<std::vector<double>> net_arc_tcost;

    // Fast lookup for cell port to net user index
    std::unordered_map<const PortInfo *, size_t> fast_port_to_user;

    // Wirelength and timing cost at last and current iteration
    wirelen_t last_wirelen_cost, curr_wirelen_cost;
    double last_timing_cost, curr_timing_cost;

    // Criticality data from timing analysis
    NetCriticalityMap net_crit;

    Context *ctx;
    float temp = 10;
    float crit_exp = 8;
    float lambda = 0.5;
    bool improved = false;
    int n_move, n_accept;
    int diameter = 35, max_x = 1, max_y = 1;
    std::unordered_map<IdString, std::tuple<int, int>> bel_types;
    std::unordered_map<IdString, BoundingBox> region_bounds;
    std::vector<std::vector<std::vector<std::vector<BelId>>>> fast_bels;
    std::unordered_set<BelId> locked_bels;
    std::vector<NetInfo *> net_by_udata;
    std::vector<decltype(NetInfo::udata)> old_udata;
    bool require_legal = true;
    const int legalise_dia = 4;
    Placer1Cfg cfg;
};

bool parallel_refine(Context *ctx, Placer1Cfg cfg)
{
    try {
        ParallelRefinementPlacer placer(ctx, cfg);
        placer.place(true);
        log_info("Checksum: 0x%08x\n", ctx->checksum());
#ifndef NDEBUG
        ctx->lock();
        ctx->check();
        ctx->unlock();
#endif
        return true;
    } catch (log_execution_error_exception) {
#ifndef NDEBUG
        ctx->check();
#endif
        return false;
    }
}

NEXTPNR_NAMESPACE_END
