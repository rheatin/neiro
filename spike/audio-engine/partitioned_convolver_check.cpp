#include "partitioned_convolver.h"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* label, const std::string& detail = {}) {
    std::printf("[%s] %-52s %s\n", ok ? "PASS" : "FAIL", label, detail.c_str());
    if (!ok) ++failures;
}

std::vector<float> direct(const std::vector<float>& x, const std::vector<float>& h) {
    std::vector<float> y(x.size(), 0.0f);
    for (size_t n = 0; n < x.size(); ++n) {
        double sum = 0.0;
        for (size_t k = 0; k < h.size() && k <= n; ++k) {
            sum += static_cast<double>(h[k]) * x[n - k];
        }
        y[n] = static_cast<float>(sum);
    }
    return y;
}

// Compare a delayed by `latency` against b. The convolver emits one block
// later than it consumes, so every comparison has to shift the reference.
double maxDiff(const std::vector<float>& a, const std::vector<float>& b,
               size_t latency, size_t from = 0) {
    double worst = 0.0;
    for (size_t i = from; i + latency < a.size() && i < b.size(); ++i) {
        worst = std::max(worst, std::fabs(static_cast<double>(a[i + latency]) - b[i]));
    }
    return worst;
}

}  // namespace

int main() {
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

    // Impulse IR: output must be the delayed input, sample for sample.
    {
        std::vector<float> ir(1, 0.5f);
        std::vector<float> x(2000);
        for (auto& v : x) v = dist(rng);
        std::vector<float> expected = direct(x, ir);
        neiro::PartitionedConvolver conv;
        check(conv.configure(ir, {}), "impulse IR configures");
        std::vector<float> y = x;
        conv.process(y.data(), y.size(), 1);
        const size_t latency = neiro::PartitionedConvolver::blockSize();
        check(maxDiff(y, expected, latency, 4) < 1e-5,
              "impulse IR reproduced after one block", std::to_string(maxDiff(y, expected, latency, 4)));
    }

    // 1000-tap random IR against direct convolution.
    {
        std::vector<float> ir(1000);
        for (auto& v : ir) v = dist(rng) * 0.01f;
        std::vector<float> x(8000);
        for (auto& v : x) v = dist(rng);
        std::vector<float> expected = direct(x, ir);
        neiro::PartitionedConvolver conv;
        check(conv.configure(ir, {}), "1000-tap IR configures");
        std::vector<float> y = x;
        conv.process(y.data(), y.size(), 1);
        const size_t latency = neiro::PartitionedConvolver::blockSize();
        const double diff = maxDiff(y, expected, latency, 8);
        check(diff < 1e-4, "1000-tap matches direct convolution", std::to_string(diff));
    }

    // Stereo with different IRs per channel.
    {
        std::vector<float> irL(700), irR(700);
        for (auto& v : irL) v = dist(rng) * 0.01f;
        for (auto& v : irR) v = dist(rng) * 0.01f;
        std::vector<float> input(6000 * 2);
        for (auto& v : input) v = dist(rng);
        std::vector<float> left(6000), right(6000);
        for (size_t i = 0; i < 6000; ++i) {
            left[i] = input[i * 2];
            right[i] = input[i * 2 + 1];
        }
        std::vector<float> expectedL = direct(left, irL);
        std::vector<float> expectedR = direct(right, irR);
        neiro::PartitionedConvolver conv;
        check(conv.configure(irL, irR), "stereo IR configures");
        std::vector<float> y = input;
        conv.process(y.data(), 6000, 2);
        std::vector<float> gotL(6000), gotR(6000);
        for (size_t i = 0; i < 6000; ++i) {
            gotL[i] = y[i * 2];
            gotR[i] = y[i * 2 + 1];
        }
        const size_t latency = neiro::PartitionedConvolver::blockSize();
        check(maxDiff(gotL, expectedL, latency, 8) < 1e-4,
              "stereo left matches direct", std::to_string(maxDiff(gotL, expectedL, latency, 8)));
        check(maxDiff(gotR, expectedR, latency, 8) < 1e-4,
              "stereo right matches direct", std::to_string(maxDiff(gotR, expectedR, latency, 8)));
    }

    // reset() clears the running state but keeps the IR. A fresh instance and a
    // reset one must produce the same output; if reset dropped the IR spectra
    // the second run would be silence.
    {
        std::vector<float> ir(400);
        for (auto& v : ir) v = dist(rng) * 0.02f;
        std::vector<float> x(2000);
        for (auto& v : x) v = dist(rng);
        std::vector<float> sink = x;
        neiro::PartitionedConvolver warm;
        warm.configure(ir, {});
        warm.process(sink.data(), sink.size(), 1);
        warm.reset();
        std::vector<float> after = x;
        warm.process(after.data(), after.size(), 1);

        neiro::PartitionedConvolver fresh;
        fresh.configure(ir, {});
        std::vector<float> expected = x;
        fresh.process(expected.data(), expected.size(), 1);
        check(maxDiff(after, expected, 0) == 0.0, "reset keeps the IR",
              std::to_string(maxDiff(after, expected, 0)));
    }

    // Multichannel: every channel must see its IR. Before the fix process()
    // clamped to two channels, so 3..6 bypassed the wet path entirely.
    {
        std::vector<float> ir(64, 0.0f);
        ir[0] = 0.5f;
        std::vector<std::vector<float>> perChannel(6, ir);
        neiro::PartitionedConvolver conv;
        check(conv.configure(perChannel), "6-channel IR configures");
        check(conv.channels() == 6, "6 channels reported", std::to_string(conv.channels()));
        std::vector<float> x(1024 * 6, 0.5f);
        conv.process(x.data(), 1024, 6);
        double worst = 0.0;
        for (size_t f = 300; f < 1024; ++f) {
            for (size_t c = 0; c < 6; ++c) {
                worst = std::max(worst,
                                 std::fabs(static_cast<double>(x[f * 6 + c]) - 0.25));
            }
        }
        check(worst < 1e-4, "all six channels carry the IR", std::to_string(worst));
    }

    // Per-channel IRs must stay independent: only channel 1 is filtered.
    {
        std::vector<float> ident(64, 0.0f);
        ident[0] = 1.0f;
        std::vector<float> half(64, 0.0f);
        half[0] = 0.5f;
        neiro::PartitionedConvolver conv;
        check(conv.configure({ident, half, ident}), "distinct per-channel IRs configure");
        std::vector<float> x(600 * 3, 0.5f);
        conv.process(x.data(), 600, 3);
        check(std::fabs(x[400 * 3] - 0.5) < 1e-4 && std::fabs(x[400 * 3 + 1] - 0.25) < 1e-4 &&
                  std::fabs(x[400 * 3 + 2] - 0.5) < 1e-4,
              "each channel uses its own IR",
              std::to_string(x[400 * 3]) + " / " + std::to_string(x[400 * 3 + 1]) + " / " +
                  std::to_string(x[400 * 3 + 2]));
    }

    // An empty per-channel entry is an identity at the same latency, not a
    // bypass. Measured by ONSET, not level: a constant input is shift-invariant,
    // so a level check cannot see a delta placed at the wrong tap.
    {
        std::vector<float> ir(64, 0.0f);
        ir[0] = 0.5f;  // causal IR, onset at tap 0
        neiro::PartitionedConvolver conv;
        check(conv.configure({ir, {}, ir}), "identity channel configures");
        std::vector<float> x(700 * 3, 0.0f);
        for (size_t c = 0; c < 3; ++c) x[10 * 3 + c] = 0.5f;
        conv.process(x.data(), 700, 3);
        const size_t latency = neiro::PartitionedConvolver::blockSize();
        size_t onset[3] = {0, 0, 0};
        for (size_t c = 0; c < 3; ++c) {
            double peak = 0.0;
            for (size_t f = 0; f < 700; ++f) {
                const double v = std::fabs(static_cast<double>(x[f * 3 + c]));
                if (v > peak) {
                    peak = v;
                    onset[c] = f;
                }
            }
        }
        check(onset[0] == 10 + latency && onset[1] == 10 + latency &&
                  onset[2] == 10 + latency,
              "filtered and identity channels answer at the same sample",
              std::to_string(onset[0]) + " / " + std::to_string(onset[1]) + " / " +
                  std::to_string(onset[2]) + " (want " + std::to_string(10 + latency) + ")");
    }

    // The two-channel form with an empty `right` is one IR for both channels;
    // it used to build one channel and leave the right silent.
    {
        std::vector<float> ir(64, 0.0f);
        ir[0] = 1.0f;
        neiro::PartitionedConvolver conv;
        check(conv.configure(ir, {}), "mono IR form configures");
        check(conv.channels() == 2, "mono IR form builds two channels",
              std::to_string(conv.channels()));
        std::vector<float> x(1024 * 2, 0.5f);
        conv.process(x.data(), 1024, 2);
        check(std::fabs(x[500 * 2] - 0.5) < 1e-4 && std::fabs(x[500 * 2 + 1] - 0.5) < 1e-4,
              "mono IR reaches both channels",
              std::to_string(x[500 * 2]) + " / " + std::to_string(x[500 * 2 + 1]));
    }

    // Limits.
    {
        neiro::PartitionedConvolver conv;
        check(!conv.configure({}, {}), "empty IR is rejected");
        std::vector<float> big(neiro::PartitionedConvolver::blockSize() *
                                   neiro::PartitionedConvolver::maxPartitions() + 1,
                               0.0f);
        check(!conv.configure(big, {}), "IR over the partition limit is rejected");
        check(!conv.configure({}, {1.0f}), "an empty left with a right is rejected");
    }

    // Irregular block sizes must give the same result as one big call.
    {
        std::vector<float> ir(300);
        for (auto& v : ir) v = dist(rng) * 0.02f;
        std::vector<float> x(4000);
        for (auto& v : x) v = dist(rng);
        neiro::PartitionedConvolver a, b;
        a.configure(ir, {});
        b.configure(ir, {});
        std::vector<float> y1 = x, y2 = x;
        a.process(y1.data(), y1.size(), 1);
        size_t off = 0;
        const size_t chunks[] = {1, 7, 255, 256, 300, 1000, 2181};
        for (size_t n : chunks) {
            b.process(y2.data() + off, n, 1);
            off += n;
        }
        check(maxDiff(y1, y2, 0) == 0.0, "block-size independence", std::to_string(maxDiff(y1, y2, 0)));
    }

    if (failures == 0) {
        std::printf("\nAll partitioned convolver checks passed.\n");
        return 0;
    }
    std::printf("\nSome partitioned convolver checks FAILED.\n");
    return 1;
}
