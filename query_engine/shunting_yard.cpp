// Lab 7: Shunting-Yard expression module (implementation).
//   tokenize -> to_rpn (infix to postfix) -> eval_rpn (stack machine).
#include "shunting_yard.h"

#include <stack>
#include <stdexcept>
#include <cctype>
#include <cmath>

namespace sy {

const std::unordered_map<std::string, OpInfo> OPS = {
    {"||", {1, false}},   // logical OR
    {"&&", {2, false}},   // logical AND
    {"=",  {3, false}}, {"!=", {3, false}},
    {"<",  {4, false}}, {">",  {4, false}},
    {"<=", {4, false}}, {">=", {4, false}},
    {"+",  {5, false}}, {"-",  {5, false}},
    {"*",  {6, false}}, {"/",  {6, false}},
    {"^",  {7, true }},   // exponentiation (right-associative)
};

// ───────────────────────────── tokenizer ─────────────────────────────

std::vector<std::string> tokenize(const std::string& expr) {
    std::vector<std::string> tokens;
    int i = 0, n = (int)expr.size();
    while (i < n) {
        if (std::isspace((unsigned char)expr[i])) { i++; continue; }

        if (std::isdigit((unsigned char)expr[i]) ||
            (expr[i] == '.' && i + 1 < n && std::isdigit((unsigned char)expr[i + 1]))) {
            int j = i;
            while (j < n && (std::isdigit((unsigned char)expr[j]) || expr[j] == '.')) j++;
            tokens.push_back(expr.substr(i, j - i));
            i = j;
        } else if (std::isalpha((unsigned char)expr[i]) || expr[i] == '_') {
            int j = i;
            while (j < n && (std::isalnum((unsigned char)expr[j]) || expr[j] == '_')) j++;
            tokens.push_back(expr.substr(i, j - i));
            i = j;
        } else if (expr[i] == '(' || expr[i] == ')') {
            tokens.push_back(std::string(1, expr[i++]));
        } else {
            if (i + 1 < n) {                       // greedily match two-char operators
                std::string two = expr.substr(i, 2);
                if (OPS.count(two)) { tokens.push_back(two); i += 2; continue; }
            }
            tokens.push_back(std::string(1, expr[i++]));
        }
    }
    return tokens;
}

// ───────────────────────────── infix -> RPN ─────────────────────────────

std::vector<std::string> to_rpn(const std::vector<std::string>& tokens) {
    std::vector<std::string> output;
    std::stack<std::string>  ops;
    for (const auto& tok : tokens) {
        if (tok == "(") {
            ops.push(tok);
        } else if (tok == ")") {
            while (!ops.empty() && ops.top() != "(") { output.push_back(ops.top()); ops.pop(); }
            if (ops.empty()) throw std::runtime_error("Mismatched parentheses");
            ops.pop();                              // discard the '('
        } else if (OPS.count(tok)) {
            const auto& o1 = OPS.at(tok);
            while (!ops.empty() && OPS.count(ops.top())) {
                const auto& o2 = OPS.at(ops.top());
                if (o2.precedence > o1.precedence ||
                    (o2.precedence == o1.precedence && !o1.right_assoc)) {
                    output.push_back(ops.top()); ops.pop();
                } else break;
            }
            ops.push(tok);
        } else {
            output.push_back(tok);                  // number or identifier
        }
    }
    while (!ops.empty()) {
        if (ops.top() == "(") throw std::runtime_error("Mismatched parentheses");
        output.push_back(ops.top()); ops.pop();
    }
    return output;
}

// ───────────────────────────── RPN evaluator ─────────────────────────────

double eval_rpn(const std::vector<std::string>& rpn,
                const std::unordered_map<std::string, double>& vars) {
    std::stack<double> stk;
    for (const auto& tok : rpn) {
        if (OPS.count(tok)) {
            if (stk.size() < 2) throw std::runtime_error("Malformed expression");
            double b = stk.top(); stk.pop();
            double a = stk.top(); stk.pop();
            if      (tok == "+")  stk.push(a + b);
            else if (tok == "-")  stk.push(a - b);
            else if (tok == "*")  stk.push(a * b);
            else if (tok == "/")  stk.push(a / b);
            else if (tok == "^")  stk.push(std::pow(a, b));
            else if (tok == "<")  stk.push(a <  b ? 1.0 : 0.0);
            else if (tok == ">")  stk.push(a >  b ? 1.0 : 0.0);
            else if (tok == "<=") stk.push(a <= b ? 1.0 : 0.0);
            else if (tok == ">=") stk.push(a >= b ? 1.0 : 0.0);
            else if (tok == "=")  stk.push(a == b ? 1.0 : 0.0);
            else if (tok == "!=") stk.push(a != b ? 1.0 : 0.0);
            else if (tok == "&&") stk.push((a != 0.0 && b != 0.0) ? 1.0 : 0.0);
            else if (tok == "||") stk.push((a != 0.0 || b != 0.0) ? 1.0 : 0.0);
        } else {
            try { stk.push(std::stod(tok)); }       // numeric literal
            catch (...) {                            // otherwise an identifier
                auto it = vars.find(tok);
                if (it == vars.end()) throw std::runtime_error("Unknown variable: " + tok);
                stk.push(it->second);
            }
        }
    }
    if (stk.empty()) throw std::runtime_error("Empty expression");
    return stk.top();
}

} // namespace sy
