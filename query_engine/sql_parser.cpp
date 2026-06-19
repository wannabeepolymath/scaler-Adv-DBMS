// Lab 7: Minimal SQL SELECT parser/executor (implementation).
//   The WHERE clause is handed to the shunting-yard module, converted to RPN
//   once, then evaluated per row against a variable map built from the columns.
#include "sql_parser.h"
#include "shunting_yard.h"

#include <sstream>
#include <algorithm>
#include <iostream>
#include <cctype>

namespace sql {

double row_val(const Row& row, const std::string& col) {
    auto it = row.cols.find(col);
    if (it == row.cols.end()) return 0.0;
    if (auto* d = std::get_if<double>(&it->second)) return *d;
    if (auto* s = std::get_if<std::string>(&it->second)) {
        try { return std::stod(*s); } catch (...) {}
    }
    return 0.0;
}

static std::string to_upper(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

// ───────────────────────────── parser ─────────────────────────────

SelectQuery parse_select(const std::string& sql) {
    SelectQuery q;
    std::istringstream ss(sql);
    std::string word;
    ss >> word;                                     // SELECT

    while (ss >> word && to_upper(word) != "FROM") {  // column list until FROM
        if (!word.empty() && word.back() == ',') word.pop_back();
        if (word == "*") q.columns.clear();
        else             q.columns.push_back(word);
    }
    ss >> q.from;                                   // table name

    while (ss >> word) {                            // optional clauses
        std::string kw = to_upper(word);
        if (kw == "WHERE") {
            std::string clause, w2;
            while (ss >> w2) {
                if (to_upper(w2) == "ORDER" || to_upper(w2) == "LIMIT") { word = w2; goto next_clause; }
                clause += (clause.empty() ? "" : " ") + w2;
            }
            q.where_raw = clause;
            break;
        next_clause:
            q.where_raw = clause;
            kw = to_upper(word);
        }
        if (kw == "ORDER") {
            ss >> word;                             // BY
            ss >> q.order_by;
            std::string dir;
            if (ss >> dir && to_upper(dir) == "DESC") q.order_asc = false;
        }
        if (kw == "LIMIT") ss >> q.limit;
    }
    return q;
}

// ───────────────────────────── executor ─────────────────────────────

std::vector<Row> execute(const SelectQuery& q, const std::vector<Row>& data) {
    std::vector<std::string> rpn;
    if (!q.where_raw.empty()) rpn = sy::to_rpn(sy::tokenize(q.where_raw));

    std::vector<Row> result;
    for (const auto& row : data) {
        if (!rpn.empty()) {                         // filter: WHERE
            std::unordered_map<std::string, double> vars;
            for (auto& [k, v] : row.cols) vars[k] = row_val(row, k);
            if (!sy::eval_rpn(rpn, vars)) continue;
        }
        if (q.columns.empty()) {                    // project: SELECT *
            result.push_back(row);
        } else {                                    // project: named columns
            Row projected;
            for (auto& col : q.columns)
                if (row.cols.count(col)) projected.cols[col] = row.cols.at(col);
            result.push_back(projected);
        }
    }

    if (!q.order_by.empty()) {                      // sort: ORDER BY
        std::sort(result.begin(), result.end(), [&](const Row& a, const Row& b) {
            double va = row_val(a, q.order_by), vb = row_val(b, q.order_by);
            return q.order_asc ? va < vb : va > vb;
        });
    }
    if (q.limit >= 0 && (int)result.size() > q.limit) result.resize(q.limit);  // LIMIT
    return result;
}

void print_rows(const std::vector<Row>& rows) {
    if (rows.empty()) { std::cout << "  (0 rows)\n"; return; }
    for (const auto& row : rows) {
        std::cout << "  ";
        for (const auto& [k, v] : row.cols) {
            std::cout << k << "=";
            if (auto* d = std::get_if<double>(&v))      std::cout << *d;
            if (auto* s = std::get_if<std::string>(&v)) std::cout << *s;
            std::cout << "  ";
        }
        std::cout << "\n";
    }
}

} // namespace sql
