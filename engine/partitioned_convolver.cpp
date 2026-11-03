// Uniform-partitioned FFT convolution. See partitioned_convolver.h.

#include "partitioned_convolver.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

namespace neiro {
namespace {

constexpr double kPi = 3.14159265358979323846;

void fft(std::vector<std::complex<double>>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * kPi / static_cast<double>(len) *
                           (inverse ? 1.0 : -1.0);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t j = 0; j < len / 2; ++j) {
                const std::complex<double> u = a[i + j];
                const std::complex<double> v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse) {
        for (auto& v : a) v /= static_cast<double>(n);
    }
}

}  // namespace

struct PartitionedConvolver::Impl {
    static constexpr size_t B = PartitionedConvolver::blockSize();
    static constexpr size_t N = PartitionedConvolver::fftSize();
    static constexpr size_t MaxParts = PartitionedConvolver::maxPartitions();

    size_t channels = 0;
    size_t partitions = 0;
    bool ready = false;

    // One block of N complex values per channel per partition.
    std::vector<std::complex<double>> irFreq;     // channels * MaxParts * N
    std::vector<std::complex<double>> freqDelay;  // channels * MaxParts * N
    size_t delayPos = 0;

    std::vector<double> inputPrev;  // channels * B
    std::vector<double> inputAcc;   // channels * B
    size_t inputFill = 0;

    std::vector<double> output;  // channels * B, one block
    size_t outputRead = 0;
    bool haveOutput = false;

    std::vector<std::complex<double>> work;

    void init(size_t ch) {
        channels = ch;
        irFreq.assign(ch * MaxParts * N, std::complex<double>(0.0, 0.0));
        freqDelay.assign(ch * MaxParts * N, std::complex<double>(0.0, 0.0));
        inputPrev.assign(ch * B, 0.0);
        inputAcc.assign(ch * B, 0.0);
        output.assign(ch * B, 0.0);
        work.assign(N, std::complex<double>(0.0, 0.0));
        delayPos = 0;
        inputFill = 0;
        outputRead = 0;
        haveOutput = false;
    }

    void clearState() {
        std::fill(freqDelay.begin(), freqDelay.end(), std::complex<double>(0.0, 0.0));
        std::fill(inputPrev.begin(), inputPrev.end(), 0.0);
        std::fill(inputAcc.begin(), inputAcc.end(), 0.0);
        std::fill(output.begin(), output.end(), 0.0);
        delayPos = 0;
        inputFill = 0;
        outputRead = 0;
        haveOutput = false;
    }

    void buildChannel(size_t c, const std::vector<float>& ir) {
        std::complex<double>* dst = irFreq.data() + c * MaxParts * N;
        if (ir.empty()) {
            // Identity: a delta at tap 0. It must start where a real IR starts,
            // because the onset of a causal IR is tap 0 and the block latency
            // is added on top of it. A delta at B/2 would answer 128 samples
            // later than a filtered channel.
            std::fill(work.begin(), work.end(), std::complex<double>(0.0, 0.0));
            work[0] = std::complex<double>(1.0, 0.0);
            fft(work, false);
            std::copy(work.begin(), work.end(), dst);
            return;
        }
        for (size_t p = 0; p < partitions; ++p) {
            std::fill(work.begin(), work.end(), std::complex<double>(0.0, 0.0));
            const size_t start = p * B;
            const size_t count = start < ir.size() ? std::min(B, ir.size() - start) : 0;
            for (size_t i = 0; i < count; ++i) {
                work[i] = std::complex<double>(static_cast<double>(ir[start + i]), 0.0);
            }
            fft(work, false);
            std::copy(work.begin(), work.end(), dst + p * N);
        }
    }

    void runBlock() {
        // Spectrum of [previous block, current block], per channel.
        for (size_t c = 0; c < channels; ++c) {
            std::fill(work.begin(), work.end(), std::complex<double>(0.0, 0.0));
            const double* prev = inputPrev.data() + c * B;
            const double* acc = inputAcc.data() + c * B;
            for (size_t i = 0; i < B; ++i) {
                work[i] = std::complex<double>(prev[i], 0.0);
                work[B + i] = std::complex<double>(acc[i], 0.0);
            }
            fft(work, false);
            std::copy(work.begin(), work.end(),
                      freqDelay.data() + (c * MaxParts + delayPos) * N);
        }

        for (size_t c = 0; c < channels; ++c) {
            std::fill(work.begin(), work.end(), std::complex<double>(0.0, 0.0));
            const std::complex<double>* ir = irFreq.data() + c * MaxParts * N;
            for (size_t p = 0; p < partitions; ++p) {
                const size_t slot = (delayPos + MaxParts - p) % MaxParts;
                const auto* x = freqDelay.data() + (c * MaxParts + slot) * N;
                const auto* h = ir + p * N;
                for (size_t k = 0; k < N; ++k) {
                    work[k] += x[k] * h[k];
                }
            }
            fft(work, true);
            double* out = output.data() + c * B;
            for (size_t i = 0; i < B; ++i) out[i] = work[B + i].real();
            std::copy(inputAcc.begin() + static_cast<std::ptrdiff_t>(c * B),
                      inputAcc.begin() + static_cast<std::ptrdiff_t>((c + 1) * B),
                      inputPrev.begin() + static_cast<std::ptrdiff_t>(c * B));
            std::fill(inputAcc.begin() + static_cast<std::ptrdiff_t>(c * B),
                      inputAcc.begin() + static_cast<std::ptrdiff_t>((c + 1) * B), 0.0);
        }
        delayPos = (delayPos + 1) % MaxParts;
        inputFill = 0;
        outputRead = 0;
    }
};

PartitionedConvolver::PartitionedConvolver() : impl_(new Impl) {}
PartitionedConvolver::~PartitionedConvolver() = default;

bool PartitionedConvolver::configure(const std::vector<float>& left,
                                     const std::vector<float>& right) {
    // An empty left with a non-empty right would build a silently dead left
    // channel; refuse the pair instead of returning a working-looking filter.
    if (left.empty() && !right.empty()) return false;
    if (left.empty() && right.empty()) return false;
    // A mono IR applies to both channels; that is the documented meaning of an
    // empty `right`, and both channels are built so neither is left silent.
    return configure({left, right.empty() ? left : right});
}

bool PartitionedConvolver::configure(const std::vector<std::vector<float>>& perChannel) {
    if (perChannel.empty()) return false;
    size_t taps = 0;
    for (const auto& ir : perChannel) taps = std::max(taps, ir.size());
    if (taps == 0) return false;
    const size_t partitions = (taps + blockSize() - 1) / blockSize();
    if (partitions > maxPartitions()) return false;

    impl_->init(perChannel.size());
    impl_->partitions = partitions;
    for (size_t c = 0; c < perChannel.size(); ++c) {
        impl_->buildChannel(c, perChannel[c]);
    }
    impl_->ready = true;
    return true;
}

size_t PartitionedConvolver::channels() const { return impl_->channels; }

void PartitionedConvolver::reset() {
    if (!impl_->ready) return;
    // Clear the running state only. The IR spectra stay; rebuilding them would
    // silence a convolver that is already configured.
    impl_->clearState();
}

void PartitionedConvolver::process(float* interleaved, size_t frames, size_t channels) {
    if (!impl_->ready || channels == 0) return;
    const size_t nch = std::min(channels, impl_->channels);
    for (size_t f = 0; f < frames; ++f) {
        // Capture the input before the output overwrites the buffer.
        for (size_t c = 0; c < nch; ++c) {
            impl_->inputAcc[c * Impl::B + impl_->inputFill] =
                static_cast<double>(interleaved[f * channels + c]);
        }

        // Emit the block completed on the previous block boundary. The block
        // produced at this one is emitted next: exactly B samples of latency.
        for (size_t c = 0; c < nch; ++c) {
            const double y = impl_->haveOutput ? impl_->output[c * Impl::B + impl_->outputRead]
                                               : 0.0;
            interleaved[f * channels + c] = static_cast<float>(y);
        }
        if (impl_->haveOutput) {
            if (impl_->outputRead + 1 == Impl::B) {
                impl_->haveOutput = false;
                impl_->outputRead = 0;
            } else {
                ++impl_->outputRead;
            }
        }

        if (impl_->inputFill + 1 == Impl::B) {
            impl_->runBlock();
            impl_->haveOutput = true;
            impl_->outputRead = 0;
        } else {
            ++impl_->inputFill;
        }
    }
}

}  // namespace neiro
