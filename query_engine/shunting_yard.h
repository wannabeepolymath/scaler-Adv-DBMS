// Lab 7: Shunting-Yard expression module (declarations).
//   Converts infix WHERE-clause expressions into RPN and evaluates them.
#pragma once

#include <string>
#include <vector>
#include <unordered_map>

namespace sy {

// Operator metadata: precedence (higher binds tighter) + associativity.
struct OpInfo { int precedence; bool right_assoc; };

// Operator table shared by the converter and the evaluator.
extern const std::unordered_map<std::string, OpInfo> OPS;

// Split an infix expression into tokens: numbers, identifiers, operators, parens.
std::vector<std::string> tokenize(const std::string& expr);

// Dijkstra's shunting-yard: infix token stream -> postfix (RPN) token stream.
std::vector<std::string> to_rpn(const std::vector<std::string>& tokens);

// Evaluate an RPN stream against a variable map (all values are doubles;
// booleans are encoded as 1.0 / 0.0). Throws on unknown identifiers.
double eval_rpn(const std::vector<std::string>& rpn,
                const std::unordered_map<std::string, double>& vars);

} // namespace sy
