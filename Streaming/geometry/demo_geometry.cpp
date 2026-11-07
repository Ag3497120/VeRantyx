#include "mvr_geometry.hpp"
#include <iostream>
#include <iomanip>
#include <string>
using namespace mvr::geometry;
static void axis(const AxisMap& a){std::cout<<"{\"sourcePixels\":"<<a.sourcePixels<<",\"packedPixels\":"<<a.packedPixels<<",\"sourceBegin\":"<<a.sourceBegin<<",\"sourceEnd\":"<<a.sourceEnd<<",\"packedBegin\":"<<a.packedBegin<<",\"packedEnd\":"<<a.packedEnd<<"}";}
int main(int argc,char**argv){try{
 CropOptions crop;FoveationOptions foveation;crop.enabled=true;foveation.enabled=true;
 Size base{2064,2208};
 for(int i=1;i<argc;i++){std::string a=argv[i];if(a=="--no-crop")crop.enabled=false;else if(a=="--no-foveation")foveation.enabled=false;else throw std::invalid_argument("Usage: demo_geometry [--no-crop] [--no-foveation]");}
 std::array<Fov,2> native{{{-.94,.76,.88,-.84},{-.76,.94,.88,-.84}}};
 auto p=makePlan(native,base,crop,foveation,1,1);
 std::cout<<std::setprecision(12)<<"{\"protocolVersion\":"<<p.protocolVersion<<",\"session\":"<<p.session<<",\"revision\":"<<p.revision<<",\"features\":"<<p.features<<",\"baseWidth\":"<<base.width<<",\"baseHeight\":"<<base.height<<",\"eyes\":[";
 for(int e=0;e<2;e++){auto& v=p.eyes[e];auto& r=v.render;if(e)std::cout<<',';
 std::cout<<"{\"renderWidth\":"<<r.size.width<<",\"renderHeight\":"<<r.size.height<<",\"fov\":{\"left\":"<<r.fov.left<<",\"right\":"<<r.fov.right<<",\"up\":"<<r.fov.up<<",\"down\":"<<r.fov.down<<"},\"cropRect\":{\"x\":"<<r.sourceRect.x<<",\"y\":"<<r.sourceRect.y<<",\"width\":"<<r.sourceRect.width<<",\"height\":"<<r.sourceRect.height<<"},\"x\":";axis(v.x);std::cout<<",\"y\":";axis(v.y);std::cout<<'}';}
 std::cout<<"]}\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
