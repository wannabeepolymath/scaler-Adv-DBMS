// Lab 7: Minimal SQL SELECT parser/executor over an in-memory vector<Row>
//   (declarations). Filter (WHERE) -> project (columns) -> sort (ORDER BY) -> LIMIT.
#pragma once

#include <string>
#include <vector>
#include <variant>
#include <unordered_map>

namespace sql {

// A column value is either numeric or text (mirrors SQLite's dynamic typing).
using Value = std::variant<double, std::string>;

struct Row { std::unordered_map<std::string, Value> cols; };

// Parsed representation of a SELECT statement (a tiny query "AST").
struct SelectQuery {
    std::vector<std::string> columns;     // empty == SELECT *
    std::string              from;        // table name (data is pre-fetched)
    std::string              where_raw;   // raw WHERE clause, evaluated via shunting-yard
    std::string              order_by;    // column name, empty == no sort
    bool                     order_asc = true;
    int                      limit = -1;  // -1 == no limit
};

// Numeric view of a column (text that parses as a number is coerced).
double row_val(const Row& row, const std::string& col);

// Parse a SELECT string into a SelectQuery (keywords are case-insensitive).
SelectQuery parse_select(const std::string& sql);

// Run the query against pre-fetched rows and return the result set.
std::vector<Row> execute(const SelectQuery& q, const std::vector<Row>& data);

// Pretty-print a result set, one row per line.
void print_rows(const std::vector<Row>& rows);

} // namespace sql
