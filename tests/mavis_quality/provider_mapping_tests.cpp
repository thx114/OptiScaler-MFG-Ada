#define NOMINMAX
#define MFGUNLOCK_LOCAL_LOW_OVERHEAD
#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include "../../OptiScaler/framegen/dlssg/mavis/blackwell.hpp"
#include "../../OptiScaler/framegen/dlssg/mavis/thin_geometry.hpp"
void check(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
int wmain(int argc,wchar_t** argv) { try {
 using namespace mfgunlock;
 check(argc==2,"provide a local NVIDIA DLSSG DLL path");
 std::vector<blackwell::Patch> patches;std::vector<void*> allocations;
 blackwell::Result result;std::string detail;
 check(!blackwell::Apply(GetModuleHandleW(nullptr),patches,allocations,result,detail,true),"unknown provider must fail closed");
 check(patches.empty() && allocations.empty(),"unknown provider must not mutate");
 // Map the provider as an image without running its DllMain or any GPU code.
 HMODULE provider=LoadLibraryExW(argv[1],nullptr,DONT_RESOLVE_DLL_REFERENCES);
 check(provider!=nullptr,"map provider");
 blackwell::g_adaptive_quality_enabled=true;
 blackwell::g_adaptive_quality_profile=adaptivequality::Profile::kLuminanceDirectionalV3;
 blackwell::g_adaptive_quality_v3_temporal_geometry=false;
 blackwell::g_adaptive_quality_v3_inpaint_mode=0;
 check(blackwell::Apply(provider,patches,allocations,result,detail,true,blackwell::SilhouetteGuardMode::Balanced,true),"quality cubin apply");
 check(result.motion_vector && result.inpaint && result.inpaint_decision,"all three quality roles");
 check(result.adaptive_geometry_version==adaptivequality::ComponentVersion::kV3,"Local Stable V3 geometry");
 check(result.adaptive_inpaint_version==adaptivequality::ComponentVersion::kV2,"V2 Compatibility inpaint");
 check(result.adaptive_geometry_variant==blackwell::AdaptiveGeometryVariant::kLocal,"no confidence history");
 std::vector<thingeometry::Redirect> warp;thingeometry::Result warpResult;std::string version;
 thingeometry::g_adaptive_quality_enabled=true;
 thingeometry::g_adaptive_quality_profile=adaptivequality::Profile::kLuminanceDirectionalV3;
 thingeometry::Options options;options.validated_warp_blend=true;
 check(thingeometry::Apply(provider,options,warp,warpResult,version),"validated warp apply");
 check(warpResult.validated_warp_blend.applied,"validated warp effective");
 thingeometry::Restore(warp);blackwell::Restore(patches,allocations);
 FreeLibrary(provider);
 std::cout<<"PASS: exact mapped 310.9 provider, Local Stable geometry/V2 inpaint/warp, unsupported fail-closed, restore\n";
 return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
