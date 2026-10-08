"""First GPU check: are float32 results bit-identical to the CPU's?

Kinoko's CPU build does plain IEEE float32 operations (no fused multiply-add). A GPU only reproduces that if the compiler
is told not to fuse (`--fmad=false`) and keeps correctly rounded division and square root. This runs a few kernels on a
million random values with fusing off and on and compares the raw bits with numpy (which uses the same SSE operations
as the engine).

    python tools/gpu_smoke.py
"""

import numpy as np

import cupy as cp

SRC = r'''
extern "C" __global__ void ops(const float* a, const float* b, const float* c, float* out_madd, float* out_div,
                               float* out_sqrt, float* out_dot, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float x = a[i], y = b[i], z = c[i];
    out_madd[i] = x * y + z;                   // the pattern that gets fused into one rounding when fusing is allowed
    out_div[i] = x / y;
    out_sqrt[i] = sqrtf(fabsf(x));
    out_dot[i] = x * y + y * z + z * x;        // a small dot product, like the engine's vector math
}
'''


def run(fmad):
    opts = ("--std=c++17", f"--fmad={'true' if fmad else 'false'}")
    kernel = cp.RawModule(code=SRC, options=opts).get_function("ops")
    rng = np.random.default_rng(1)
    n = 1_000_000
    a, b, c = (rng.standard_normal(n).astype(np.float32) * np.float32(37.0) for _ in range(3))
    b += np.float32(0.01) * np.sign(b)  # keep divisors away from zero
    outs = [cp.zeros(n, dtype=cp.float32) for _ in range(4)]
    kernel(((n + 255) // 256,), (256,), (cp.asarray(a), cp.asarray(b), cp.asarray(c), *outs, np.int32(n)))
    gpu = [cp.asnumpy(o) for o in outs]
    with np.errstate(all="ignore"):
        cpu = [a * b + c, a / b, np.sqrt(np.abs(a)), (a * b + b * c) + c * a]
    names = ["x*y+z", "x/y", "sqrt(|x|)", "x*y+y*z+z*x"]
    for name, g, ref in zip(names, gpu, cpu):
        bad = int((g.view(np.uint32) != ref.view(np.uint32)).sum())
        print(f"  fmad={str(fmad):5s}  {name:12s}  bit-identical to CPU on {n - bad:>8d} / {n}   ({'OK' if bad == 0 else 'DIFFERS'})")


if __name__ == "__main__":
    dev = cp.cuda.Device(0)
    props = cp.cuda.runtime.getDeviceProperties(0)
    print(f"GPU: {props['name'].decode()}   {props['totalGlobalMem'] / 2**30:.1f} GiB   compute capability {dev.compute_capability}")
    print(f"CuPy {cp.__version__}, CUDA runtime {cp.cuda.runtime.runtimeGetVersion()}, driver {cp.cuda.runtime.driverGetVersion()}")
    run(False)
    run(True)
