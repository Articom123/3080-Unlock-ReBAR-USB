"""CUDA GEMM throughput for tensor operations and an IEEE FP32 control."""
import statistics
import time

import torch

N, WARMUP_S, SAMPLES, REPS = 4096, 2.0, 30, 20


def benchmark(dtype, device):
    a = torch.randn(N, N, device=device, dtype=dtype)
    b = torch.randn(N, N, device=device, dtype=dtype)
    c = torch.empty(N, N, device=device, dtype=dtype)

    # Let clocks ramp up and cuBLAS choose its kernel before timing.
    end = time.perf_counter() + WARMUP_S
    while time.perf_counter() < end:
        torch.matmul(a, b, out=c)
        torch.cuda.synchronize()

    start = torch.cuda.Event(enable_timing=True)
    stop = torch.cuda.Event(enable_timing=True)
    times = []
    for _ in range(SAMPLES):
        start.record()
        for _ in range(REPS):
            torch.matmul(a, b, out=c)
        stop.record()
        stop.synchronize()
        times.append(start.elapsed_time(stop) / REPS)
    ms = statistics.median(times)
    return 2 * N**3 / (ms * 1e-3) / 1e12


def main():
    if not torch.cuda.is_available():
        raise SystemExit("No CUDA-enabled PyTorch found")
    if not torch.cuda.is_bf16_supported():
        raise SystemExit("This benchmark requires a CUDA GPU with BF16 support")

    device = torch.device("cuda")
    print(f"{torch.cuda.get_device_name(device)} | torch {torch.__version__} | CUDA {torch.version.cuda}", flush=True)
    matmul = torch.backends.cuda.matmul
    # Avoid mixing the newer precision API with the legacy allow_tf32 API.
    modern_precision = hasattr(matmul, "fp32_precision")
    precision_key = "fp32_precision" if modern_precision else "allow_tf32"
    saved = {precision_key: getattr(matmul, precision_key)}
    for key in ("allow_fp16_reduced_precision_reduction",
                "allow_bf16_reduced_precision_reduction", "allow_fp16_accumulation"):
        if hasattr(matmul, key):
            saved[key] = getattr(matmul, key)
            setattr(matmul, key, False)

    try:
        with torch.inference_mode():
            for label, dtype, tf32 in (
                ("FP16 (FP32 accumulate)", torch.float16, False),
                ("BF16 (FP32 accumulate)", torch.bfloat16, False),
                ("TF32", torch.float32, True),
                ("FP32 (control)", torch.float32, False),
            ):
                setattr(matmul, precision_key, ("tf32" if tf32 else "ieee") if modern_precision else tf32)
                # TF32 permits tensor-core kernels for FP32 inputs. The control
                # disables TF32 so it measures IEEE FP32 at the same GEMM shape.
                result = benchmark(dtype, device)
                print(f"{label} {result:.1f} TFLOPS", flush=True)
    finally:
        for key, value in saved.items():
            setattr(matmul, key, value)


if __name__ == "__main__":
    main()
