// Quick CPU eval benchmark for the tree-walk evaluator (ExprEval.h).
// Build: g++ -std=c++11 -O3 -I ../ofx/Expression bench_expr.cpp -o bench_expr && ./bench_expr
// Simulates a full-frame, single-thread render for a few representative presets so
// we can see absolute per-pixel eval throughput WITHOUT a host. Single thread on
// purpose: this isolates the eval cost from host band parallelism.
#include "ExprEval.h"
#include <cstdio>
#include <chrono>
#include <vector>
#include <string>
using namespace expreval;

enum { V_R=0,V_G,V_B,V_A,V_X,V_Y,V_CX,V_CY,V_WIDTH,V_HEIGHT,V_FRAME };

struct Case { const char* name; const char* r; const char* g; const char* b; const char* a; const char* vars; };

// representative: a trivial passthrough, a mid grade, and the heavy Plasma preset.
static Case cases[] = {
  { "passthrough",   "r","g","b","a", "" },
  { "gamma",         "pow(r,1/k1)","pow(g,1/k1)","pow(b,1/k1)","a", "" },
  { "plasma",
    "0.5 + 0.5*sin(v)","0.5 + 0.5*sin(v + 2.0944)","0.5 + 0.5*sin(v + 4.1888)","1",
    "v = sin(cx*k1*3) + sin(cy*k1*3 + t*0.1) + sin((cx+cy)*k1*2) + sin(hypot(cx,cy)*k1*4 - t*0.13)" },
  { "clouds_fbm",    "c","c","c","1",
    "n = fBm(cx*k1, cy*k1, t*0.05, 5, 2, 0.5); c = clamp(n*0.5 + 0.5)" },
};

int main() {
  const int W = 1920, H = 1080;
  std::vector<std::string> names = {"r","g","b","a","x","y","cx","cy","width","height","frame",
                                    "k1","k2","k3","k4","ref.r","ref.g","ref.b"};
  for (auto& cs : cases) {
    // build like buildContext: derived (vars) then channels
    std::vector<std::string> nm = names;
    std::vector<std::pair<int,Program>> derived;
    std::string err;
    // parse "name = formula; ..." block
    std::string vars = cs.vars;
    size_t pos=0;
    while (pos < vars.size()) {
      size_t nl=vars.find('\n',pos), sc=vars.find(';',pos);
      size_t end=(nl<sc)?nl:sc;
      std::string line=vars.substr(pos,(end==std::string::npos)?std::string::npos:end-pos);
      pos=(end==std::string::npos)?vars.size():end+1;
      size_t a=line.find_first_not_of(" \t"); if(a==std::string::npos) continue;
      size_t eq=line.find('=',a); if(eq==std::string::npos) continue;
      std::string name=line.substr(a,eq-a);
      while(!name.empty()&&(name.back()==' '||name.back()=='\t')) name.pop_back();
      std::string expr=line.substr(eq+1);
      int slot=(int)nm.size(); nm.push_back(name);
      Program p; if(!p.compile(expr,nm,err)){printf("compile fail %s: %s\n",cs.name,err.c_str());return 1;}
      derived.push_back({slot,p});
    }
    Program ch[4];
    const char* src[4]={cs.r,cs.g,cs.b,cs.a};
    for(int c=0;c<4;++c) if(!ch[c].compile(src[c],nm,err)){printf("compile fail %s ch%d: %s\n",cs.name,c,err.c_str());return 1;}

    std::vector<double> v(nm.size(),0.0);
    v[V_WIDTH]=W; v[V_HEIGHT]=H; v[V_FRAME]=12;
    v[11]=3; // k1
    const double halfW=W*0.5, halfH=H*0.5, invHalfW=1.0/halfW;

    // ---- correctness: tape must match tree-walk bit-for-bit over a sample grid
    long mism=0; double maxdiff=0;
    for(int y=0;y<H;y+=37){
      for(int x=0;x<W;x+=37){
        v[V_R]=0.5;v[V_G]=0.25;v[V_B]=0.1;v[V_A]=1.0;v[V_X]=x;v[V_Y]=y;
        v[V_CX]=((double)x-halfW)*invHalfW; v[V_CY]=((double)y-halfH)*invHalfW;
        for(auto& d:derived) v[d.first]=d.second.eval(&v[0]);
        for(int c=0;c<4;++c){ double e=ch[c].eval(&v[0]),t=ch[c].evalTape(&v[0]);
          if(e!=t){ double dd=std::fabs(e-t); if(dd>maxdiff)maxdiff=dd; ++mism; } }
      }
    }

    auto run=[&](bool tape)->double{
      volatile double sink=0;
      auto t0=std::chrono::steady_clock::now();
      for(int y=0;y<H;++y){
        for(int x=0;x<W;++x){
          v[V_R]=0.5; v[V_G]=0.25; v[V_B]=0.1; v[V_A]=1.0;
          v[V_X]=x; v[V_Y]=y;
          v[V_CX]=((double)x-halfW)*invHalfW;
          v[V_CY]=((double)y-halfH)*invHalfW;
          if(tape){ for(auto& d:derived) v[d.first]=d.second.evalTape(&v[0]);
            sink+=ch[0].evalTape(&v[0])+ch[1].evalTape(&v[0])+ch[2].evalTape(&v[0])+ch[3].evalTape(&v[0]); }
          else    { for(auto& d:derived) v[d.first]=d.second.eval(&v[0]);
            sink+=ch[0].eval(&v[0])+ch[1].eval(&v[0])+ch[2].eval(&v[0])+ch[3].eval(&v[0]); }
        }
      }
      auto t1=std::chrono::steady_clock::now();
      (void)sink;
      return std::chrono::duration<double,std::milli>(t1-t0).count();
    };
    double msTree=run(false), msTape=run(true);
    printf("%-12s tree %7.1f ms   tape %7.1f ms   %.2fx   [%s]\n",
           cs.name, msTree, msTape, msTree/msTape,
           mism==0 ? "exact" : "MISMATCH");
    if(mism) printf("   !! %ld mismatches, maxdiff=%g\n", mism, maxdiff);
  }
  return 0;
}
