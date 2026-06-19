// Lab 7: Driver for the modular SQL query engine.
//   Build (CMake):  cmake -S . -B build && cmake --build build && ./build/query_engine
//   Build (direct): g++ -std=c++17 -o query_engine main.cpp shunting_yard.cpp sql_parser.cpp
//
// Pipeline:  SQL string -> parse_select (AST) -> execute
//            WHERE clauses go infix -> RPN (shunting-yard) -> per-row evaluation.
#include "shunting_yard.h"
#include "sql_parser.h"

#include <iostream>

// Part 1: show the shunting-yard conversion + evaluation on its own.
static void shunting_demo() {
    std::cout << "=== Part 1: Shunting-Yard expression evaluator ===\n";
    std::string expr = "age * 2 + salary / 1000 > 100";
    auto rpn = sy::to_rpn(sy::tokenize(expr));

    std::cout << "Expression : " << expr << "\nRPN        : ";
    for (auto& t : rpn) std::cout << t << " ";
    std::cout << "\n";

    std::unordered_map<std::string, double> vars = {{"age", 30}, {"salary", 50000}};
    std::cout << "Result     : " << (sy::eval_rpn(rpn, vars) ? "true" : "false") << "\n\n";
}

// Part 2: run real SELECT statements through the parser + executor.
static void sql_demo() {
    std::cout << "=== Part 2: SQL SELECT over in-memory rows ===\n";
    std::vector<sql::Row> students = {
        {{{ "id", 1.0 }, { "name", std::string("Alice") }, { "age", 22.0 }, { "gpa", 3.8 }}},
        {{{ "id", 2.0 }, { "name", std::string("Bob")   }, { "age", 25.0 }, { "gpa", 2.9 }}},
        {{{ "id", 3.0 }, { "name", std::string("Carol") }, { "age", 21.0 }, { "gpa", 3.5 }}},
        {{{ "id", 4.0 }, { "name", std::string("Dave")  }, { "age", 30.0 }, { "gpa", 3.1 }}},
    };

    const char* queries[] = {
        "SELECT id, name, gpa FROM students WHERE gpa > 3.0 ORDER BY gpa DESC LIMIT 3",
        "SELECT * FROM students WHERE age >= 22 && age <= 26",
        "SELECT name FROM students WHERE (gpa > 3.4 || age < 22) && id != 1",
    };
    for (auto* q : queries) {
        std::cout << "SQL: " << q << "\n";
        sql::print_rows(sql::execute(sql::parse_select(q), students));
        std::cout << "\n";
    }
}

int main() {
    shunting_demo();
    sql_demo();
    return 0;
}
