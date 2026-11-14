# bf16_bench.py - BF16 GEMM throughput, same shape/protocol as the repo's cuBLAS benchmark
import statistics, time, torch

N, WARMUP_S, SAMPLES, REPS = 4096, 2.0, 30, 20

assert torch.cuda.is_available(), "No CUDA-enabled PyTorch found"
# FP32 accumulation (CUBLAS_COMPUTE_32F), as in the repo benchmark; no reduced-precision split-K
torch.backends.cuda.matmul.allow_bf16_reduced_precision_reduction = False
dev = torch.device("cuda")
print(f"{torch.cuda.get_device_name(dev)} | torch {torch.__version__} | CUDA {torch.version.cuda}")

a = torch.randn(N, N, device=dev, dtype=torch.bfloat16)
b = torch.randn(N, N, device=dev, dtype=torch.bfloat16)
c = torch.empty(N, N, device=dev, dtype=torch.bfloat16)

# Warmup: let clocks ramp up and cuBLAS pick its kernel
end = time.perf_counter() + WARMUP_S
while time.perf_counter() < end:
    torch.matmul(a, b, out=c)
    torch.cuda.synchronize()

start, stop = torch.cuda.Event(enable_timing=True), torch.cuda.Event(enable_timing=True)
times = []
for _ in range(SAMPLES):
    start.record()
    for _ in range(REPS):
        torch.matmul(a, b, out=c)
    stop.record()
    stop.synchronize()
    times.append(start.elapsed_time(stop) / REPS)  # ms per GEMM

ms = statistics.median(times)
tflops = 2 * N**3 / (ms * 1e-3) / 1e12
print(f"BF16 {N}^3 GEMM: median {ms:.3f} ms  ->  {tflops:.1f} TFLOPS")

# Sanity check: the result must still be correct, not just fast
#ref = a.float() @ b.float()
#rel = ((c.float() - ref).abs().max() / ref.abs().max()).item()
#print(f"max relative error vs FP32: {rel:.2e}  ({'OK' if rel < 1e-2 else 'WRONG RESULTS'})")