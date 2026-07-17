#ifndef ATOMDB_RESULT_HPP
#define ATOMDB_RESULT_HPP

#include <string>
#include <vector>
#include <iostream>

#include "atomdb/types/Tuple.hpp"

namespace atomdb {

// ResultSet (spec §3.2): the return shape of a query. Carries a `success` flag
// alongside row data so front-ends can distinguish "empty success" from "error"
// without consulting a side-channel DbError.
class ResultSet {
public:
    bool success = true;
    std::vector<Tuple> rows;

    ResultSet() = default;
    ResultSet(bool ok, std::vector<Tuple> r)
        : success(ok), rows(std::move(r)) {}

    std::size_t size() const noexcept { return rows.size(); }
    bool empty() const noexcept { return rows.empty(); }

    // Simple ASCII table rendering; columns union across all rows (in encounter
    // order). Empty result set renders a "(no rows)" line.
    std::string toString() const {
        if (rows.empty()) {
            return std::string("(no rows)") + "\n";
        }
        // Collect column names in first-row encounter order; subsequent rows that
        // introduce new columns append them. In v0.1 the InMemory engine returns
        // homogeneous-typed rows so this is generally the first-row ordering only.
        std::vector<std::string> headers;
        for (const auto& cv : rows[0].columns()) headers.push_back(cv.name);
        // Compute column widths.
        std::vector<std::size_t> widths(headers.size(), 0);
        for (std::size_t i = 0; i < headers.size(); ++i) {
            widths[i] = headers[i].size();
        }
        std::vector<std::vector<std::string>> rendered;
        rendered.reserve(rows.size());
        for (const auto& row : rows) {
            std::vector<std::string> cells(headers.size(), "NULL");
            for (const auto& cv : row.columns()) {
                // find header index (linear; small v0.1)
                std::size_t idx = 0;
                while (idx < headers.size() && headers[idx] != cv.name) ++idx;
                if (idx < headers.size()) {
                    cells[idx] = cv.value.toString();
                    if (cells[idx].size() > widths[idx]) widths[idx] = cells[idx].size();
                }
            }
            rendered.push_back(std::move(cells));
        }
        std::string out;
        // Header row
        out += "| ";
        for (std::size_t i = 0; i < headers.size(); ++i) {
            if (i) out += " | ";
            out += headers[i];
            if (headers[i].size() < widths[i]) out += std::string(widths[i] - headers[i].size(), ' ');
        }
        out += " |\n";
        // Separator
        out += "|-";
        for (std::size_t i = 0; i < headers.size(); ++i) {
            if (i) out += "-+-";
            out += std::string(widths[i], '-');
        }
        out += "-|\n";
        // Data rows
        for (const auto& cells : rendered) {
            out += "| ";
            for (std::size_t i = 0; i < cells.size(); ++i) {
                if (i) out += " | ";
                out += cells[i];
                if (cells[i].size() < widths[i]) out += std::string(widths[i] - cells[i].size(), ' ');
            }
            out += " |\n";
        }
        return out;
    }
};

} // namespace atomdb

#endif // ATOMDB_RESULT_HPP
