// ExprEval.h
// -----------------------------------------------------------------------------
// Self-contained expression compiler/evaluator for the Expression OFX plugin.
// A small Pratt (precedence-climbing) parser compiles a Nuke-style expression
// string into a flat AST once; eval() then walks it per pixel against a caller
// supplied variable buffer. Zero dependencies, C++11, header-only.
//
// Grammar (C-like, same spirit as Nuke's Color > Math > Expression node):
//   ternary   ?:                     (right assoc, lowest)
//   || && == != < <= > >= + - * / %   (C precedence)
//   ^ power                          (right assoc, binds tighter than unary -)
//   unary - + !
//   primary: number, name, name(args...), ( expr )
//
// Variables are supplied by index: the host passes an ordered list of names at
// compile time and a matching double[] at eval time. Unknown names that are not
// functions are a compile error. See README.md for the full function reference.
// -----------------------------------------------------------------------------
#ifndef EXPR_EVAL_H
#define EXPR_EVAL_H

#include <string>
#include <vector>
#include <cmath>
#include <cstdlib>

namespace expreval {

// ---- builtin functions ------------------------------------------------------
enum FuncId {
    F_SIN, F_COS, F_TAN, F_ASIN, F_ACOS, F_ATAN, F_SINH, F_COSH, F_TANH,
    F_EXP, F_LOG, F_LOG10, F_LOG2, F_SQRT, F_ABS, F_FLOOR, F_CEIL, F_TRUNC,
    F_RINT, F_ROUND, F_SIGN, F_RADIANS, F_DEGREES, F_EXPONENT, F_MANTISSA, F_POW2,
    F_POW, F_ATAN2, F_FMOD, F_HYPOT, F_STEP, F_LDEXP,
    F_MIN, F_MAX,
    F_CLAMP1, F_CLAMP3, F_LERP, F_SMOOTHSTEP,
    F_NOISE1, F_NOISE2, F_NOISE3, F_RANDOM1, F_RANDOM2, F_RANDOM3, F_FBM, F_TURB,
    F_FROM_SRGB, F_TO_SRGB, F_FROM_REC709, F_TO_REC709, F_FROM_BYTE, F_TO_BYTE,
    F_PI, F_E
};

// ---- noise helpers (match the Matchbox version for cross-tool parity) -------
inline double ev_fract(double x) { return x - std::floor(x); }
inline double ev_hash3(double x, double y, double z) {
    double px = ev_fract(x * 0.3183099 + 0.1) * 17.0;
    double py = ev_fract(y * 0.3183099 + 0.1) * 17.0;
    double pz = ev_fract(z * 0.3183099 + 0.1) * 17.0;
    return ev_fract(px * py * pz * (px + py + pz));
}
inline double ev_vnoise(double x, double y, double z) {
    double ix = std::floor(x), iy = std::floor(y), iz = std::floor(z);
    double fx = x - ix, fy = y - iy, fz = z - iz;
    double ux = fx * fx * (3.0 - 2.0 * fx);
    double uy = fy * fy * (3.0 - 2.0 * fy);
    double uz = fz * fz * (3.0 - 2.0 * fz);
    double c000 = ev_hash3(ix,     iy,     iz),     c100 = ev_hash3(ix+1.0, iy,     iz);
    double c010 = ev_hash3(ix,     iy+1.0, iz),     c110 = ev_hash3(ix+1.0, iy+1.0, iz);
    double c001 = ev_hash3(ix,     iy,     iz+1.0), c101 = ev_hash3(ix+1.0, iy,     iz+1.0);
    double c011 = ev_hash3(ix,     iy+1.0, iz+1.0), c111 = ev_hash3(ix+1.0, iy+1.0, iz+1.0);
    double x00 = c000 + (c100 - c000) * ux, x10 = c010 + (c110 - c010) * ux;
    double x01 = c001 + (c101 - c001) * ux, x11 = c011 + (c111 - c011) * ux;
    double y0 = x00 + (x10 - x00) * uy, y1 = x01 + (x11 - x01) * uy;
    return y0 + (y1 - y0) * uz;
}
inline double ev_noise(double x, double y, double z) { return ev_vnoise(x, y, z) * 2.0 - 1.0; }
inline double ev_random(double x, double y, double z) {
    double s = std::sin(x * 12.9898 + y * 78.233 + z * 37.719) * 43758.5453;
    return ev_fract(s);
}
inline double ev_to_sRGB(double c)   { return (c <= 0.0031308) ? c * 12.92 : 1.055 * std::pow(c, 1.0/2.4) - 0.055; }
inline double ev_from_sRGB(double c) { return (c <= 0.04045)   ? c / 12.92 : std::pow((c + 0.055)/1.055, 2.4); }
inline double ev_to_rec709(double c)   { return (c < 0.018) ? c * 4.5 : 1.099 * std::pow(c, 0.45) - 0.099; }
inline double ev_from_rec709(double c) { return (c < 0.081) ? c / 4.5 : std::pow((c + 0.099)/1.099, 1.0/0.45); }

// ---- AST --------------------------------------------------------------------
enum NodeKind { N_NUM, N_VAR, N_UNARY, N_BINARY, N_TERNARY, N_CALL };
enum BinOp { B_ADD, B_SUB, B_MUL, B_DIV, B_MOD, B_POW,
             B_LT, B_LE, B_GT, B_GE, B_EQ, B_NE, B_AND, B_OR };

struct Node {
    NodeKind kind;
    double num;          // N_NUM
    int    var;          // N_VAR (index into vars)
    int    op;           // N_UNARY ('-','!') / N_BINARY (BinOp)
    FuncId fid;          // N_CALL
    int    a, b, c;      // child node indices (-1 if unused)
    std::vector<int> args; // N_CALL argument node indices
    Node() : kind(N_NUM), num(0), var(-1), op(0), fid(F_PI), a(-1), b(-1), c(-1) {}
};

class Program {
public:
    // Compile `src` against `varNames` (ordered). Returns false and fills `err`
    // on failure. An empty expression compiles to constant 0 (as in Nuke).
    bool compile(const std::string& src, const std::vector<std::string>& varNames, std::string& err);

    // Evaluate. `vars` must have varNames.size() entries.
    double eval(const double* vars) const { return root_ < 0 ? 0.0 : evalNode(root_, vars); }

    bool ok() const { return ok_; }

private:
    std::vector<Node> nodes_;
    int  root_ = -1;
    bool ok_ = false;
    const std::vector<std::string>* names_ = nullptr;

    // --- tokenizer ---
    std::string s_;
    size_t p_ = 0;
    std::string err_;
    bool fail(const std::string& m) { err_ = m; return false; }

    void skipws() { while (p_ < s_.size() && (s_[p_]==' '||s_[p_]=='\t'||s_[p_]=='\n'||s_[p_]=='\r')) ++p_; }
    int  newNode(const Node& n) { nodes_.push_back(n); return (int)nodes_.size() - 1; }

    // --- parser (returns node index, or -1 on error with err_ set) ---
    int parseTernary();
    int parseBinary(int minPrec);
    int parseUnary();
    int parsePrimary();
    int makeCall(const std::string& name, std::vector<int>& args);
    int varIndex(const std::string& name) const;

    double evalNode(int idx, const double* vars) const;

    static int  binPrec(BinOp op);
    static bool rightAssoc(BinOp op) { return op == B_POW; }
};

// ---- helpers ----------------------------------------------------------------
inline int Program::varIndex(const std::string& name) const {
    if (!names_) return -1;
    for (size_t i = 0; i < names_->size(); ++i) if ((*names_)[i] == name) return (int)i;
    return -1;
}

inline int Program::binPrec(BinOp op) {
    switch (op) {
        case B_OR:  return 1;
        case B_AND: return 2;
        case B_EQ: case B_NE: return 3;
        case B_LT: case B_LE: case B_GT: case B_GE: return 4;
        case B_ADD: case B_SUB: return 5;
        case B_MUL: case B_DIV: case B_MOD: return 6;
        case B_POW: return 8;
    }
    return 0;
}

inline bool Program::compile(const std::string& src, const std::vector<std::string>& varNames, std::string& err) {
    nodes_.clear(); root_ = -1; ok_ = false; err_.clear();
    names_ = &varNames; s_ = src; p_ = 0;

    skipws();
    if (p_ >= s_.size()) {                 // empty == 0 (Nuke behaviour)
        Node n; n.kind = N_NUM; n.num = 0.0; root_ = newNode(n);
        ok_ = true; return true;
    }
    int r = parseTernary();
    if (r < 0) { err = err_; return false; }
    skipws();
    if (p_ < s_.size()) { err = "unexpected trailing characters near '" + s_.substr(p_) + "'"; return false; }
    root_ = r; ok_ = true; return true;
}

// peek the next binary operator without consuming; returns true if one is present
inline bool peekBinOp(const std::string& s, size_t p, BinOp& op, int& len) {
    if (p >= s.size()) return false;
    char c = s[p]; char d = (p + 1 < s.size()) ? s[p+1] : '\0';
    switch (c) {
        case '+': op = B_ADD; len = 1; return true;
        case '-': op = B_SUB; len = 1; return true;
        case '*': op = B_MUL; len = 1; return true;
        case '/': op = B_DIV; len = 1; return true;
        case '%': op = B_MOD; len = 1; return true;
        case '^': op = B_POW; len = 1; return true;
        case '<': if (d=='=') { op = B_LE; len = 2; } else { op = B_LT; len = 1; } return true;
        case '>': if (d=='=') { op = B_GE; len = 2; } else { op = B_GT; len = 1; } return true;
        case '=': if (d=='=') { op = B_EQ; len = 2; return true; } return false;
        case '!': if (d=='=') { op = B_NE; len = 2; return true; } return false;
        case '&': if (d=='&') { op = B_AND; len = 2; return true; } return false;
        case '|': if (d=='|') { op = B_OR; len = 2; return true; } return false;
    }
    return false;
}

inline int Program::parseTernary() {
    int cond = parseBinary(1);
    if (cond < 0) return -1;
    skipws();
    if (p_ < s_.size() && s_[p_] == '?') {
        ++p_;
        int a = parseTernary(); if (a < 0) return -1;
        skipws();
        if (p_ >= s_.size() || s_[p_] != ':') return fail("expected ':' in ternary"), -1;
        ++p_;
        int b = parseTernary(); if (b < 0) return -1;
        Node n; n.kind = N_TERNARY; n.a = cond; n.b = a; n.c = b;
        return newNode(n);
    }
    return cond;
}

inline int Program::parseBinary(int minPrec) {
    int left = parseUnary();
    if (left < 0) return -1;
    for (;;) {
        skipws();
        BinOp op; int len;
        if (!peekBinOp(s_, p_, op, len)) break;
        int prec = binPrec(op);
        if (prec < minPrec) break;
        p_ += len;
        int nextMin = rightAssoc(op) ? prec : prec + 1;
        int right = parseBinary(nextMin);
        if (right < 0) return -1;
        Node n; n.kind = N_BINARY; n.op = op; n.a = left; n.b = right;
        left = newNode(n);
    }
    return left;
}

inline int Program::parseUnary() {
    skipws();
    if (p_ < s_.size() && (s_[p_] == '-' || s_[p_] == '+' || s_[p_] == '!')) {
        char op = s_[p_]; ++p_;
        int operand = parseBinary(7);   // binds tighter than * but looser than ^
        if (operand < 0) return -1;
        if (op == '+') return operand;  // unary plus is a no-op
        Node n; n.kind = N_UNARY; n.op = op; n.a = operand;
        return newNode(n);
    }
    return parsePrimary();
}

inline int Program::parsePrimary() {
    skipws();
    if (p_ >= s_.size()) return fail("unexpected end of expression"), -1;
    char c = s_[p_];

    if (c == '(') {
        ++p_;
        int e = parseTernary(); if (e < 0) return -1;
        skipws();
        if (p_ >= s_.size() || s_[p_] != ')') return fail("expected ')'"), -1;
        ++p_; return e;
    }

    if ((c >= '0' && c <= '9') || c == '.') {
        size_t start = p_;
        while (p_ < s_.size() && ((s_[p_] >= '0' && s_[p_] <= '9') || s_[p_] == '.')) ++p_;
        if (p_ < s_.size() && (s_[p_] == 'e' || s_[p_] == 'E')) {   // exponent
            ++p_;
            if (p_ < s_.size() && (s_[p_] == '+' || s_[p_] == '-')) ++p_;
            while (p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9') ++p_;
        }
        Node n; n.kind = N_NUM; n.num = std::atof(s_.substr(start, p_ - start).c_str());
        return newNode(n);
    }

    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        size_t start = p_;
        while (p_ < s_.size() &&
               ((s_[p_] >= 'a' && s_[p_] <= 'z') || (s_[p_] >= 'A' && s_[p_] <= 'Z') ||
                (s_[p_] >= '0' && s_[p_] <= '9') || s_[p_] == '_' || s_[p_] == '.')) ++p_;
        std::string name = s_.substr(start, p_ - start);
        skipws();
        if (p_ < s_.size() && s_[p_] == '(') {          // function call
            ++p_;
            std::vector<int> args;
            skipws();
            if (p_ < s_.size() && s_[p_] == ')') { ++p_; }
            else {
                for (;;) {
                    int a = parseTernary(); if (a < 0) return -1;
                    args.push_back(a);
                    skipws();
                    if (p_ < s_.size() && s_[p_] == ',') { ++p_; continue; }
                    if (p_ < s_.size() && s_[p_] == ')') { ++p_; break; }
                    return fail("expected ',' or ')' in call to " + name), -1;
                }
            }
            return makeCall(name, args);
        }
        // bare name: constant or variable
        if (name == "pi") { Node n; n.kind = N_NUM; n.num = 3.141592653589793; return newNode(n); }
        if (name == "e")  { Node n; n.kind = N_NUM; n.num = 2.718281828459045; return newNode(n); }
        int vi = varIndex(name);
        if (vi < 0) return fail("unknown variable '" + name + "'"), -1;
        Node n; n.kind = N_VAR; n.var = vi;
        return newNode(n);
    }

    return fail(std::string("unexpected character '") + c + "'"), -1;
}

inline int Program::makeCall(const std::string& name, std::vector<int>& args) {
    int argc = (int)args.size();
    FuncId id; bool found = true;
    auto need = [&](int n)->bool { return argc == n; };

    if      (name == "sin"   && need(1)) id = F_SIN;
    else if (name == "cos"   && need(1)) id = F_COS;
    else if (name == "tan"   && need(1)) id = F_TAN;
    else if (name == "asin"  && need(1)) id = F_ASIN;
    else if (name == "acos"  && need(1)) id = F_ACOS;
    else if (name == "atan"  && need(1)) id = F_ATAN;
    else if (name == "atan"  && need(2)) id = F_ATAN2;     // atan(y,x)
    else if (name == "atan2" && need(2)) id = F_ATAN2;
    else if (name == "sinh"  && need(1)) id = F_SINH;
    else if (name == "cosh"  && need(1)) id = F_COSH;
    else if (name == "tanh"  && need(1)) id = F_TANH;
    else if (name == "exp"   && need(1)) id = F_EXP;
    else if (name == "log"   && need(1)) id = F_LOG;
    else if (name == "log10" && need(1)) id = F_LOG10;
    else if (name == "log2"  && need(1)) id = F_LOG2;
    else if (name == "sqrt"  && need(1)) id = F_SQRT;
    else if ((name == "abs"  || name == "fabs") && need(1)) id = F_ABS;
    else if (name == "floor" && need(1)) id = F_FLOOR;
    else if (name == "ceil"  && need(1)) id = F_CEIL;
    else if ((name == "trunc"|| name == "int") && need(1)) id = F_TRUNC;
    else if (name == "rint"  && need(1)) id = F_RINT;
    else if (name == "round" && need(1)) id = F_ROUND;
    else if (name == "sign"  && need(1)) id = F_SIGN;
    else if (name == "radians" && need(1)) id = F_RADIANS;
    else if (name == "degrees" && need(1)) id = F_DEGREES;
    else if (name == "exponent" && need(1)) id = F_EXPONENT;
    else if (name == "mantissa" && need(1)) id = F_MANTISSA;
    else if (name == "pow2"  && need(1)) id = F_POW2;
    else if (name == "pow"   && need(2)) id = F_POW;
    else if (name == "fmod"  && need(2)) id = F_FMOD;
    else if (name == "hypot" && need(2)) id = F_HYPOT;
    else if (name == "step"  && need(2)) id = F_STEP;
    else if (name == "ldexp" && need(2)) id = F_LDEXP;
    else if (name == "min"   && argc >= 2) id = F_MIN;
    else if (name == "max"   && argc >= 2) id = F_MAX;
    else if (name == "clamp" && need(1)) id = F_CLAMP1;
    else if (name == "clamp" && need(3)) id = F_CLAMP3;
    else if ((name == "lerp" || name == "mix") && need(3)) id = F_LERP;
    else if (name == "smoothstep" && need(3)) id = F_SMOOTHSTEP;
    else if (name == "noise" && need(1)) id = F_NOISE1;
    else if (name == "noise" && need(2)) id = F_NOISE2;
    else if (name == "noise" && need(3)) id = F_NOISE3;
    else if (name == "random" && need(1)) id = F_RANDOM1;
    else if (name == "random" && need(2)) id = F_RANDOM2;
    else if (name == "random" && need(3)) id = F_RANDOM3;
    else if (name == "fBm"   && need(6)) id = F_FBM;
    else if (name == "turbulence" && need(6)) id = F_TURB;
    else if (name == "from_sRGB"   && need(1)) id = F_FROM_SRGB;
    else if (name == "to_sRGB"     && need(1)) id = F_TO_SRGB;
    else if (name == "from_rec709" && need(1)) id = F_FROM_REC709;
    else if (name == "to_rec709"   && need(1)) id = F_TO_REC709;
    else if (name == "from_byte"   && need(1)) id = F_FROM_BYTE;
    else if (name == "to_byte"     && need(1)) id = F_TO_BYTE;
    else if (name == "pi" && need(0)) id = F_PI;
    else if (name == "e"  && need(0)) id = F_E;
    else found = false;

    if (!found) return fail("unknown function or wrong argument count: " + name + "()"), -1;
    Node n; n.kind = N_CALL; n.fid = id; n.args = args;
    return newNode(n);
}

inline double Program::evalNode(int idx, const double* vars) const {
    const Node& n = nodes_[idx];
    switch (n.kind) {
        case N_NUM: return n.num;
        case N_VAR: return vars[n.var];
        case N_UNARY: {
            double v = evalNode(n.a, vars);
            return (n.op == '-') ? -v : (v == 0.0 ? 1.0 : 0.0); // '!' otherwise
        }
        case N_TERNARY:
            return evalNode(n.a, vars) != 0.0 ? evalNode(n.b, vars) : evalNode(n.c, vars);
        case N_BINARY: {
            // short-circuit logicals
            if (n.op == B_AND) return (evalNode(n.a, vars) != 0.0 && evalNode(n.b, vars) != 0.0) ? 1.0 : 0.0;
            if (n.op == B_OR)  return (evalNode(n.a, vars) != 0.0 || evalNode(n.b, vars) != 0.0) ? 1.0 : 0.0;
            double a = evalNode(n.a, vars), b = evalNode(n.b, vars);
            switch (n.op) {
                case B_ADD: return a + b;  case B_SUB: return a - b;
                case B_MUL: return a * b;  case B_DIV: return a / b;
                case B_MOD: return std::fmod(a, b); case B_POW: return std::pow(a, b);
                case B_LT: return a <  b ? 1.0 : 0.0; case B_LE: return a <= b ? 1.0 : 0.0;
                case B_GT: return a >  b ? 1.0 : 0.0; case B_GE: return a >= b ? 1.0 : 0.0;
                case B_EQ: return a == b ? 1.0 : 0.0; case B_NE: return a != b ? 1.0 : 0.0;
                default: return 0.0;
            }
        }
        case N_CALL: {
            double a0 = n.args.size() > 0 ? evalNode(n.args[0], vars) : 0.0;
            double a1 = n.args.size() > 1 ? evalNode(n.args[1], vars) : 0.0;
            double a2 = n.args.size() > 2 ? evalNode(n.args[2], vars) : 0.0;
            switch (n.fid) {
                case F_SIN: return std::sin(a0);   case F_COS: return std::cos(a0);
                case F_TAN: return std::tan(a0);   case F_ASIN: return std::asin(a0);
                case F_ACOS: return std::acos(a0); case F_ATAN: return std::atan(a0);
                case F_SINH: return std::sinh(a0); case F_COSH: return std::cosh(a0);
                case F_TANH: return std::tanh(a0); case F_EXP: return std::exp(a0);
                case F_LOG: return std::log(a0);   case F_LOG10: return std::log10(a0);
                case F_LOG2: return std::log(a0)/std::log(2.0); case F_SQRT: return std::sqrt(a0);
                case F_ABS: return std::fabs(a0);  case F_FLOOR: return std::floor(a0);
                case F_CEIL: return std::ceil(a0); case F_TRUNC: return (a0 < 0 ? std::ceil(a0) : std::floor(a0));
                case F_RINT: return std::floor(a0 + 0.5); case F_ROUND: return std::floor(a0 + 0.5);
                case F_SIGN: return (a0 > 0) - (a0 < 0); case F_RADIANS: return a0 * 0.017453292519943295;
                case F_DEGREES: return a0 * 57.29577951308232;
                case F_EXPONENT: return std::floor(std::log2(std::fabs(a0) + 1e-30));
                case F_MANTISSA: return a0 / std::exp2(std::floor(std::log2(std::fabs(a0) + 1e-30)));
                case F_POW2: return a0 * a0;
                case F_POW: return std::pow(a0, a1); case F_ATAN2: return std::atan2(a0, a1);
                case F_FMOD: return std::fmod(a0, a1); case F_HYPOT: return std::hypot(a0, a1);
                case F_STEP: return a1 < a0 ? 0.0 : 1.0;        // step(edge,x)
                case F_LDEXP: return a0 * std::exp2(a1);
                case F_MIN: { double m = a0; for (size_t i = 1; i < n.args.size(); ++i) m = std::min(m, evalNode(n.args[i], vars)); return m; }
                case F_MAX: { double m = a0; for (size_t i = 1; i < n.args.size(); ++i) m = std::max(m, evalNode(n.args[i], vars)); return m; }
                case F_CLAMP1: return a0 < 0 ? 0.0 : (a0 > 1 ? 1.0 : a0);
                case F_CLAMP3: return a0 < a1 ? a1 : (a0 > a2 ? a2 : a0);
                case F_LERP: return a0 + (a1 - a0) * a2;
                case F_SMOOTHSTEP: { double t = (a2 - a0) / (a1 - a0); t = t < 0 ? 0 : (t > 1 ? 1 : t); return t * t * (3 - 2 * t); }
                case F_NOISE1: return ev_noise(a0, 0, 0); case F_NOISE2: return ev_noise(a0, a1, 0);
                case F_NOISE3: return ev_noise(a0, a1, a2);
                case F_RANDOM1: return ev_random(a0, 0, 0); case F_RANDOM2: return ev_random(a0, a1, 0);
                case F_RANDOM3: return ev_random(a0, a1, a2);
                case F_FBM: case F_TURB: {
                    int oct = (int)evalNode(n.args[3], vars);
                    double lac = evalNode(n.args[4], vars), gain = evalNode(n.args[5], vars);
                    double sum = 0, amp = 1, fr = 1;
                    for (int i = 0; i < oct && i < 32; ++i) {
                        double v = ev_noise(a0 * fr, a1 * fr, a2 * fr);
                        sum += amp * (n.fid == F_TURB ? std::fabs(v) : v);
                        fr *= lac; amp *= gain;
                    }
                    return sum;
                }
                case F_FROM_SRGB: return ev_from_sRGB(a0); case F_TO_SRGB: return ev_to_sRGB(a0);
                case F_FROM_REC709: return ev_from_rec709(a0); case F_TO_REC709: return ev_to_rec709(a0);
                case F_FROM_BYTE: return ev_from_sRGB(a0 / 255.0); case F_TO_BYTE: return ev_to_sRGB(a0) * 255.0;
                case F_PI: return 3.141592653589793; case F_E: return 2.718281828459045;
            }
            return 0.0;
        }
    }
    return 0.0;
}

} // namespace expreval
#endif // EXPR_EVAL_H
