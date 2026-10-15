// FastMath.cpp: the scalar tanPi, sinPi and cosPi (01 §7.1, §7.3; FC 01 §5.1, FC K2 #14). They run per control tick
// (SVF g = tanPi(fc/fs), the tilt prewarp), so they are out of line; everything else in FastMath is inline in
// FastMath.h.
//
// Seeded from FCompressor, then changed in one way, as 01 §7.3 says: the reductions and the two polynomials
// (sinPiPoly, cosPiPoly) moved into FastMath.h, where the vector forms use them, and each scalar function here returns
// lane 0 of its vector form. The sequence of fsdsp::simd ops per lane is the seed's, so the bits are the seed's:
// dsp.fastmath's seed.{tanpi,sinpi,cospi}.hash rows hold them to FCompressor's blessed hashes.
//
// Both reduce to y in [0, 1/4] with exact reflections (1 - a for a in [1/2, 1] and 1/2 - b for b in [1/4, 1/2] are
// Sterbenz-exact), then use
//   sin(pi y) = y * (s0 + s1 y^2 + s2 y^4 + s3 y^6)      relative error 3.2e-9 (minimax fit)
//   cos(pi y) = 1 + y^2 * (c1 + c2 y^2 + c3 y^4 + c4 y^6)  relative error 6.4e-11
// so the measured errors (dsp.fastmath) are float rounding: sinPi/cosPi 8.4e-8 absolute and 1.3e-7 relative, tanPi
// 2.0e-7 relative over [0, 0.499]; all three monotone where the function is. Exact values: sinPi(0) = sinPi(+-1) = 0,
// sinPi(+-1/2) = +-1, cosPi(0) = 1, cosPi(+-1/2) = 0, cosPi(+-1) = -1, tanPi(0) = 0; sinPi is odd and cosPi even,
// exactly.
#include "fsdsp/core/FastMath.h"

namespace fsdsp {

float tanPi(float x) noexcept FSDSP_NONBLOCKING { return simd::lane<0>(fsdsp::tanPi(simd::set1(x))); }
float sinPi(float x) noexcept FSDSP_NONBLOCKING { return simd::lane<0>(fsdsp::sinPi(simd::set1(x))); }
float cosPi(float x) noexcept FSDSP_NONBLOCKING { return simd::lane<0>(fsdsp::cosPi(simd::set1(x))); }

} // namespace fsdsp
