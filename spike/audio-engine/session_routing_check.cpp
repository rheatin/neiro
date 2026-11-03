#include "nae.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* label, const std::string& detail = {}) {
    if (!ok) ++failures;
    std::printf("  [%s] %-46s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
}
void put16(std::vector<uint8_t>* b, uint16_t v){b->push_back(v&0xff);b->push_back(v>>8);}
void put32(std::vector<uint8_t>* b, uint32_t v){for(int i=0;i<4;++i)b->push_back((v>>(8*i))&0xff);}
bool write51(const char* path){
    constexpr uint32_t rate=48000, frames=rate*2;
    std::vector<uint8_t> w;
    w.insert(w.end(),{'R','I','F','F'});put32(&w,36+frames*6*2);
    w.insert(w.end(),{'W','A','V','E'});w.insert(w.end(),{'f','m','t',' '});
    put32(&w,16);put16(&w,1);put16(&w,6);put32(&w,rate);put32(&w,rate*6*2);
    put16(&w,12);put16(&w,16);w.insert(w.end(),{'d','a','t','a'});put32(&w,frames*6*2);
    for(uint32_t i=0;i<frames;++i){
        const double s=0.25*std::sin(2.0*3.14159265358979323846*1000.0*i/rate);
        for(int c=0;c<6;++c){const int16_t v=(c==4||c==5)?static_cast<int16_t>(s*32767.0):0;put16(&w,static_cast<uint16_t>(v));}
    }
    FILE* f=std::fopen(path,"wb");if(!f)return false;const bool ok=std::fwrite(w.data(),1,w.size(),f)==w.size();std::fclose(f);return ok;
}
}  // namespace

int main(int argc, char** argv) {
    const char* path = "session_routing_51.wav";
    if (!write51(path)) return 1;
    NAE_SessionConfig cfg{};
    cfg.structSize=sizeof(cfg); cfg.path=path; cfg.outputMode=NAE_OUTPUT_DEVICE;
    cfg.quality=NAE_QUALITY_HIGH; cfg.shareMode=NAE_SHARE_EXCLUSIVE; cfg.sliceMs=16;
    NAE_OutputPolicy policy{};
    NAE_OutputPolicyDefaults(&policy);
    policy.ratePolicy=NAE_RATE_DEVICE;
    policy.routingMode=NAE_ROUTING_DOWNMIX;
    policy.downmixPreset=NAE_DOWNMIX_LTRT;
    policy.targetChannelMask=NAE_LAYOUT_STEREO;
    policy.ditherMode=NAE_DITHER_OFF;
    policy.bassManagement=NAE_BASS_OFF;
    policy.clipGuardMode=NAE_CLIP_GUARD_OFF;
    const char* device = argc > 1 ? argv[1] : nullptr;
    cfg.deviceId = device;
    NAE_Session* s = nullptr;
    const int32_t rc=NAE_SessionOpenWithPolicy(&cfg,&policy,&s);
    check(rc==NAE_OK && s!=nullptr,"routed session opens",std::to_string(rc));
    if(rc!=NAE_OK||!s){std::printf("%s\n",NAE_GetLastError());return 1;}
    NAE_SessionState st{}; st.structSize=sizeof(st);
    check(NAE_SessionGetState(s,&st)==NAE_OK,"state readable");
    check(st.fileChannels==6,"file channels 6",std::to_string(st.fileChannels));
    check(st.channels==2,"device channels 2",std::to_string(st.channels));
    NAE_ResolvedSignalPath r{}; r.structSize=sizeof(r);
    check(NAE_SessionGetResolvedSignalPath(s,&r)==NAE_OK,"resolved path readable");
    check(r.sourceChannels==6 && r.channels==2,"resolved 6->2",
          std::to_string(r.sourceChannels)+"->"+std::to_string(r.channels));
    check(r.channelConversion==1,"channel conversion reported");
    check(r.bassManagement==NAE_BASS_AUTO || r.bassManagement==NAE_BASS_OFF,
          "bass management is reported");
    check(r.bitPerfect==0,"routed path not bit-perfect");
    bool routingWarning=false;
    for(uint32_t i=0;i<r.diagnosticCount;++i)
        if(std::strstr(r.diagnostics[i].message,"Explicit routing")) routingWarning=true;
    check(!routingWarning,"no routing warning when applied");
    NAE_SessionClose(s);
    std::remove(path);
    std::printf("\n%s\n",failures==0?"All session routing checks passed.":"Some session routing checks FAILED.");
    return failures==0?0:1;
}
