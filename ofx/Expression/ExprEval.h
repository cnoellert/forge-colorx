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
#include <cstdio>

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

// ---- noise helpers: classic Perlin (Gustavson/Ashima, tableless) ------------
// Real gradient (Perlin) noise via a COMPUTABLE permutation, permute(x) =
// mod289((34x+1)x) — no 256-entry table, so the identical float sequence ports
// bit-for-bit to ExprKernel.h (CPU transpile), ExprKernelCuda.h and
// ExprKernelMetal.h, and (in vec form) to ColorExpression.glsl. Computed in FLOAT
// so the CPU oracle matches the float-only GPU back-ends. Every op is +,-,*,floor
// with intermediates kept < 289 (float-exact) — NO transcendentals, so noise AND
// random() are now cross-backend parity-clean (the old sinf random() gap is gone).
// The differential tests guard the C-family copies against drift. (Output range is
// Ashima's ~[-1,1]; this changed the noise values vs the old value-noise — PASSOFF
// always noted noise is a swappable approximation.)
inline float ev_fractf(float x)   { return x - std::floor(x); }
inline float ev_pmod289(float x)  { return x - std::floor(x * (1.0f/289.0f)) * 289.0f; }
inline float ev_ppermute(float x) { return ev_pmod289((x*34.0f + 1.0f) * x); }
inline float ev_ptinv(float r)    { return 1.79284291400159f - 0.85373472095314f * r; }
inline float ev_pfade(float t)    { return t*t*t*(t*(t*6.0f - 15.0f) + 10.0f); }
inline float ev_pnoise3(float Px, float Py, float Pz) {
    float i0x=std::floor(Px), i0y=std::floor(Py), i0z=std::floor(Pz);
    float i1x=i0x+1.0f, i1y=i0y+1.0f, i1z=i0z+1.0f;
    i0x=ev_pmod289(i0x); i0y=ev_pmod289(i0y); i0z=ev_pmod289(i0z);
    i1x=ev_pmod289(i1x); i1y=ev_pmod289(i1y); i1z=ev_pmod289(i1z);
    float f0x=Px-std::floor(Px), f0y=Py-std::floor(Py), f0z=Pz-std::floor(Pz);
    float f1x=f0x-1.0f, f1y=f0y-1.0f, f1z=f0z-1.0f;
    float ixL[4]={i0x,i1x,i0x,i1x}, iyL[4]={i0y,i0y,i1y,i1y};   // (0,0)(1,0)(0,1)(1,1)
    float gx[8], gy[8], gz[8];
    for (int L=0; L<4; ++L) {
        float ixy = ev_ppermute(ev_ppermute(ixL[L]) + iyL[L]);
        for (int zz=0; zz<2; ++zz) {                            // 0 = z0 plane, 1 = z1
            float p = ev_ppermute(ixy + (zz==0 ? i0z : i1z));
            float g  = p * (1.0f/7.0f);
            float ggy = ev_fractf(std::floor(g) * (1.0f/7.0f)) - 0.5f;
            float ggx = ev_fractf(g);
            float ggz = 0.5f - std::fabs(ggx) - std::fabs(ggy);
            float sz  = (ggz <= 0.0f) ? 1.0f : 0.0f;
            ggx -= sz * ((ggx >= 0.0f ? 1.0f : 0.0f) - 0.5f);
            ggy -= sz * ((ggy >= 0.0f ? 1.0f : 0.0f) - 0.5f);
            int k = L + zz*4;  gx[k]=ggx; gy[k]=ggy; gz[k]=ggz;
        }
    }
    for (int k=0;k<8;++k){ float n=ev_ptinv(gx[k]*gx[k]+gy[k]*gy[k]+gz[k]*gz[k]); gx[k]*=n; gy[k]*=n; gz[k]*=n; }
    float n000=gx[0]*f0x+gy[0]*f0y+gz[0]*f0z, n100=gx[1]*f1x+gy[1]*f0y+gz[1]*f0z;
    float n010=gx[2]*f0x+gy[2]*f1y+gz[2]*f0z, n110=gx[3]*f1x+gy[3]*f1y+gz[3]*f0z;
    float n001=gx[4]*f0x+gy[4]*f0y+gz[4]*f1z, n101=gx[5]*f1x+gy[5]*f0y+gz[5]*f1z;
    float n011=gx[6]*f0x+gy[6]*f1y+gz[6]*f1z, n111=gx[7]*f1x+gy[7]*f1y+gz[7]*f1z;
    float wx=ev_pfade(f0x), wy=ev_pfade(f0y), wz=ev_pfade(f0z);
    float nz0=n000+(n001-n000)*wz, nz1=n100+(n101-n100)*wz, nz2=n010+(n011-n010)*wz, nz3=n110+(n111-n110)*wz;
    float ny0=nz0+(nz2-nz0)*wy, ny1=nz1+(nz3-nz1)*wy;
    return 2.2f * (ny0 + (ny1-ny0)*wx);
}
inline double ev_noise(double x, double y, double z) {
    return (double)ev_pnoise3((float)x, (float)y, (float)z);
}
// random(): tableless permute hash of the integer cell — float-only (GLSL-120
// safe), parity-clean across all back-ends. Two permute rounds give 289*289
// distinct values (no banding). Cell-based, so feed pixel coords for per-pixel.
inline double ev_random(double x, double y, double z) {
    float a=ev_pmod289(std::floor((float)x)), b=ev_pmod289(std::floor((float)y)), c=ev_pmod289(std::floor((float)z));
    float h1=ev_ppermute(ev_ppermute(ev_ppermute(a)+b)+c);
    float h2=ev_ppermute(ev_ppermute(ev_ppermute(a+1.0f)+b)+c);
    return (double)ev_fractf((h1*289.0f + h2) * (1.0f/83521.0f));
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

    // Stack-VM evaluate: same result as eval(), but walks a flat opcode tape (built
    // in compile()) with a value stack instead of recursing the AST. No per-node
    // function call / vector-index chase. Short-circuit &&,||,?: preserved via jumps,
    // so it is bit-for-bit eval()-equivalent. Prototype: measured against eval() in
    // tests/bench_expr.cpp.
    double evalTape(const double* vars) const;

    // Transpile the compiled AST to a C-like expression string over a variable
    // array `v[]`, calling the exh_* helpers from ExprKernel.h. The same string
    // is valid as a CUDA / Metal / GLSL kernel body given the matching prelude;
    // tests/test_transpile.cpp differential-checks it against eval(). The grammar
    // emitted is a portable float-math subset (no doubles assumed): every literal
    // and helper is type-agnostic, so the prelude's FLT typedef picks the width.
    std::string emitC() const { std::string o; if (root_ < 0) o = "0.0"; else emitNode(root_, o); return o; }

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
    void   emitNode(int idx, std::string& o) const;

    static int  binPrec(BinOp op);
    static bool rightAssoc(BinOp op) { return op == B_POW; }

    // --- stack VM (prototype) ---
    enum OpCode { OP_NUM, OP_VAR, OP_NEG, OP_NOT, OP_BIN, OP_CALL,
                  OP_JZ, OP_JNZ, OP_JMP };
    struct Instr { int op; int i; int n; double d; };  // i: var/binop/fid/jmp-target; n: argc
    std::vector<Instr> tape_;
    int  emitI(const Instr& in) { tape_.push_back(in); return (int)tape_.size() - 1; }
    void emitTape(int idx);                 // post-order compile of node `idx`
    void compileTape();                     // build tape_ from root_ (call after parse)
    static double applyBin(int op, double a, double b);
    static double applyCall(FuncId fid, const double* a, int n);
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
        ok_ = true; compileTape(); return true;
    }
    int r = parseTernary();
    if (r < 0) { err = err_; return false; }
    skipws();
    if (p_ < s_.size()) { err = "unexpected trailing characters near '" + s_.substr(p_) + "'"; return false; }
    root_ = r; ok_ = true; compileTape(); return true;
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
        // Nuke parity: 't' is an alias for 'frame' (in Nuke's expression language
        // frame/t/x all evaluate to the frame number; the Expression node reuses
        // x for the pixel coord, so only t aliases frame here). A user temp named
        // 't' still wins (varIndex is checked first).
        if (vi < 0 && name == "t") vi = varIndex("frame");
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
                    // float-internal to match exh_fbm/exh_turb on the GPU (see noise note)
                    int oct = (int)evalNode(n.args[3], vars);
                    float lac = (float)evalNode(n.args[4], vars), gain = (float)evalNode(n.args[5], vars);
                    float x0 = (float)a0, y0 = (float)a1, z0 = (float)a2;
                    float sum = 0.0f, amp = 1.0f, fr = 1.0f;
                    for (int i = 0; i < oct && i < 32; ++i) {
                        float v = ev_pnoise3(x0 * fr, y0 * fr, z0 * fr);
                        sum += amp * (n.fid == F_TURB ? std::fabs(v) : v);
                        fr *= lac; amp *= gain;
                    }
                    return (double)sum;
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

// ---- stack VM: flatten the AST to a postfix opcode tape ---------------------
// emitTape walks the node post-order, pushing operands before the op that consumes
// them. Short-circuit &&,||,?: are compiled to conditional jumps so evaluation
// order matches eval() exactly (the skipped branch is never executed).
inline void Program::emitTape(int idx) {
    const Node& n = nodes_[idx];
    switch (n.kind) {
        case N_NUM: { Instr in{OP_NUM,0,0,n.num}; emitI(in); return; }
        case N_VAR: { Instr in{OP_VAR,n.var,0,0}; emitI(in); return; }
        case N_UNARY:
            emitTape(n.a);
            { Instr in{n.op == '-' ? OP_NEG : OP_NOT,0,0,0}; emitI(in); }
            return;
        case N_TERNARY: {
            emitTape(n.a);
            Instr jz{OP_JZ,0,0,0}; int pjz = emitI(jz);   // pop cond; if 0 -> else
            emitTape(n.b);
            Instr jmp{OP_JMP,0,0,0}; int pjmp = emitI(jmp);
            tape_[pjz].i = (int)tape_.size();              // else target
            emitTape(n.c);
            tape_[pjmp].i = (int)tape_.size();             // end target
            return;
        }
        case N_BINARY: {
            if (n.op == B_AND) {
                emitTape(n.a);
                Instr j1{OP_JZ,0,0,0}; int p1 = emitI(j1);
                emitTape(n.b);
                Instr j2{OP_JZ,0,0,0}; int p2 = emitI(j2);
                Instr one{OP_NUM,0,0,1.0}; emitI(one);
                Instr jmp{OP_JMP,0,0,0}; int pj = emitI(jmp);
                tape_[p1].i = tape_[p2].i = (int)tape_.size();
                Instr zero{OP_NUM,0,0,0.0}; emitI(zero);
                tape_[pj].i = (int)tape_.size();
                return;
            }
            if (n.op == B_OR) {
                emitTape(n.a);
                Instr j1{OP_JNZ,0,0,0}; int p1 = emitI(j1);
                emitTape(n.b);
                Instr j2{OP_JNZ,0,0,0}; int p2 = emitI(j2);
                Instr zero{OP_NUM,0,0,0.0}; emitI(zero);
                Instr jmp{OP_JMP,0,0,0}; int pj = emitI(jmp);
                tape_[p1].i = tape_[p2].i = (int)tape_.size();
                Instr one{OP_NUM,0,0,1.0}; emitI(one);
                tape_[pj].i = (int)tape_.size();
                return;
            }
            emitTape(n.a); emitTape(n.b);
            { Instr in{OP_BIN,n.op,0,0}; emitI(in); }
            return;
        }
        case N_CALL: {
            for (size_t i = 0; i < n.args.size(); ++i) emitTape(n.args[i]);
            Instr in{OP_CALL,(int)n.fid,(int)n.args.size(),0}; emitI(in);
            return;
        }
    }
}

inline void Program::compileTape() {
    tape_.clear();
    if (root_ >= 0) emitTape(root_);
}

inline double Program::applyBin(int op, double a, double b) {
    switch (op) {
        case B_ADD: return a + b;  case B_SUB: return a - b;
        case B_MUL: return a * b;  case B_DIV: return a / b;
        case B_MOD: return std::fmod(a, b); case B_POW: return std::pow(a, b);
        case B_LT: return a <  b ? 1.0 : 0.0; case B_LE: return a <= b ? 1.0 : 0.0;
        case B_GT: return a >  b ? 1.0 : 0.0; case B_GE: return a >= b ? 1.0 : 0.0;
        case B_EQ: return a == b ? 1.0 : 0.0; case B_NE: return a != b ? 1.0 : 0.0;
    }
    return 0.0;
}

inline double Program::applyCall(FuncId fid, const double* a, int n) {
    double a0 = n > 0 ? a[0] : 0.0, a1 = n > 1 ? a[1] : 0.0, a2 = n > 2 ? a[2] : 0.0;
    switch (fid) {
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
        case F_STEP: return a1 < a0 ? 0.0 : 1.0;
        case F_LDEXP: return a0 * std::exp2(a1);
        case F_MIN: { double m = a0; for (int i = 1; i < n; ++i) m = std::min(m, a[i]); return m; }
        case F_MAX: { double m = a0; for (int i = 1; i < n; ++i) m = std::max(m, a[i]); return m; }
        case F_CLAMP1: return a0 < 0 ? 0.0 : (a0 > 1 ? 1.0 : a0);
        case F_CLAMP3: return a0 < a1 ? a1 : (a0 > a2 ? a2 : a0);
        case F_LERP: return a0 + (a1 - a0) * a2;
        case F_SMOOTHSTEP: { double t = (a2 - a0) / (a1 - a0); t = t < 0 ? 0 : (t > 1 ? 1 : t); return t * t * (3 - 2 * t); }
        case F_NOISE1: return ev_noise(a0, 0, 0); case F_NOISE2: return ev_noise(a0, a1, 0);
        case F_NOISE3: return ev_noise(a0, a1, a2);
        case F_RANDOM1: return ev_random(a0, 0, 0); case F_RANDOM2: return ev_random(a0, a1, 0);
        case F_RANDOM3: return ev_random(a0, a1, a2);
        case F_FBM: case F_TURB: {
            int oct = (int)a[3]; float lac = (float)a[4], gain = (float)a[5];
            float x0 = (float)a0, y0 = (float)a1, z0 = (float)a2;
            float sum = 0.0f, amp = 1.0f, fr = 1.0f;
            for (int i = 0; i < oct && i < 32; ++i) {
                float v = ev_pnoise3(x0 * fr, y0 * fr, z0 * fr);
                sum += amp * (fid == F_TURB ? std::fabs(v) : v);
                fr *= lac; amp *= gain;
            }
            return (double)sum;
        }
        case F_FROM_SRGB: return ev_from_sRGB(a0); case F_TO_SRGB: return ev_to_sRGB(a0);
        case F_FROM_REC709: return ev_from_rec709(a0); case F_TO_REC709: return ev_to_rec709(a0);
        case F_FROM_BYTE: return ev_from_sRGB(a0 / 255.0); case F_TO_BYTE: return ev_to_sRGB(a0) * 255.0;
        case F_PI: return 3.141592653589793; case F_E: return 2.718281828459045;
    }
    return 0.0;
}

inline double Program::evalTape(const double* vars) const {
    double st[256]; int sp = 0;
    const Instr* T = tape_.data();
    const int N = (int)tape_.size();
    for (int pc = 0; pc < N; ++pc) {
        const Instr& in = T[pc];
        switch (in.op) {
            case OP_NUM: st[sp++] = in.d; break;
            case OP_VAR: st[sp++] = vars[in.i]; break;
            case OP_NEG: st[sp-1] = -st[sp-1]; break;
            case OP_NOT: st[sp-1] = (st[sp-1] == 0.0) ? 1.0 : 0.0; break;
            case OP_JMP: pc = in.i - 1; break;
            case OP_JZ:  { double v = st[--sp]; if (v == 0.0) pc = in.i - 1; } break;
            case OP_JNZ: { double v = st[--sp]; if (v != 0.0) pc = in.i - 1; } break;
            case OP_BIN: { double b = st[--sp], a = st[--sp]; st[sp++] = applyBin(in.i, a, b); } break;
            case OP_CALL: { double r = applyCall((FuncId)in.i, &st[sp - in.n], in.n); sp -= in.n; st[sp++] = r; } break;
        }
    }
    return sp > 0 ? st[sp-1] : 0.0;
}

// ---- transpiler: AST -> C-like kernel body over v[] (see ExprKernel.h) -------
inline void Program::emitNode(int idx, std::string& o) const {
    const Node& n = nodes_[idx];
    switch (n.kind) {
        case N_NUM: {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g", n.num);
            std::string s(buf);
            // make it an unambiguous floating literal ("3" -> "3.0")
            if (s.find_first_of(".eEnN") == std::string::npos) s += ".0";
            o += "(" + s + ")";
            return;
        }
        case N_VAR:
            o += "v[" + std::to_string(n.var) + "]";
            return;
        case N_UNARY:
            if (n.op == '-') { o += "(-("; emitNode(n.a, o); o += "))"; }
            else             { o += "(("; emitNode(n.a, o); o += ")==0.0?1.0:0.0)"; } // '!'
            return;
        case N_TERNARY:
            o += "(("; emitNode(n.a, o); o += ")!=0.0?(";
            emitNode(n.b, o); o += "):("; emitNode(n.c, o); o += "))";
            return;
        case N_BINARY: {
            const char* cmp = nullptr;
            switch (n.op) {
                case B_ADD: o += "("; emitNode(n.a,o); o += "+"; emitNode(n.b,o); o += ")"; return;
                case B_SUB: o += "("; emitNode(n.a,o); o += "-"; emitNode(n.b,o); o += ")"; return;
                case B_MUL: o += "("; emitNode(n.a,o); o += "*"; emitNode(n.b,o); o += ")"; return;
                case B_DIV: o += "("; emitNode(n.a,o); o += "/"; emitNode(n.b,o); o += ")"; return;
                case B_MOD: o += "fmod("; emitNode(n.a,o); o += ","; emitNode(n.b,o); o += ")"; return;
                case B_POW: o += "pow(";  emitNode(n.a,o); o += ","; emitNode(n.b,o); o += ")"; return;
                case B_LT: cmp = "<";  break; case B_LE: cmp = "<="; break;
                case B_GT: cmp = ">";  break; case B_GE: cmp = ">="; break;
                case B_EQ: cmp = "=="; break; case B_NE: cmp = "!="; break;
                case B_AND:
                    o += "((("; emitNode(n.a,o); o += ")!=0.0)&&(("; emitNode(n.b,o); o += ")!=0.0)?1.0:0.0)"; return;
                case B_OR:
                    o += "((("; emitNode(n.a,o); o += ")!=0.0)||(("; emitNode(n.b,o); o += ")!=0.0)?1.0:0.0)"; return;
            }
            o += "(("; emitNode(n.a,o); o += ")"; o += cmp; o += "("; emitNode(n.b,o); o += ")?1.0:0.0)";
            return;
        }
        case N_CALL: {
            // emit a plain call `fn(arg0,arg1,...)`
            auto call = [&](const char* fn) {
                o += fn; o += "(";
                for (size_t i = 0; i < n.args.size(); ++i) { if (i) o += ","; emitNode(n.args[i], o); }
                o += ")";
            };
            // emit `fn(arg0,arg1,arg2)` padding missing args with 0.0 (noise/random)
            auto call3 = [&](const char* fn) {
                o += fn; o += "(";
                for (int i = 0; i < 3; ++i) {
                    if (i) o += ",";
                    if (i < (int)n.args.size()) emitNode(n.args[i], o); else o += "0.0";
                }
                o += ")";
            };
            switch (n.fid) {
                case F_SIN: call("sin"); return;   case F_COS: call("cos"); return;
                case F_TAN: call("tan"); return;   case F_ASIN: call("asin"); return;
                case F_ACOS: call("acos"); return; case F_ATAN: call("atan"); return;
                case F_SINH: call("sinh"); return; case F_COSH: call("cosh"); return;
                case F_TANH: call("tanh"); return; case F_EXP: call("exp"); return;
                case F_LOG: call("log"); return;   case F_LOG10: call("log10"); return;
                case F_LOG2: call("exh_log2"); return; case F_SQRT: call("sqrt"); return;
                case F_ABS: call("fabs"); return;  case F_FLOOR: call("floor"); return;
                case F_CEIL: call("ceil"); return; case F_TRUNC: call("exh_trunc"); return;
                case F_RINT: call("exh_round"); return; case F_ROUND: call("exh_round"); return;
                case F_SIGN: call("exh_sign"); return;
                case F_RADIANS: o += "(("; emitNode(n.args[0],o); o += ")*0.017453292519943295)"; return;
                case F_DEGREES: o += "(("; emitNode(n.args[0],o); o += ")*57.29577951308232)"; return;
                case F_EXPONENT: call("exh_exponent"); return; case F_MANTISSA: call("exh_mantissa"); return;
                case F_POW2: call("exh_pow2"); return;
                case F_POW: call("pow"); return;   case F_ATAN2: call("atan2"); return;
                case F_FMOD: call("fmod"); return; case F_HYPOT: call("hypot"); return;
                case F_STEP: call("exh_step"); return; case F_LDEXP: call("exh_ldexp"); return;
                case F_MIN: case F_MAX: {
                    const char* fn = (n.fid == F_MIN) ? "fmin" : "fmax";
                    int k = (int)n.args.size();
                    for (int i = 0; i < k - 1; ++i) { o += fn; o += "("; emitNode(n.args[i], o); o += ","; }
                    emitNode(n.args[k - 1], o);
                    for (int i = 0; i < k - 1; ++i) o += ")";
                    return;
                }
                case F_CLAMP1: call("exh_clamp1"); return; case F_CLAMP3: call("exh_clamp3"); return;
                case F_LERP: call("exh_lerp"); return; case F_SMOOTHSTEP: call("exh_smoothstep"); return;
                case F_NOISE1: case F_NOISE2: case F_NOISE3: call3("exh_noise"); return;
                case F_RANDOM1: case F_RANDOM2: case F_RANDOM3: call3("exh_random"); return;
                case F_FBM: call("exh_fbm"); return; case F_TURB: call("exh_turb"); return;
                case F_FROM_SRGB: call("exh_from_srgb"); return; case F_TO_SRGB: call("exh_to_srgb"); return;
                case F_FROM_REC709: call("exh_from_rec709"); return; case F_TO_REC709: call("exh_to_rec709"); return;
                case F_FROM_BYTE: call("exh_from_byte"); return; case F_TO_BYTE: call("exh_to_byte"); return;
                case F_PI: o += "(3.141592653589793)"; return; case F_E: o += "(2.718281828459045)"; return;
            }
            o += "0.0";
            return;
        }
    }
    o += "0.0";
}

} // namespace expreval
#endif // EXPR_EVAL_H
