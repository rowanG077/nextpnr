/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  David Shah <david@symbioticeda.com>
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

#include "timing.h"
#include <algorithm>
#include <unordered_map>
#include <utility>
#include "log.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN

typedef std::vector<const PortRef*> PortRefVector;

struct Timing
{
    Context *ctx;
    bool update;
    delay_t min_slack;
    PortRefVector current_path;
    PortRefVector *crit_path;

    Timing(Context *ctx, bool update, PortRefVector *crit_path = nullptr): ctx(ctx), update(update), min_slack(1.0e12 / ctx->target_freq), crit_path(crit_path) {}

    delay_t follow_net(NetInfo *net, int path_length, delay_t slack)
    {
        delay_t net_budget = slack / (path_length + 1);
        for (auto &usr : net->users) {
            if (crit_path)
                current_path.push_back(&usr);
            // If budget override is less than existing budget, then do not increment path length
            int pl = path_length + 1;
            auto budget = ctx->getBudgetOverride(net, usr, net_budget);
            if (budget < net_budget) {
                net_budget = budget;
                pl = std::max(1, path_length);
            }
            auto delay = ctx->getNetinfoRouteDelay(net, usr);
            net_budget = std::min(
                    net_budget, follow_user_port(usr, pl, slack - delay));
            if (update)
                usr.budget = std::min(usr.budget, delay + net_budget);
            if (crit_path)
                current_path.pop_back();
        }
        return net_budget;
    }

    // Follow a path, returning budget to annotate
    delay_t follow_user_port(PortRef &user, int path_length, delay_t slack)
    {
        delay_t value;
        if (ctx->getPortClock(user.cell, user.port) != IdString()) {
            // At the end of a timing path (arguably, should check setup time
            // here too)
            value = slack / path_length;
            if (slack < min_slack) {
                min_slack = slack;
                if (crit_path)
                    *crit_path = current_path;
            }
        } else {
            // Default to the path ending here, if no further paths found
            value = slack / path_length;
            // Follow outputs of the user
            for (auto port : user.cell->ports) {
                if (port.second.type == PORT_OUT) {
                    DelayInfo comb_delay;
                    // Look up delay through this path
                    bool is_path = ctx->getCellDelay(user.cell, user.port, port.first, comb_delay);
                    if (is_path) {
                        NetInfo *net = port.second.net;
                        if (net) {
                            delay_t path_budget = follow_net(net, path_length, slack - comb_delay.maxDelay());
                            value = std::min(value, path_budget);
                        }
                    }
                }
            }
        }
        return value;
    }

    delay_t walk_paths()
    {
        delay_t default_slack = delay_t(1.0e12 / ctx->target_freq);
    
        // Go through all clocked drivers and distribute the available path
        //   slack evenly into the budget of every sink on the path
        for (auto &cell : ctx->cells) {
            for (auto port : cell.second->ports) {
                if (port.second.type == PORT_OUT) {
                    IdString clock_domain = ctx->getPortClock(cell.second.get(), port.first);
                    if (clock_domain != IdString()) {
                        delay_t slack = default_slack; // TODO: clock constraints
                        DelayInfo clkToQ;
                        if (ctx->getCellDelay(cell.second.get(), clock_domain, port.first, clkToQ))
                            slack -= clkToQ.maxDelay();
                        if (port.second.net)
                            follow_net(port.second.net, 0, slack);
                    }
                }
            }
        }
        return min_slack;
    }

    void assign_budget()
    {
        // Clear delays to a very high value first
        delay_t default_slack = delay_t(1.0e12 / ctx->target_freq);
        for (auto &net : ctx->nets) {
            for (auto &usr : net.second->users) {
                usr.budget = default_slack;
            }
        }

        walk_paths();
    }
};

void assign_budget(Context *ctx, bool quiet)
{
    if (!quiet) {
        log_break();
        log_info("Annotating ports with timing budgets\n");
    }

    Timing timing(ctx, true /* update */);
    timing.assign_budget();


    if (!quiet || ctx->verbose) {
        for (auto &net : ctx->nets) {
            for (auto &user : net.second->users) {
                // Post-update check
                if (ctx->user_freq && user.budget < 0)
                    log_warning("port %s.%s, connected to net '%s', has negative "
                                "timing budget of %fns\n",
                                user.cell->name.c_str(ctx), user.port.c_str(ctx), net.first.c_str(ctx),
                                ctx->getDelayNS(user.budget));
                else if (ctx->verbose)
                    log_info("port %s.%s, connected to net '%s', has "
                             "timing budget of %fns\n",
                             user.cell->name.c_str(ctx), user.port.c_str(ctx), net.first.c_str(ctx),
                             ctx->getDelayNS(user.budget));
            }
        }
    }

    // For slack redistribution, if user has not specified a frequency
    //   dynamically adjust the target frequency to be the currently
    //   achieved maximum
    if (!ctx->user_freq && ctx->slack_redist_iter > 0) {
        delay_t default_slack = delay_t(1.0e12 / ctx->target_freq);
        ctx->target_freq = 1e12 / (default_slack - timing.min_slack);
        if (ctx->verbose)
            log_info("minimum slack for this assign = %d, target Fmax for next update = %.2f MHz\n", timing.min_slack,
                     ctx->target_freq / 1e6);
    }

    if (!quiet)
        log_info("Checksum: 0x%08x\n", ctx->checksum());
}

void timing_analysis(Context *ctx, bool print_path)
{
    PortRefVector crit_path;

    Timing timing(ctx, false /* update */, &crit_path);
    auto min_slack = timing.walk_paths();

    if (print_path) {
        if (crit_path.empty()) {
            log_info("Design contains no timing paths\n");
        } else {
            delay_t total = 0;
            log_break();
            log_info("Critical path report:\n");
            log_info("curr total\n");

            auto &front = crit_path.front();
            auto &front_port = front->cell->ports.at(front->port);
            auto &front_driver = front_port.net->driver;
            auto last_port = ctx->getPortClock(front_driver.cell, front_driver.port);
            for (auto sink : crit_path) {
                auto sink_cell = sink->cell;
                auto &port = sink_cell->ports.at(sink->port);
                auto net = port.net;
                auto &driver = net->driver;
                auto driver_cell = driver.cell;
                DelayInfo comb_delay;
                ctx->getCellDelay(sink_cell, last_port, driver.port, comb_delay);
                total += comb_delay.maxDelay();
                log_info("%4d %4d  Source %s.%s\n", comb_delay.maxDelay(), total, driver_cell->name.c_str(ctx),
                         driver.port.c_str(ctx));
                auto net_delay = ctx->getNetinfoRouteDelay(net, *sink);
                total += net_delay;
                auto driver_loc = ctx->getBelLocation(driver_cell->bel);
                auto sink_loc = ctx->getBelLocation(sink_cell->bel);
                log_info("%4d %4d    Net %s budget %d (%d,%d) -> (%d,%d)\n", net_delay, total, net->name.c_str(ctx),
                         sink->budget, driver_loc.x, driver_loc.y, sink_loc.x, sink_loc.y);
                log_info("                Sink %s.%s\n", sink_cell->name.c_str(ctx), sink->port.c_str(ctx));
                last_port = sink->port;
            }
            log_break();
        }
    }

    delay_t default_slack = delay_t(1.0e12 / ctx->target_freq);
    log_info("estimated Fmax = %.2f MHz\n", 1e6 / (default_slack - min_slack));
}

NEXTPNR_NAMESPACE_END
