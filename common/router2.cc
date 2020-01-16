/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019  David Shah <dave@ds0.me>
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
 *  Core routing algorithm based on CRoute:
 *
 *     CRoute: A Fast High-quality Timing-driven Connection-based FPGA Router
 *     Dries Vercruyce, Elias Vansteenkiste and Dirk Stroobandt
 *     DOI 10.1109/FCCM.2019.00017 [PDF on SciHub]
 *
 *  Modified for the nextpnr Arch API and data structures; optimised for
 *  real-world FPGA architectures in particular ECP5 and Xilinx UltraScale+
 *
 */

#include <algorithm>
#include <boost/container/flat_map.hpp>
#include <deque>
#include <fstream>
#include <queue>
#include <thread>
#include "log.h"
#include "nextpnr.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
struct Router2
{

    struct PerArcData
    {
        std::unordered_map<WireId, PipId> wires;
        ArcBounds bb;
    };

    // As we allow overlap at first; the nextpnr bind functions can't be used
    // as the primary relation between arcs and wires/pips
    struct PerNetData
    {
        std::vector<PerArcData> arcs;
        ArcBounds bb;
        // Coordinates of the center of the net, used for the weight-to-average
        int cx, cy, hpwl;
    };

    struct PerWireData
    {
        // net --> number of arcs; driving pip
        std::unordered_map<int, std::pair<int, PipId>> bound_nets;
        // Historical congestion cost
        float hist_cong_cost = 1.0;
        // Wire is unavailable as locked to another arc
        bool unavailable = false;
        // This wire has to be used for this net
        int reserved_net = -1;
    };

    float present_wire_cost(const PerWireData &w, int net_uid)
    {
        int other_sources = int(w.bound_nets.size());
        if (w.bound_nets.count(net_uid))
            other_sources -= 1;
        if (other_sources == 0)
            return 1.0f;
        else
            return 1 + other_sources * curr_cong_weight;
    }

    struct WireScore
    {
        float cost;
        float togo_cost;
        delay_t delay;
        float total() const { return cost + togo_cost; }
    };

    Context *ctx;

    // Use 'udata' for fast net lookups and indexing
    std::vector<NetInfo *> nets_by_udata;
    std::vector<PerNetData> nets;
    void setup_nets()
    {
        // Populate per-net and per-arc structures at start of routing
        nets.resize(ctx->nets.size());
        nets_by_udata.resize(ctx->nets.size());
        size_t i = 0;
        for (auto net : sorted(ctx->nets)) {
            NetInfo *ni = net.second;
            ni->udata = i;
            nets_by_udata.at(i) = ni;
            nets.at(i).arcs.resize(ni->users.size());

            // Start net bounding box at overall min/max
            nets.at(i).bb.x0 = std::numeric_limits<int>::max();
            nets.at(i).bb.x1 = std::numeric_limits<int>::min();
            nets.at(i).bb.y0 = std::numeric_limits<int>::max();
            nets.at(i).bb.y1 = std::numeric_limits<int>::min();
            nets.at(i).cx = 0;
            nets.at(i).cy = 0;

            if (ni->driver.cell != nullptr) {
                Loc drv_loc = ctx->getBelLocation(ni->driver.cell->bel);
                nets.at(i).cx += drv_loc.x;
                nets.at(i).cy += drv_loc.y;
            }

            for (size_t j = 0; j < ni->users.size(); j++) {
                auto &usr = ni->users.at(j);
                WireId src_wire = ctx->getNetinfoSourceWire(ni), dst_wire = ctx->getNetinfoSinkWire(ni, usr);
                if (ni->driver.cell == nullptr)
                    src_wire = dst_wire;
                if (src_wire == WireId())
                    log_error("No wire found for port %s on source cell %s.\n", ctx->nameOf(ni->driver.port),
                              ctx->nameOf(ni->driver.cell));
                if (dst_wire == WireId())
                    log_error("No wire found for port %s on destination cell %s.\n", ctx->nameOf(usr.port),
                              ctx->nameOf(usr.cell));
                // Set bounding box for this arc
                nets.at(i).arcs.at(j).bb = ctx->getRouteBoundingBox(src_wire, dst_wire);
                // Expand net bounding box to include this arc
                nets.at(i).bb.x0 = std::min(nets.at(i).bb.x0, nets.at(i).arcs.at(j).bb.x0);
                nets.at(i).bb.x1 = std::max(nets.at(i).bb.x1, nets.at(i).arcs.at(j).bb.x1);
                nets.at(i).bb.y0 = std::min(nets.at(i).bb.y0, nets.at(i).arcs.at(j).bb.y0);
                nets.at(i).bb.y1 = std::max(nets.at(i).bb.y1, nets.at(i).arcs.at(j).bb.y1);
                // Add location to centroid sum
                Loc usr_loc = ctx->getBelLocation(usr.cell->bel);
                nets.at(i).cx += usr_loc.x;
                nets.at(i).cy += usr_loc.y;
            }
            nets.at(i).hpwl = std::max(
                    std::abs(nets.at(i).bb.y1 - nets.at(i).bb.y0) + std::abs(nets.at(i).bb.x1 - nets.at(i).bb.x0), 1);
            nets.at(i).cx /= int(ni->users.size() + 1);
            nets.at(i).cy /= int(ni->users.size() + 1);
            if (ctx->debug)
                log_info("%s: bb=(%d, %d)->(%d, %d) c=(%d, %d) hpwl=%d\n", ctx->nameOf(ni), nets.at(i).bb.x0,
                         nets.at(i).bb.y0, nets.at(i).bb.x1, nets.at(i).bb.y1, nets.at(i).cx, nets.at(i).cy,
                         nets.at(i).hpwl);
            i++;
        }
    }

    std::unordered_map<WireId, PerWireData> wires;
    void setup_wires()
    {
        // Set up per-wire structures, so that MT parts don't have to do any memory allocation
        // This is possibly quite wasteful and not cache-optimal; further consideration necessary
        for (auto wire : ctx->getWires()) {
            wires[wire];
            NetInfo *bound = ctx->getBoundWireNet(wire);
            if (bound != nullptr) {
                wires[wire].bound_nets[bound->udata] = std::make_pair(1, bound->wires.at(wire).pip);
                if (bound->wires.at(wire).strength > STRENGTH_STRONG)
                    wires[wire].unavailable = true;
            }
        }
    }

    struct QueuedWire
    {

        explicit QueuedWire(WireId wire = WireId(), PipId pip = PipId(), Loc loc = Loc(), WireScore score = WireScore{},
                            int randtag = 0)
                : wire(wire), pip(pip), loc(loc), score(score), randtag(randtag){};

        WireId wire;
        PipId pip;
        Loc loc;
        WireScore score;
        int randtag = 0;

        struct Greater
        {
            bool operator()(const QueuedWire &lhs, const QueuedWire &rhs) const noexcept
            {
                float lhs_score = lhs.score.cost + lhs.score.togo_cost,
                      rhs_score = rhs.score.cost + rhs.score.togo_cost;
                return lhs_score == rhs_score ? lhs.randtag > rhs.randtag : lhs_score > rhs_score;
            }
        };
    };

    int bb_margin_x = 4, bb_margin_y = 4; // number of units outside the bounding box we may go
    bool hit_test_pip(ArcBounds &bb, Loc l)
    {
        return l.x >= (bb.x0 - bb_margin_x) && l.x <= (bb.x1 + bb_margin_x) && l.y >= (bb.y0 - bb_margin_y) &&
               l.y <= (bb.y1 + bb_margin_y);
    }

    double curr_cong_weight, hist_cong_weight, estimate_weight;
    // Soft-route a net (don't touch Arch data structures which might not be thread safe)
    // If is_mt is true, then strict bounding box rules are applied and log_* won't be called
    struct VisitInfo
    {
        WireScore score;
        PipId pip;
    };
    struct ThreadContext
    {
        // Nets to route
        std::vector<NetInfo *> route_nets;
        // Nets that failed routing
        std::vector<NetInfo *> failed_nets;

        std::vector<int> route_arcs;

        std::priority_queue<QueuedWire, std::vector<QueuedWire>, QueuedWire::Greater> queue;
        std::unordered_map<WireId, VisitInfo> visited;
        // Special case where one net has multiple logical arcs to the same physical sink
        std::unordered_set<WireId> processed_sinks;

        // Backwards routing
        std::queue<WireId> backwards_queue;
        std::unordered_map<WireId, PipId> backwards_pip;
    };

    enum ArcRouteResult
    {
        ARC_SUCCESS,
        ARC_RETRY_WITHOUT_BB,
        ARC_FATAL,
    };

// Define to make sure we don't print in a multithreaded context
#define ARC_LOG_ERR(...)                                                                                               \
    do {                                                                                                               \
        if (is_mt)                                                                                                     \
            return ARC_FATAL;                                                                                          \
        else                                                                                                           \
            log_error(__VA_ARGS__);                                                                                    \
    } while (0)
#define ROUTE_LOG_DBG(...)                                                                                             \
    do {                                                                                                               \
        if (!is_mt && ctx->debug)                                                                                      \
            log(__VA_ARGS__);                                                                                          \
    } while (0)

    void bind_pip_internal(NetInfo *net, size_t user, WireId wire, PipId pip)
    {
        auto &b = wires.at(wire).bound_nets[net->udata];
        ++b.first;
        if (b.first == 1) {
            b.second = pip;
        } else {
            NPNR_ASSERT(b.second == pip);
        }
        nets.at(net->udata).arcs.at(user).wires[wire] = pip;
    }

    void unbind_pip_internal(NetInfo *net, size_t user, WireId wire, bool dont_touch_arc = false)
    {
        auto &b = wires.at(wire).bound_nets[net->udata];
        --b.first;
        if (b.first == 0) {
            wires.at(wire).bound_nets.erase(net->udata);
        }
        if (!dont_touch_arc)
            nets.at(net->udata).arcs.at(user).wires.erase(wire);
    }

    void ripup_arc(NetInfo *net, size_t user)
    {
        auto &ad = nets.at(net->udata).arcs.at(user);
        for (auto &wire : ad.wires)
            unbind_pip_internal(net, user, wire.first, true);
        ad.wires.clear();
    }

    float score_wire_for_arc(NetInfo *net, size_t user, WireId wire, PipId pip)
    {
        auto &wd = wires.at(wire);
        auto &nd = nets.at(net->udata);
        float base_cost = ctx->getDelayNS(ctx->getPipDelay(pip).maxDelay() + ctx->getWireDelay(wire).maxDelay() +
                                          ctx->getDelayEpsilon());
        float present_cost = present_wire_cost(wd, net->udata);
        float hist_cost = wd.hist_cong_cost;
        float bias_cost = 0;
        int source_uses = 0;
        if (wd.bound_nets.count(net->udata))
            source_uses = wd.bound_nets.at(net->udata).first;
        if (pip != PipId()) {
            Loc pl = ctx->getPipLocation(pip);
            bias_cost = 0.5f * (base_cost / int(net->users.size())) *
                        ((std::abs(pl.x - nd.cx) + std::abs(pl.y - nd.cy)) / float(nd.hpwl));
        }
        return base_cost * hist_cost * present_cost / (1 + source_uses) + bias_cost;
    }

    float get_togo_cost(NetInfo *net, size_t user, WireId wire, WireId sink)
    {
        auto &wd = wires.at(wire);
        int source_uses = 0;
        if (wd.bound_nets.count(net->udata))
            source_uses = wd.bound_nets.at(net->udata).first;
        // FIXME: timing/wirelength balance?
        float ipin_cost = ctx->getDelayNS(ctx->getWireDelay(sink).maxDelay() + ctx->getDelayEpsilon());
        return std::max(0.0f, ctx->getDelayNS(ctx->estimateDelay(wire, sink)) - ipin_cost) / (1 + source_uses) +
               ipin_cost;
    }

    bool check_arc_routing(NetInfo *net, size_t usr)
    {
        auto &ad = nets.at(net->udata).arcs.at(usr);
        WireId src_wire = ctx->getNetinfoSourceWire(net);
        WireId dst_wire = ctx->getNetinfoSinkWire(net, net->users.at(usr));
        WireId cursor = dst_wire;
        while (ad.wires.count(cursor)) {
            auto &wd = wires.at(cursor);
            if (wd.bound_nets.size() != 1)
                return false;
            auto &uh = ad.wires.at(cursor);
            if (uh == PipId())
                break;
            cursor = ctx->getPipSrcWire(uh);
        }
        return (cursor == src_wire);
    }

    // Returns true if a wire contains no source ports or driving pips
    bool is_wire_undriveable(WireId wire)
    {
        for (auto bp : ctx->getWireBelPins(wire))
            if (ctx->getBelPinType(bp.bel, bp.pin) != PORT_IN)
                return false;
        for (auto p : ctx->getPipsUphill(wire))
            return false;
        return true;
    }

    // Find all the wires that must be used to route a given arc
    void reserve_wires_for_arc(NetInfo *net, size_t i)
    {
        // This is slightly tricky, because of the possibility of "diamonds"
        // eg       /--C--\\
        //    sink ----B----D--...
        // we need to discover that D is a reserved wire; despite the branch and choice of B/C
        WireId src = ctx->getNetinfoSourceWire(net);
        WireId sink = ctx->getNetinfoSinkWire(net, net->users.at(i));
        if (sink == WireId())
            return;
        std::unordered_set<WireId> rsv;
        WireId cursor = sink;
        bool done = false;
        log("resevering wires for arc %d of net %s\n", int(i), ctx->nameOf(net));
        while (!done) {
            auto &wd = wires.at(cursor);
            if (ctx->debug)
                log("      %s\n", ctx->nameOfWire(cursor));
            wd.reserved_net = net->udata;
            if (cursor == src)
                break;
            WireId next_cursor;
            for (auto uh : ctx->getPipsUphill(cursor)) {
                WireId w = ctx->getPipSrcWire(uh);
                if (is_wire_undriveable(w))
                    continue;
                if (next_cursor != WireId()) {
                    done = true;
                    break;
                }
                next_cursor = w;
            }
            if (next_cursor == WireId())
                break;
            cursor = next_cursor;
        }
    }

    void find_all_reserved_wires()
    {
        for (auto net : nets_by_udata) {
            WireId src = ctx->getNetinfoSourceWire(net);
            if (src == WireId())
                continue;
            for (size_t i = 0; i < net->users.size(); i++)
                reserve_wires_for_arc(net, i);
        }
    }

    ArcRouteResult route_arc(ThreadContext &t, NetInfo *net, size_t i, bool is_mt, bool is_bb = true)
    {

        auto &nd = nets[net->udata];
        auto &ad = nd.arcs[i];
        auto &usr = net->users.at(i);
        ROUTE_LOG_DBG("Routing arc %d of net '%s' (%d, %d) -> (%d, %d)\n", int(i), ctx->nameOf(net), ad.bb.x0, ad.bb.y0,
                      ad.bb.x1, ad.bb.y1);
        WireId src_wire = ctx->getNetinfoSourceWire(net), dst_wire = ctx->getNetinfoSinkWire(net, usr);

        if (src_wire == WireId())
            ARC_LOG_ERR("No wire found for port %s on source cell %s.\n", ctx->nameOf(net->driver.port),
                        ctx->nameOf(net->driver.cell));
        if (dst_wire == WireId())
            ARC_LOG_ERR("No wire found for port %s on destination cell %s.\n", ctx->nameOf(usr.port),
                        ctx->nameOf(usr.cell));
        // Check if arc was already done _in this iteration_
        if (t.processed_sinks.count(dst_wire))
            return ARC_SUCCESS;

        if (!t.queue.empty()) {
            std::priority_queue<QueuedWire, std::vector<QueuedWire>, QueuedWire::Greater> new_queue;
            t.queue.swap(new_queue);
        }
        if (!t.backwards_queue.empty()) {
            std::queue<WireId> new_queue;
            t.backwards_queue.swap(new_queue);
        }
        // First try strongly iteration-limited routing backwards BFS
        // this will deal with certain nets faster than forward A*
        // and comes at a minimal performance cost for the others
        // This could also be used to speed up forwards routing by a hybrid
        // bidirectional approach
        int backwards_iter = 0;
        int backwards_limit = 10;
        t.backwards_pip.clear();
        t.backwards_queue.push(dst_wire);
        while (!t.backwards_queue.empty() && backwards_iter < backwards_limit) {
            WireId cursor = t.backwards_queue.front();
            t.backwards_queue.pop();
            auto &cwd = wires.at(cursor);
            PipId cpip;
            if (cwd.bound_nets.count(net->udata)) {
                // If we can tack onto existing routing; try that
                // Only do this if the existing routing is uncontented; however
                WireId cursor2 = cursor;
                bool bwd_merge_fail = false;
                while (wires.at(cursor2).bound_nets.count(net->udata)) {
                    if (wires.at(cursor2).bound_nets.size() > 1) {
                        bwd_merge_fail = true;
                        break;
                    }
                    PipId p = wires.at(cursor2).bound_nets.at(net->udata).second;
                    if (p == PipId())
                        break;
                    cursor2 = ctx->getPipSrcWire(p);
                }
                if (!bwd_merge_fail && cursor2 == src_wire) {
                    // Found a path to merge to existing routing; backwards
                    cursor2 = cursor;
                    while (wires.at(cursor2).bound_nets.count(net->udata)) {
                        PipId p = wires.at(cursor2).bound_nets.at(net->udata).second;
                        if (p == PipId())
                            break;
                        cursor2 = ctx->getPipSrcWire(p);
                        t.backwards_pip[cursor2] = p;
                    }
                    break;
                }
                cpip = cwd.bound_nets.at(net->udata).second;
            }
            bool did_something = false;
            for (auto uh : ctx->getPipsUphill(cursor)) {
                did_something = true;
                if (!ctx->checkPipAvail(uh) && ctx->getBoundPipNet(uh) != net)
                    continue;
                if (cpip != PipId() && cpip != uh)
                    continue; // don't allow multiple pips driving a wire with a net
                WireId next = ctx->getPipSrcWire(uh);
                if (t.backwards_pip.count(next))
                    continue; // skip wires that have already been visited
                auto &wd = wires.at(next);
                if (wd.unavailable)
                    continue;
                if (wd.reserved_net != -1 && wd.reserved_net != net->udata)
                    continue;
                if (wd.bound_nets.size() > 1 || (wd.bound_nets.size() == 1 && !wd.bound_nets.count(net->udata)))
                    continue; // never allow congestion in backwards routing
                t.backwards_queue.push(next);
                t.backwards_pip[next] = uh;
            }
            if (did_something)
                ++backwards_iter;
        }
        // Check if backwards routing succeeded in reaching source
        if (t.backwards_pip.count(src_wire)) {
            ROUTE_LOG_DBG("   Routed (backwards): ");
            WireId cursor_fwd = src_wire;
            bind_pip_internal(net, i, src_wire, PipId());
            while (t.backwards_pip.count(cursor_fwd)) {
                auto &v = t.backwards_pip.at(cursor_fwd);
                cursor_fwd = ctx->getPipDstWire(v);
                bind_pip_internal(net, i, cursor_fwd, v);
                if (ctx->debug) {
                    auto &wd = wires.at(cursor_fwd);
                    ROUTE_LOG_DBG("      wire: %s (curr %d hist %f)\n", ctx->nameOfWire(cursor_fwd),
                                  int(wd.bound_nets.size()) - 1, wd.hist_cong_cost);
                }
            }
            NPNR_ASSERT(cursor_fwd == dst_wire);
            t.processed_sinks.insert(dst_wire);
            return ARC_SUCCESS;
        }

        // Normal forwards A* routing
        t.visited.clear();
        WireScore base_score;
        base_score.cost = 0;
        base_score.delay = ctx->getWireDelay(src_wire).maxDelay();
        base_score.togo_cost = get_togo_cost(net, i, src_wire, dst_wire);

        // Add source wire to queue
        t.queue.push(QueuedWire(src_wire, PipId(), Loc(), base_score));
        t.visited[src_wire].score = base_score;
        t.visited[src_wire].pip = PipId();

        int toexplore = 25000 * std::max(1, (ad.bb.x1 - ad.bb.x0) + (ad.bb.y1 - ad.bb.y0));
        int iter = 0;
        int explored = 1;
        bool debug_arc = /*usr.cell->type.str(ctx).find("RAMB") != std::string::npos && (usr.port ==
                            ctx->id("ADDRATIEHIGH0") || usr.port == ctx->id("ADDRARDADDRL0"))*/
                false;
        while (!t.queue.empty() && (!is_bb || iter < toexplore)) {
            auto curr = t.queue.top();
            t.queue.pop();
            ++iter;
#if 0
            ROUTE_LOG_DBG("current wire %s\n", ctx->nameOfWire(curr.wire));
#endif
            // Explore all pips downhill of cursor
            for (auto dh : ctx->getPipsDownhill(curr.wire)) {
                // Skip pips outside of box in bounding-box mode
#if 0
                ROUTE_LOG_DBG("trying pip %s\n", ctx->nameOfPip(dh));
#endif
#if 0
                int wire_intent = ctx->wireIntent(curr.wire);
                if (is_bb && !hit_test_pip(ad.bb, ctx->getPipLocation(dh)) && wire_intent != ID_PSEUDO_GND && wire_intent != ID_PSEUDO_VCC)
                    continue;
#else
                if (is_bb && !hit_test_pip(ad.bb, ctx->getPipLocation(dh)))
                    continue;
                if (!ctx->checkPipAvail(dh) && ctx->getBoundPipNet(dh) != net)
                    continue;
#endif
                // Evaluate score of next wire
                WireId next = ctx->getPipDstWire(dh);
#if 1
                if (debug_arc)
                    ROUTE_LOG_DBG("   src wire %s\n", ctx->nameOfWire(next));
#endif
                auto &nwd = wires.at(next);
                if (nwd.unavailable)
                    continue;
                if (nwd.reserved_net != -1 && nwd.reserved_net != net->udata)
                    continue;
                if (nwd.bound_nets.count(net->udata) && nwd.bound_nets.at(net->udata).second != dh)
                    continue;
                WireScore next_score;
                next_score.cost = curr.score.cost + score_wire_for_arc(net, i, next, dh);
                next_score.delay =
                        curr.score.delay + ctx->getPipDelay(dh).maxDelay() + ctx->getWireDelay(next).maxDelay();
                next_score.togo_cost = 1.75 * get_togo_cost(net, i, next, dst_wire);
                if (!t.visited.count(next) || (t.visited.at(next).score.total() > next_score.total())) {
                    ++explored;
#if 0
                    ROUTE_LOG_DBG("exploring wire %s cost %f togo %f\n", ctx->nameOfWire(next), next_score.cost,
                                  next_score.togo_cost);
#endif
                    // Add wire to queue if it meets criteria
                    t.queue.push(QueuedWire(next, dh, ctx->getPipLocation(dh), next_score, ctx->rng()));
                    t.visited[next].score = next_score;
                    t.visited[next].pip = dh;
                    if (next == dst_wire) {
                        toexplore = std::min(toexplore, iter + 5);
                    }
                }
            }
        }
        if (t.visited.count(dst_wire)) {
            ROUTE_LOG_DBG("   Routed (explored %d wires): ", explored);
            WireId cursor_bwd = dst_wire;
            while (t.visited.count(cursor_bwd)) {
                auto &v = t.visited.at(cursor_bwd);
                bind_pip_internal(net, i, cursor_bwd, v.pip);
                if (ctx->debug) {
                    auto &wd = wires.at(cursor_bwd);
                    ROUTE_LOG_DBG("      wire: %s (curr %d hist %f share %d)\n", ctx->nameOfWire(cursor_bwd),
                                  int(wd.bound_nets.size()) - 1, wd.hist_cong_cost,
                                  wd.bound_nets.count(net->udata) ? wd.bound_nets.at(net->udata).first : 0);
                }
                if (v.pip == PipId()) {
                    NPNR_ASSERT(cursor_bwd == src_wire);
                    break;
                }
                ROUTE_LOG_DBG("         pip: %s (%d, %d)\n", ctx->nameOfPip(v.pip), ctx->getPipLocation(v.pip).x,
                              ctx->getPipLocation(v.pip).y);
                cursor_bwd = ctx->getPipSrcWire(v.pip);
            }
            t.processed_sinks.insert(dst_wire);
            return ARC_SUCCESS;
        } else {
            return ARC_RETRY_WITHOUT_BB;
        }
    }
#undef ARC_ERR

    bool route_net(ThreadContext &t, NetInfo *net, bool is_mt)
    {

#ifdef ARCH_ECP5
        if (net->is_global)
            return true;
#endif

        ROUTE_LOG_DBG("Routing net '%s'...\n", ctx->nameOf(net));

        // Nothing to do if net is undriven
        if (net->driver.cell == nullptr)
            return true;

        bool have_failures = false;
        t.processed_sinks.clear();
        t.route_arcs.clear();
        for (size_t i = 0; i < net->users.size(); i++) {
            // Ripup failed arcs to start with
            // Check if arc is already legally routed
            if (check_arc_routing(net, i))
                continue;
            auto &usr = net->users.at(i);
            WireId dst_wire = ctx->getNetinfoSinkWire(net, usr);
            // Case of arcs that were pre-routed strongly (e.g. clocks)
            if (net->wires.count(dst_wire) && net->wires.at(dst_wire).strength > STRENGTH_STRONG)
                return ARC_SUCCESS;
            // Ripup arc to start with
            ripup_arc(net, i);
            t.route_arcs.push_back(i);
        }
        for (auto i : t.route_arcs) {
            auto res1 = route_arc(t, net, i, is_mt, true);
            if (res1 == ARC_FATAL)
                return false; // Arc failed irrecoverably
            else if (res1 == ARC_RETRY_WITHOUT_BB) {
                if (is_mt) {
                    // Can't break out of bounding box in multi-threaded mode, so mark this arc as a failure
                    have_failures = true;
                } else {
                    // Attempt a re-route without the bounding box constraint
                    ROUTE_LOG_DBG("Rerouting arc %d of net '%s' without bounding box, possible tricky routing...\n",
                                  int(i), ctx->nameOf(net));
                    auto res2 = route_arc(t, net, i, is_mt, false);
                    // If this also fails, no choice but to give up
                    if (res2 != ARC_SUCCESS)
                        log_error("Failed to route arc %d of net '%s', from %s to %s.\n", int(i), ctx->nameOf(net),
                                  ctx->nameOfWire(ctx->getNetinfoSourceWire(net)),
                                  ctx->nameOfWire(ctx->getNetinfoSinkWire(net, net->users.at(i))));
                }
            }
        }
        return !have_failures;
    }
#undef ROUTE_LOG_DBG

    int total_wire_use = 0;
    int overused_wires = 0;
    int total_overuse = 0;
    std::vector<int> route_queue;
    std::set<int> failed_nets;

    void update_congestion()
    {
        total_overuse = 0;
        overused_wires = 0;
        total_wire_use = 0;
        failed_nets.clear();
        for (auto &wire : wires) {
            total_wire_use += int(wire.second.bound_nets.size());
            int overuse = int(wire.second.bound_nets.size()) - 1;
            if (overuse > 0) {
                wire.second.hist_cong_cost += overuse * hist_cong_weight;
                total_overuse += overuse;
                overused_wires += 1;
                for (auto &bound : wire.second.bound_nets)
                    failed_nets.insert(bound.first);
            }
        }
    }

    bool bind_and_check(NetInfo *net, int usr_idx)
    {
#ifdef ARCH_ECP5
        if (net->is_global)
            return true;
#endif
        bool success = true;
        auto &nd = nets.at(net->udata);
        auto &ad = nd.arcs.at(usr_idx);
        auto &usr = net->users.at(usr_idx);
        WireId src = ctx->getNetinfoSourceWire(net);
        // Skip routes with no source
        if (src == WireId())
            return true;
        WireId dst = ctx->getNetinfoSinkWire(net, usr);
        // Skip routes where the destination is already bound
        if (dst == WireId() || ctx->getBoundWireNet(dst) == net)
            return true;
        // Skip routes where there is no routing (special cases)
        if (ad.wires.empty())
            return true;

        WireId cursor = dst;

        std::vector<PipId> to_bind;

        while (cursor != src) {
            if (!ctx->checkWireAvail(cursor)) {
                if (ctx->getBoundWireNet(cursor) == net)
                    break; // hit the part of the net that is already bound
                else {
                    success = false;
                    break;
                }
            }
            if (!ad.wires.count(cursor)) {
                log("Failure details:\n");
                log("    Cursor: %s\n", ctx->nameOfWire(cursor));
                log("    route backtrace: \n");
                for (auto w : ad.wires)
                    log("        %s: %s (src: %s)\n", ctx->nameOfWire(w.first), ctx->nameOfPip(w.second),
                        ctx->nameOfWire(ctx->getPipSrcWire(w.second)));
                log_error("Internal error; incomplete route tree for arc %d of net %s.\n", usr_idx, ctx->nameOf(net));
            }
            auto &p = ad.wires.at(cursor);
            if (!ctx->checkPipAvail(p)) {
                success = false;
                break;
            } else {
                to_bind.push_back(p);
            }
            cursor = ctx->getPipSrcWire(p);
        }

        if (success) {
            if (ctx->getBoundWireNet(src) == nullptr)
                ctx->bindWire(src, net, STRENGTH_WEAK);
            for (auto tb : to_bind)
                ctx->bindPip(tb, net, STRENGTH_WEAK);
        } else {
            ripup_arc(net, usr_idx);
            failed_nets.insert(net->udata);
        }
        return success;
    }

    int arch_fail = 0;
    bool bind_and_check_all()
    {
        bool success = true;
        std::vector<WireId> net_wires;
        for (auto net : nets_by_udata) {
#ifdef ARCH_ECP5
            if (net->is_global)
                continue;
#endif
            // Ripup wires and pips used by the net in nextpnr's structures
            net_wires.clear();
            for (auto &w : net->wires) {
                if (w.second.strength <= STRENGTH_STRONG)
                    net_wires.push_back(w.first);
            }
            for (auto w : net_wires)
                ctx->unbindWire(w);
            // Bind the arcs using the routes we have discovered
            for (size_t i = 0; i < net->users.size(); i++) {
                if (!bind_and_check(net, i)) {
                    ++arch_fail;
                    success = false;
                }
            }
        }
        return success;
    }

    void write_heatmap(std::ostream &out, bool congestion = false)
    {
        std::vector<std::vector<int>> hm_xy;
        int max_x = 0, max_y = 0;
        for (auto &w : wires) {
            auto &wd = w.second;
            int val = int(wd.bound_nets.size()) - (congestion ? 1 : 0);
            if (wd.bound_nets.empty())
                continue;
            // Estimate wire location by driving pip location
            PipId drv;
            for (auto &bn : wd.bound_nets)
                if (bn.second.second != PipId()) {
                    drv = bn.second.second;
                    break;
                }
            if (drv == PipId())
                continue;
            Loc l = ctx->getPipLocation(drv);
            max_x = std::max(max_x, l.x);
            max_y = std::max(max_y, l.y);
            if (l.y >= int(hm_xy.size()))
                hm_xy.resize(l.y + 1);
            if (l.x >= int(hm_xy.at(l.y).size()))
                hm_xy.at(l.y).resize(l.x + 1);
            if (val > 0)
                hm_xy.at(l.y).at(l.x) += val;
        }
        for (int y = 0; y <= max_y; y++) {
            for (int x = 0; x <= max_x; x++) {
                if (y >= int(hm_xy.size()) || x >= int(hm_xy.at(y).size()))
                    out << "0,";
                else
                    out << hm_xy.at(y).at(x) << ",";
            }
            out << std::endl;
        }
    }
    int mid_x = 0, mid_y = 0;

    void partition_nets()
    {
        // Create a histogram of positions in X and Y positions
        std::map<int, int> cxs, cys;
        for (auto &n : nets) {
            if (n.cx != -1)
                ++cxs[n.cx];
            if (n.cy != -1)
                ++cys[n.cy];
        }
        // 4-way split for now
        int accum_x = 0, accum_y = 0;
        int halfway = int(nets.size()) / 2;
        for (auto &p : cxs) {
            if (accum_x < halfway && (accum_x + p.second) >= halfway)
                mid_x = p.first;
            accum_x += p.second;
        }
        for (auto &p : cys) {
            if (accum_y < halfway && (accum_y + p.second) >= halfway)
                mid_y = p.first;
            accum_y += p.second;
        }
        log_info("x splitpoint: %d\n", mid_x);
        log_info("y splitpoint: %d\n", mid_y);
        std::vector<int> bins(5, 0);
        for (auto &n : nets) {
            if (n.bb.x0 < mid_x && n.bb.x1 < mid_x && n.bb.y0 < mid_y && n.bb.y1 < mid_y)
                ++bins[0]; // TL
            else if (n.bb.x0 >= mid_x && n.bb.x1 >= mid_x && n.bb.y0 < mid_y && n.bb.y1 < mid_y)
                ++bins[1]; // TR
            else if (n.bb.x0 < mid_x && n.bb.x1 < mid_x && n.bb.y0 >= mid_y && n.bb.y1 >= mid_y)
                ++bins[2]; // BL
            else if (n.bb.x0 >= mid_x && n.bb.x1 >= mid_x && n.bb.y0 >= mid_y && n.bb.y1 >= mid_y)
                ++bins[3]; // BR
            else
                ++bins[4]; // cross-boundary
        }
        for (int i = 0; i < 5; i++)
            log_info("bin %d N=%d\n", i, bins[i]);
    }

    void router_thread(ThreadContext &t)
    {
        for (auto n : t.route_nets) {
            bool result = route_net(t, n, true);
            if (!result)
                t.failed_nets.push_back(n);
        }
    }

    void do_route()
    {
        // Don't multithread if fewer than 200 nets (heuristic)
        if (route_queue.size() < 200) {
            ThreadContext st;
            for (size_t j = 0; j < route_queue.size(); j++) {
                route_net(st, nets_by_udata[route_queue[j]], false);
            }
            return;
        }
        const int N = 4;
        std::vector<ThreadContext> tcs(N + 1);
        for (auto n : route_queue) {
            auto &nd = nets.at(n);
            auto ni = nets_by_udata.at(n);
            int bin = N;
            int le_x = mid_x - bb_margin_x;
            int rs_x = mid_x + bb_margin_x;
            int le_y = mid_y - bb_margin_y;
            int rs_y = mid_y + bb_margin_y;

            if (nd.bb.x0 < le_x && nd.bb.x1 < le_x && nd.bb.y0 < le_y && nd.bb.y1 < le_y)
                bin = 0;
            else if (nd.bb.x0 >= rs_x && nd.bb.x1 >= rs_x && nd.bb.y0 < le_y && nd.bb.y1 < le_y)
                bin = 1;
            else if (nd.bb.x0 < le_x && nd.bb.x1 < le_x && nd.bb.y0 >= rs_y && nd.bb.y1 >= rs_y)
                bin = 2;
            else if (nd.bb.x0 >= rs_x && nd.bb.x1 >= rs_x && nd.bb.y0 >= rs_y && nd.bb.y1 >= rs_y)
                bin = 3;
            tcs.at(bin).route_nets.push_back(ni);
        }
        log_info("%d/%d nets not multi-threadable\n", int(tcs.at(N).route_nets.size()), int(route_queue.size()));
        // Multithreaded part of routing
        std::vector<std::thread> threads;
        for (int i = 0; i < N; i++) {
            threads.emplace_back([this, &tcs, i]() { router_thread(tcs.at(i)); });
        }
        for (int i = 0; i < N; i++)
            threads.at(i).join();
        // Singlethreaded part of routing - nets that cross partitions
        // or don't fit within bounding box
        for (auto st_net : tcs.at(N).route_nets)
            route_net(tcs.at(N), st_net, false);
        // Failed nets
        for (int i = 0; i < N; i++)
            for (auto fail : tcs.at(i).failed_nets)
                route_net(tcs.at(N), fail, false);
    }

    void router_test()
    {
        setup_nets();
        setup_wires();
        find_all_reserved_wires();
        partition_nets();
        curr_cong_weight = 0.5;
        hist_cong_weight = 1.0;
        ThreadContext st;
        int iter = 1;

        for (size_t i = 0; i < nets_by_udata.size(); i++)
            route_queue.push_back(i);

        do {
            ctx->sorted_shuffle(route_queue);
#if 0
            for (size_t j = 0; j < route_queue.size(); j++) {
                route_net(st, nets_by_udata[route_queue[j]], false);
                if ((j % 1000) == 0 || j == (route_queue.size() - 1))
                    log("    routed %d/%d\n", int(j), int(route_queue.size()));
            }
#endif
            do_route();
            route_queue.clear();
            update_congestion();
#if 1
            if (iter == 1 && ctx->debug) {
                std::ofstream cong_map("cong_map_0.csv");
                write_heatmap(cong_map, true);
            }
#endif
            if (overused_wires == 0) {
                // Try and actually bind nextpnr Arch API wires
                bind_and_check_all();
            }
            for (auto cn : failed_nets)
                route_queue.push_back(cn);
            log_info("iter=%d wires=%d overused=%d overuse=%d archfail=%s\n", iter, total_wire_use, overused_wires,
                     total_overuse, overused_wires > 0 ? "NA" : std::to_string(arch_fail).c_str());
            ++iter;
            curr_cong_weight *= 2;
        } while (!failed_nets.empty());
    }
};
} // namespace

void router2_test(Context *ctx)
{
    Router2 rt;
    rt.ctx = ctx;
    rt.router_test();
}

NEXTPNR_NAMESPACE_END