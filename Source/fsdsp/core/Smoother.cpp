// Smoother.cpp: the prepare() bodies of Smoother4 and LinearRamp (01 §7.1, §7.8; seeded from FCompressor: FC 01 §5.1;
// FC E §4.6; FC K2 #20). They run when an engine is prepared, never per sample or per tick; the tick() members are
// inline in Smoother.h.
#include "fsdsp/core/Smoother.h"

#include "fsdsp/core/Units.h"

namespace fsdsp {

void Smoother4::prepare(float fs, float tauMs, simd::f32x4 epsilon) noexcept FSDSP_NONBLOCKING
{
    a = simd::set1(alphaFromTau(tauMs, fs));
    eps = epsilon;
}

void LinearRamp::prepare(float fs, float ms) noexcept FSDSP_NONBLOCKING
{
    const float msFs = ms * fs;                             // ramp length in samples, times 1000
    step = msFs > 1000.0f ? 1000.0f / msFs : 1.0f;          // shorter than one sample, <= 0 or NaN: instant
}

} // namespace fsdsp
