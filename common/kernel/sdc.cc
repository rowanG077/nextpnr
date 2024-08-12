/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2020  gatecat <gatecat@ds0.me>
 *  Copyright (C) 2024  rowanG077 <goemansrowan@gmail.com>
 *
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

#include "log.h"
#include "nextpnr.h"

#include <algorithm>
#include <iterator>

NEXTPNR_NAMESPACE_BEGIN

struct TclEntity
{
    enum EntityType
    {
        ENTITY_CELL,
        ENTITY_PORT,
        ENTITY_NET,
        ENTITY_PIN,
    } type;
    IdString name;
    IdString pin; // for cell pins only

    TclEntity(EntityType type, IdString name) : type(type), name(name) {}
    TclEntity(EntityType type, IdString name, IdString pin) : type(type), name(name), pin(pin) {}

    const std::string &to_string(Context *ctx) { return name.str(ctx); }

    CellInfo *get_cell(Context *ctx) const
    {
        if (type != ENTITY_CELL)
            return nullptr;
        return ctx->cells.at(name).get();
    }

    PortInfo *get_port(Context *ctx) const
    {
        if (type != ENTITY_PORT)
            return nullptr;
        return &ctx->ports.at(name);
    }

    NetInfo *get_net(Context *ctx) const
    {
        if (type == ENTITY_PIN) {
            CellInfo *cell = nullptr;
            if (ctx->cells.count(name)) {
                cell = ctx->cells.at(name).get();
            } else {
                const std::string &n = name.str(ctx);
                auto pos = n.rfind('.');
                if (pos == std::string::npos)
                    return nullptr;
                // remove one hierarchy layer due to Radiant weirdness around PLLs etc
                IdString stripped_name = ctx->id(n.substr(0, pos));
                if (!ctx->cells.count(stripped_name))
                    return nullptr;
                cell = ctx->cells.at(stripped_name).get();
            }
            if (!cell->ports.count(pin))
                return nullptr;
            return cell->ports.at(pin).net;
        } else if (type == ENTITY_NET) {
            return ctx->nets.at(name).get();
        } else {
            return nullptr;
        }
    }
};

struct TclValue
{
    TclValue(const std::string &s) : is_string(true), str(s) {};
    TclValue(const std::vector<TclEntity> &l) : is_string(false), list(l) {};

    bool is_string;
    std::string str;             // simple string value
    std::vector<TclEntity> list; // list of entities
};

struct SDCParser
{
    std::string buf;
    int pos = 0;
    int lineno = 1;
    Context *ctx;

    SDCParser(const std::string &buf, Context *ctx) : buf(buf), ctx(ctx) {};

    inline bool eof() const { return pos == int(buf.size()); }

    inline char peek() const { return buf.at(pos); }

    inline char get()
    {
        char c = buf.at(pos++);
        if (c == '\n')
            ++lineno;
        return c;
    }

    std::string get(int n)
    {
        std::string s = buf.substr(pos, n);
        pos += n;
        return s;
    }

    // If next char matches c, take it from the stream and return true
    bool check_get(char c)
    {
        if (peek() == c) {
            get();
            return true;
        } else {
            return false;
        }
    }

    // If next char matches any in chars, take it from the stream and return true
    bool check_get_any(const std::string &chrs)
    {
        char c = peek();
        if (chrs.find(c) != std::string::npos) {
            get();
            return true;
        } else {
            return false;
        }
    }

    inline void skip_blank(bool nl = false)
    {
        while (!eof() && check_get_any(nl ? " \t\n\r" : " \t"))
            ;
    }

    // Return true if end of line (or file)
    inline bool skip_check_eol()
    {
        skip_blank(false);
        if (eof())
            return true;
        char c = peek();
        // Comments count as end of line
        if (c == '#') {
            get();
            while (!eof() && peek() != '\n' && peek() != '\r')
                get();
            return true;
        }
        if (c == ';') {
            // Forced end of line
            get();
            return true;
        }
        return (c == '\n' || c == '\r');
    }

    inline std::string get_str()
    {
        std::string s;
        skip_blank(false);
        if (eof())
            return "";

        bool in_quotes = false, in_braces = false, escaped = false;

        char c = get();

        if (c == '"')
            in_quotes = true;
        else if (c == '{')
            in_braces = true;
        else
            s += c;

        while (true) {
            char c = peek();
            if (!in_quotes && !in_braces && !escaped && (std::isblank(c) || c == ']')) {
                break;
            }
            get();
            if (escaped) {
                s += c;
                escaped = false;
            } else if ((in_quotes && c == '"') || (in_braces && c == '}')) {
                break;
            } else if (c == '\\') {
                escaped = true;
            } else {
                s += c;
            }
        }

        return s;
    }

    TclValue evaluate(const std::vector<TclValue> &arguments)
    {
        NPNR_ASSERT(!arguments.empty());
        auto &arg0 = arguments.at(0);
        NPNR_ASSERT(arg0.is_string);
        const std::string &cmd = arg0.str;
        if (cmd == "get_ports")
            return cmd_get_ports(arguments);
        else if (cmd == "get_cells")
            return cmd_get_cells(arguments);
        else if (cmd == "get_nets")
            return cmd_get_nets(arguments);
        else if (cmd == "get_pins")
            return cmd_get_pins(arguments);
        else if (cmd == "create_clock")
            return cmd_create_clock(arguments);
        else
            log_error("Unsupported SDC command '%s'\n", cmd.c_str());
    }

    std::vector<TclValue> get_arguments()
    {
        std::vector<TclValue> args;
        while (!skip_check_eol()) {
            if (check_get('[')) {
                // Start of a sub-expression
                auto result = evaluate(get_arguments());
                NPNR_ASSERT(check_get(']'));
                args.push_back(result);
            } else if (peek() == ']') {
                break;
            } else {
                args.push_back(get_str());
            }
        }
        skip_blank(true);
        return args;
    }

    TclValue cmd_get_nets(const std::vector<TclValue> &arguments)
    {
        std::vector<TclEntity> nets;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("get_nets expected string arguments (line %d)\n", lineno);
            std::string s = arg.str;
            if (s.at(0) == '-')
                log_error("unsupported argument '%s' to get_nets (line %d)\n", s.c_str(), lineno);
            IdString id = ctx->id(s);
            if (ctx->nets.count(id) || ctx->net_aliases.count(id))
                nets.emplace_back(TclEntity::ENTITY_NET, ctx->net_aliases.count(id) ? ctx->net_aliases.at(id) : id);
            else
                log_warning("get_nets argument '%s' matched no objects.\n", s.c_str());
        }
        return nets;
    }

    TclValue cmd_get_ports(const std::vector<TclValue> &arguments)
    {
        std::vector<TclEntity> ports;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("get_ports expected string arguments (line %d)\n", lineno);
            std::string s = arg.str;
            if (s.at(0) == '-')
                log_error("unsupported argument '%s' to get_ports (line %d)\n", s.c_str(), lineno);
            IdString id = ctx->id(s);
            if (ctx->ports.count(id))
                ports.emplace_back(TclEntity::ENTITY_PORT, id);
        }
        return ports;
    }

    TclValue cmd_get_cells(const std::vector<TclValue> &arguments)
    {
        std::vector<TclEntity> cells;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("get_cells expected string arguments (line %d)\n", lineno);
            std::string s = arg.str;
            if (s.at(0) == '-')
                log_error("unsupported argument '%s' to get_cells (line %d)\n", s.c_str(), lineno);
            IdString id = ctx->id(s);
            if (ctx->cells.count(id))
                cells.emplace_back(TclEntity::ENTITY_CELL, id);
        }
        return cells;
    }

    TclValue cmd_get_pins(const std::vector<TclValue> &arguments)
    {
        std::vector<TclEntity> pins;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (!arg.is_string)
                log_error("get_pins expected string arguments (line %d)\n", lineno);
            std::string s = arg.str;
            if (s.at(0) == '-')
                log_error("unsupported argument '%s' to get_pins (line %d)\n", s.c_str(), lineno);
            auto pos = s.rfind('/');
            if (pos == std::string::npos)
                log_error("expected / in cell pin name '%s' (line %d)\n", s.c_str(), lineno);
            pins.emplace_back(TclEntity::ENTITY_PIN, ctx->id(s.substr(0, pos)), ctx->id(s.substr(pos + 1)));
            if (pins.back().get_net(ctx) == nullptr) {
                log_warning("cell pin '%s' not found\n", s.c_str());
                pins.pop_back();
            }
        }
        return pins;
    }

    TclValue cmd_create_clock(const std::vector<TclValue> &arguments)
    {
        float period = 10;
        for (int i = 1; i < int(arguments.size()); i++) {
            auto &arg = arguments.at(i);
            if (arg.is_string) {
                std::string s = arg.str;
                if (s == "-period") {
                    i++;
                    auto &val = arguments.at(i);
                    if (!val.is_string)
                        log_error("expecting string argument to -period (line %d)\n", lineno);
                    try {
                        period = std::stof(val.str);
                    } catch (std::exception &e) {
                        log_error("invalid argument '%s' to -period (line %d)\n", val.str.c_str(), lineno);
                    }
                } else if (s == "-name") {
                    i++;
                } else {
                    log_error("unsupported argument '%s' to create_clock\n", s.c_str());
                }
            } else {
                for (const auto &ety : arg.list) {
                    NetInfo *net = nullptr;
                    if (ety.type == TclEntity::ENTITY_PIN)
                        net = ety.get_net(ctx);
                    else if (ety.type == TclEntity::ENTITY_NET)
                        net = ctx->nets.at(ety.name).get();
                    else if (ety.type == TclEntity::ENTITY_PORT)
                        net = ctx->ports.at(ety.name).net;
                    else
                        log_error("create_clock applies only to cells, cell pins, or IO ports (line %d)\n", lineno);
                    ctx->addClock(net->name, 1000.0f / period);
                }
            }
        }
        return std::string{};
    }

    void operator()()
    {
        while (!eof()) {
            skip_blank(true);
            auto args = get_arguments();
            if (args.empty())
                continue;
            evaluate(args);
        }
    }
};

void Context::read_sdc(std::istream &in)
{
    std::string buf(std::istreambuf_iterator<char>(in), {});
    SDCParser(buf, getCtx())();
}

NEXTPNR_NAMESPACE_END