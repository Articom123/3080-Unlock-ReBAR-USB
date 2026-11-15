# Quick benchmark test for the tensor unlock

These results are pretty much standard for using the tensor unlock on the RTX 3080, this is mainly just to show that the tensor unlock does work alongside ReBAR. For Qwen3.8 Flash Next IQ3_S on Strata with a 131040 token context window with the GPU at stock:

| Prompt Tokens | TTFT stock ms | TTFT unlocked ms | Prefill stock tok/s | Prefill unlocked tok/s | Gain |
| --- | --- | --- | --- | --- | --- |
| 8192 | 3876.9 | 3167.7 | 2126.2 | 2609.8 | 22.7% |
| 16384 | 7296.6 | 6130.0 | 2258.5 | 2689.8 | 19.1% |
| 32768 | 14664.9 | 12123.2 | 2242.0 | 2713.4 | 21.0% |
| 65536 | 29894.8 | 24452.3 | 2197.6 | 2689.1 | 22.4% |
| 131040 | 62319.1 | 51501.3 | 2106.3 | 2550.1 | 21.1% |


Furthermore, running the tensor test:

LOCKED:
- FP16 (FP32 accumulate) 61.4 TFLOPS
- BF16 (FP32 accumulate) 59.6 TFLOPS
- TF32 30.5 TFLOPS
- FP32 (control) 20.8 TFLOPS

UNLOCKED:
- FP16 (FP32 accumulate) 102.6 TFLOPS
- BF16 (FP32 accumulate) 104.9 TFLOPS
- TF32 49.0 TFLOPS
- FP32 (control) 21.0 TFLOPS
