#include "nae.h"
#include "utf8_args.h"

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
void put16(std::vector<uint8_t>* b, uint16_t v) { b->push_back(v & 0xff); b->push_back(v >> 8); }
void put32(std::vector<uint8_t>* b, uint32_t v) { for (int i=0;i<4;++i) b->push_back((v>>(8*i))&0xff); }
bool write51(const char* path) {
    constexpr uint32_t rate = 48000, frames = rate * 2;
    constexpr uint16_t ch = 6, bits = 16;
    std::vector<uint8_t> w;
    w.insert(w.end(), {'R','I','F','F'}); put32(&w, 36 + frames*ch*2);
    w.insert(w.end(), {'W','A','V','E'});
    w.insert(w.end(), {'f','m','t',' '}); put32(&w, 16); put16(&w,1); put16(&w,ch);
    put32(&w,rate); put32(&w,rate*ch*2); put16(&w,ch*2); put16(&w,bits);
    w.insert(w.end(), {'d','a','t','a'}); put32(&w, frames*ch*2);
    for (uint32_t i=0;i<frames;++i) {
        const double t = i / 48000.0;
        const double s = 0.25 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * t);
        for (int c=0;c<6;++c) {
            const int16_t v = (c==4 || c==5) ? static_cast<int16_t>(s*32767.0) : 0;
            put16(&w, static_cast<uint16_t>(v));
        }
    }
    FILE* f = std::fopen(path,"wb"); if (!f) return false;
    const bool ok = std::fwrite(w.data(),1,w.size(),f)==w.size(); std::fclose(f); return ok;
}
}  // namespace

int main(int argc, char** argv) {
    const auto args = utf8Args(argc, argv);
    const std::string path = "routing_play_51.wav";
    if (!write51(path.c_str())) { std::printf("fixture write failed\n"); return 1; }
    NAE_RoutingMatrix m{};
    check(NAE_BuildRoutingMatrix(NAE_ROUTING_DOWNMIX, NAE_DOWNMIX_LTRT,
                                 NAE_LAYOUT_5_1, NAE_LAYOUT_STEREO, &m) == NAE_OK,
          "LtRt matrix builds");
    NAE_Handle* h = nullptr;
    int32_t rate=0, src=0, ch=0, bits=0, valid=0, flt=0, rs=0, dith=0, perfect=0, fch=0;
    double dur=0, lat=0;
    const char* device = args.size() > 1 ? args[1].c_str() : nullptr;
    const int32_t rc = NAE_OpenFileWithRouting(device, path.c_str(), NAE_OUTPUT_DEVICE,
                                               0, NAE_QUALITY_HIGH, &m, &h,
                                               &rate, &src, &ch, &bits, &valid, &flt,
                                               &rs, &dith, &perfect, &fch, &dur, &lat);
    check(rc == NAE_OK && h != nullptr, "routed open succeeds", std::to_string(rc));
    if (rc != NAE_OK || !h) {
        std::printf("open error: %s\n", NAE_GetLastError());
        return 1;
    }
    check(ch == 2, "routed output is stereo", std::to_string(ch));
    check(fch == 6, "file channels stay 6", std::to_string(fch));
    check(perfect == 0, "routed path is not bit-perfect");
    const int32_t playRc = NAE_Play(h, 200);
    check(playRc == NAE_OK, "routed play succeeds", std::to_string(playRc));
    NAE_Close(h);
    std::remove(path.c_str());
    std::printf("\n%s\n", failures == 0 ? "All routed-play checks passed."
                                        : "Some routed-play checks FAILED.");
    return failures == 0 ? 0 : 1;
}
