#include "../OptiScaler/dlssnr/PassProfiles.h"
#include <cassert>
#include <cstdio>
#include <limits>
int main()
{
    Config cfg;
    cfg.DlssNrLocalStructure=2.0f;
    cfg.DlssNrSkinStructure=0.84f;
    for (unsigned pass=0;pass<30;++pass) {
        auto off=DlssNr::Profiles::PassTuning(cfg,pass);
        assert(off.structure==2.0f && off.skin==0.84f);
    }
    cfg.DlssNrSkinIndependent=true;
    for (unsigned pass=0;pass<30;++pass) {
        auto on=DlssNr::Profiles::PassTuning(cfg,pass);
        assert(on.structure==1.0f && on.skin==0.84f);
    }
    cfg.DlssNrPass2AutoMask=false;
    cfg.DlssNrPass3LocalStructure=0.4f;
    cfg.DlssNrPass3SkinStructure=0.2f;
    cfg.DlssNrExtraPasses[0].autoMask=false;
    cfg.DlssNrExtraPasses[1].skin=0.0f;
    assert(DlssNr::Profiles::PassTuning(cfg,1).structure==2.0f);
    auto third=DlssNr::Profiles::PassTuning(cfg,2);
    assert(third.structure==0.4f && third.skin==0.2f);
    assert(DlssNr::Profiles::PassTuning(cfg,3).structure==2.0f);
    assert(DlssNr::Profiles::PassTuning(cfg,4).skin==0.0f);
    cfg.DlssNrSkinStructure=-1.0f;
    assert(DlssNr::Profiles::PassTuning(cfg,0).skin==-1.0f);
    cfg.DlssNrLocalStructure=std::numeric_limits<float>::quiet_NaN();
    assert(DlssNr::Profiles::PassTuning(cfg,0).structure==1.0f);
    cfg.DlssNrLocalStructure=2.0f;
    cfg.DlssNrSkinIndependent=false;
    assert(DlssNr::Profiles::PassTuning(cfg,0).structure==2.0f);
    assert(cfg.DlssNrLocalStructure.value()==2.0f);
    std::puts("PASS production PassTuning: all 30 passes, inheritance, per-pass mask/skin, sentinel, finite guard, toggle restore");
}

