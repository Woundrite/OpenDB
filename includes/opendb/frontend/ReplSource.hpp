#ifndef OPENDB_REPL_SOURCE_HPP
#define OPENDB_REPL_SOURCE_HPP

#include <cctype>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "opendb/contracts/ICommandSource.hpp"
#include "opendb/types/Command.hpp"
#include "opendb/types/Predicate.hpp"
#include "opendb/types/Tuple.hpp"
#include "opendb/types/Value.hpp"

namespace opendb {

// ReplSource: trivial line-based REPL front-end (milestone 4, spec §4.1).
//
// Supported commands (intentionally a tiny subset to satisfy the spec without
// a full SQL parser; a real SQL parser is a planned milestone 8):
//
//   INSERT <table> {<col>:<val>,<col>:<val>,...}        e.g. INSERT users { key:1,name:nikhil,age:30 }
//   SELECT <table>                                        full table scan
//   SELECT <table> WHERE <col>=<val>                      predicate filter
//   EXIT                                                  end-of-input signal
//
// Values are parsed very lightly:
//   - Unsigned integer literal -> Value::int64
//   - Quoted string "..." or '...' -> Value::text
//   - true / false -> Value::boolean
//   - null -> Value::null
//   - otherwise dispatched -> Value::text (raw name)
//
// On parse error, the REPL presents a DbError via an injected sink and
// continues to the next line (does NOT exit). The exit happens only on EXIT.
class ReplSource : public ICommandSource {
public:
    explicit ReplSource(std::istream& in, std::ostream& out) : in_(in), out_(out) {}

    std::optional<Command> nextCommand() override {
        std::string line;
        if (!std::getline(in_, line)) return std::nullopt; // EOF

        // Trim whitespace.
        std::size_t first = 0;
        while (first < line.size() && std::isspace(static_cast<unsigned char>(line[first]))) ++first;
        std::size_t last = line.size();
        while (last > first && std::isspace(static_cast<unsigned char>(line[last - 1]))) --last;
        line = line.substr(first, last - first);
        if (line.empty()) return std::nullopt; // empty line -> pause (REPL caller retries)

        return parse(line);
    }

    void present(const ResultSet& rs) override {
        if (rs.rows.empty()) {
            out_ << (rs.success ? "[OK]\n" : "[FAIL]\n");
            return;
        }
        out_ << (rs.success ? "[OK] " : "[FAIL] ") << rs.toString();
    }

    void present(const DbError& err) override {
        out_ << "[ERR] " << err.toString() << "\n";
    }

private:
    std::istream& in_;
    std::ostream& out_;

    std::optional<Command> parse(const std::string& line) {
        std::istringstream iss(line);
        std::string verb;
        if (!(iss >> verb)) return std::nullopt;

        // Own a string so Command can keep a stable reference.
        std::string table;
        if (verb == "INSERT") {
            iss >> table;
            std::string tuple_lit;
            iss >> tuple_lit;
            // Support both space-separated ("{a:1,b:2}") and a literal that
            // occupies the whole remaining line.
            std::string trailing;
            std::getline(iss, trailing);
            if (!tuple_lit.empty() && tuple_lit.front() == '{' && tuple_lit.back() == '}') {
                Tuple t = parseTupleLit(tuple_lit);
                if (t.size() == 0) return std::nullopt; // parser already printed err
                return Command(CommandType::Insert,
                               std::move(table),
                               std::nullopt,
                               std::move(t));
            } else {
                // Tuple literal begins with '{' but spilled into subsequent
                // tokens: reattach from `tuple_lit` + `trailing`.
                std::string combined = tuple_lit + trailing;
                auto trim = [](std::string& s) {
                    std::size_t f = 0;
                    while (f < s.size() && std::isspace(static_cast<unsigned char>(s[f]))) ++f;
                    std::size_t l = s.size();
                    while (l > f && std::isspace(static_cast<unsigned char>(s[l - 1]))) --l;
                    s = s.substr(f, l - f);
                };
                trim(combined);
                Tuple t = parseTupleLit(combined);
                if (t.size() == 0) return std::nullopt;
                return Command(CommandType::Insert,
                               std::move(table),
                               std::nullopt,
                               std::move(t));
            }
        }
        if (verb == "SELECT") {
            iss >> table;
            std::string next;
            if (!(iss >> next)) {
                // Full scan.
                return Command(CommandType::Select, std::move(table));
            }
            if (next == "WHERE") {
                std::string pred;
                std::getline(iss, pred);
                auto parsed = parseEqualityPred(pred);
                return Command(CommandType::Select,
                               std::move(table),
                               std::move(parsed));
            }
            return Command(CommandType::Select, std::move(table));
        }
        if (verb == "EXIT") return std::nullopt;
        // Unknown verb: short-circuit by emitting an error to the source and
        // returning nullopt so the loop pauses. Caller invokes nextCommand()
        // again on the next line; we have no way to return an error from
        // nextCommand() that the loop would surface. Instead we report errors
        // here via a std::cerr one-off: per spec §5.4 v0.1 parser errors are
        // reported but the loop does not exit naively.
        std::cerr << "[ERR] Unknown verb: " << verb << "\n";
        return std::nullopt;
    }

    // Parse a literal like {key:1,name:"nikhil",age:30}
    static Tuple parseTupleLit(const std::string& lit) {
        Tuple t;
        if (lit.size() < 2 || lit.front() != '{' || lit.back() != '}') return t;
        std::string body = lit.substr(1, lit.size() - 2);
        // Split on commas (no nesting in v0.1).
        std::size_t start = 0;
        while (start <= body.size()) {
            std::size_t end = body.find(',', start);
            if (end == std::string::npos) end = body.size();
            std::string entry = body.substr(start, end - start);
            std::size_t colon = entry.find(':');
            if (colon == std::string::npos) {
                std::cerr << "[ERR] tuple entry '" << entry << "' missing ':'\n";
                return Tuple{}; // signal failure -> empty
            }
            std::string name = entry.substr(0, colon);
            std::string vlit = entry.substr(colon + 1);
            // trim leading/trailing spaces
            auto trim = [](std::string& s) {
                std::size_t f = 0;
                while (f < s.size() && std::isspace(static_cast<unsigned char>(s[f]))) ++f;
                std::size_t l = s.size();
                while (l > f && std::isspace(static_cast<unsigned char>(s[l - 1]))) --l;
                s = s.substr(f, l - f);
            };
            trim(name);
            trim(vlit);
            t.set(name, parseValueLit(vlit));
            start = end + 1;
        }
        return t;
    }

    static Value parseValueLit(const std::string& vlit) {
        if (vlit == "null")  return Value::null();
        if (vlit == "true")  return Value::boolean(true);
        if (vlit == "false") return Value::boolean(false);
        // Quoted string: "..." or '...'.
        if (vlit.size() >= 2 &&
            ((vlit.front() == '"' && vlit.back() == '"') ||
             (vlit.front() == '\'' && vlit.back() == '\''))) {
            return Value::text(vlit.substr(1, vlit.size() - 2));
        }
        // Integer literal?
        std::size_t consumed = 0;
        try {
            long long n = std::stoll(vlit, &consumed);
            if (consumed == vlit.size()) return Value::integer(n);
        } catch (...) {}
        return Value::text(vlit);
    }

    static std::optional<Predicate> parseEqualityPred(std::string pred) {
        // Trim.
        auto trim = [](std::string& s) {
            std::size_t f = 0;
            while (f < s.size() && std::isspace(static_cast<unsigned char>(s[f]))) ++f;
            std::size_t l = s.size();
            while (l > f && std::isspace(static_cast<unsigned char>(s[l - 1]))) --l;
            s = s.substr(f, l - f);
        };
        trim(pred);
        std::size_t eq = pred.find('=');
        if (eq == std::string::npos) return std::nullopt;
        std::string col = pred.substr(0, eq);
        std::string vlit = pred.substr(eq + 1);
        trim(col); trim(vlit);
        return Predicate(ComparisonNode::make(col, ComparisonOp::Equals,
                                               parseValueLit(vlit)));
    }
};

} // namespace opendb

#endif // OPENDB_REPL_SOURCE_HPP
