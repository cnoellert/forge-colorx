// Unit tests for the self-contained expression evaluator (ofx/Expression/ExprEval.h).
// Build:  g++ -std=c++11 -O2 -I ../ofx/Expression test_expr.cpp -o test_expr && ./test_expr
// CI runs this from the repo root (see .github/workflows/ci.yml).

#include "ExprEval.h"
#include <cstdio>
#include <cmath>
using namespace expreval;

static int fails = 0;

static void check(const char* expr, double expect) {
    std::vector<std::string> names = {"r","g","b","a","x","y","cx","cy","width","height","frame"};
    std::vector<double> v(names.size(), 0.0);
    v[0]=0.5; v[1]=0.25; v[2]=0.1; v[3]=1.0; v[4]=100; v[5]=200; v[8]=1920; v[9]=1080; v[10]=12;
    Program p; std::string err;
    if (!p.compile(expr, names, err)) { printf("COMPILE FAIL [%s]: %s\n", expr, err.c_str()); fails++; return; }
    double got = p.eval(&v[0]);
    bool ok = std::fabs(got - expect) < 1e-6;
    printf("%-40s = %-14g (expect %-14g) %s\n", expr, got, expect, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

static void checkErr(const char* expr) {
    std::vector<std::string> names = {"r","g","b"};
    Program p; std::string err;
    bool c = p.compile(expr, names, err);
    printf("%-40s -> %s\n", expr, c ? "compiled (UNEXPECTED)" : ("rejected: " + err).c_str());
    if (c) fails++;
}

int main() {
    check("r+g", 0.75);
    check("1-r", 0.5);
    check("pow(r,2)", 0.25);
    check("r*2+1", 2.0);
    check("-2^2", -4.0);                 // ^ binds tighter than unary -
    check("2^3^2", 512.0);               // right associative
    check("clamp(2)", 1.0);
    check("clamp(-1)", 0.0);
    check("clamp(r,0,0.4)", 0.4);
    check("min(r,g,b)", 0.1);
    check("max(r,g,b)", 0.5);
    check("lerp(0,10,0.5)", 5.0);
    check("mix(0,10,0.25)", 2.5);
    check("r>0.4?1:0", 1.0);
    check("r<0.4?1:0", 0.0);
    check("r>0.4 && g>0.4 ? 1 : 0", 0.0);
    check("r>0.4 || g>0.4 ? 1 : 0", 1.0);
    check("x/width", 100.0/1920.0);
    check("hypot(3,4)", 5.0);
    check("fmod(7,3)", 1.0);
    check("abs(-3)", 3.0);
    check("floor(2.9)", 2.0);
    check("ceil(2.1)", 3.0);
    check("trunc(-2.7)", -2.0);
    check("sign(-5)", -1.0);
    check("pi", M_PI);
    check("pi()", M_PI);
    check("degrees(pi)", 180.0);
    check("radians(180)", M_PI);
    check("step(0.3,0.5)", 1.0);
    check("step(0.6,0.5)", 0.0);
    check("smoothstep(0,1,0.5)", 0.5);
    check("2*(r+g)", 1.5);
    check("", 0.0);                      // empty -> 0 (Nuke behaviour)
    check("pow2(3)", 9.0);
    check("atan2(1,1)", M_PI/4.0);

    checkErr("r + nope");                // unknown variable
    checkErr("foo(1)");                  // unknown function
    checkErr("clamp(1,2)");              // wrong argument count
    checkErr("(1+2");                    // unbalanced parens

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
