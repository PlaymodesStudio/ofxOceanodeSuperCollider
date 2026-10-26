#include "PhysicalModes.h"
#include <cassert>
#include <iostream>
using namespace physical;
int main(){
 Body body;Spots spots;
 for(double shape:{0.,.37,1.})for(int a=0;a<NFAMILIES;++a)for(int b=0;b<NFAMILIES;++b){
  body.shape=shape;
  for(int endpoint:{a,b}){
   FamilyWeights w{};w[endpoint]=1;body.family=endpoint;
   assert(modeCount(body,w)==modeCount(body));
   for(int k=0;k<MAXM;++k){auto old=mode(k,body),now=mode(k,body,w);assert(old.on==now.on && old.ratio==now.ratio && old.t60==now.t60 && old.amp==now.amp);if(old.on){auto x=weights(old,spots),y=weights(now,spots);assert(x.e==y.e&&x.p1==y.p1&&x.p2==y.p2&&x.c==y.c);}}
  }
  for(int k=0;k<MAXM;++k){
   FamilyWeights left{},right{};left[a]+=.500001;left[b]+=.499999;right[a]+=.499999;right[b]+=.500001;
   body.family=a;auto x=mode(k,body,left);body.family=b;auto y=mode(k,body,right);
   assert(x.on==y.on);if(!x.on)continue;
   assert(std::isfinite(x.ratio)&&std::isfinite(y.ratio)&&std::abs(std::log(x.ratio/y.ratio))<.0001);
   assert(std::isfinite(x.t60)&&std::isfinite(y.t60)&&std::abs(std::log(x.t60/y.t60))<.0001);
   assert(std::abs(x.amp-y.amp)<.001);
   const auto p=weights(x,spots),q=weights(y,spots);assert(std::abs(p.e-q.e)<.001&&std::abs(p.p1-q.p1)<.001&&std::abs(p.p2-q.p2)<.001&&std::abs(p.c-q.c)<.001);
  }
 }
 std::cout<<"PASS: 36 body-family pairs x 3 shapes x 48 modes; exact legacy endpoints and continuous frequency/decay/gain/pickup coefficients at .5\n";
}
