"""直接调用正式 Slang 采样函数，独立数值积分核对 PDF 归一化和样本矩。"""

import json
from pathlib import Path

import falcor
import numpy as np


ROOT = Path(__file__).resolve().parents[2]
SAMPLE_COUNT = 262144


def run():
    device = falcor.Device(type=falcor.DeviceType.Vulkan)
    compute = falcor.ComputePass(
        device,
        file=str(ROOT / "Source/MyRenderPasses/SignificanceCache/Tests/Lobes.cs.slang"),
        cs_entry="main",
    )
    flags = falcor.ResourceBindFlags.ShaderResource | falcor.ResourceBindFlags.UnorderedAccess
    buffer = device.create_structured_buffer(16, SAMPLE_COUNT, flags)
    compute.root_var["gResults"] = buffer
    compute.root_var["CB"]["gCount"] = SAMPLE_COUNT
    # 确定性半球积分与 GPU 拒绝采样独立，检查的是实际着色器结果。
    z = (np.arange(1024) + 0.5) / 1024
    phi = (np.arange(2048) + 0.5) * (2 * np.pi / 2048)
    x = np.sqrt(1 - z * z)[:, None] * np.cos(phi)[None, :]
    results = []
    for exponent in (1, 2, 3, 8, 32, 64, 128):
        for cosine in (0.001, 0.25, 0.7, 1.0):
            compute.root_var["CB"]["gExponent"] = exponent
            compute.root_var["CB"]["gCosine"] = cosine
            compute.execute(SAMPLE_COUNT)
            samples = buffer.to_numpy().view(np.float32).reshape(-1, 4)
            assert np.isfinite(samples).all() and np.all(samples[:, 3] == 0)
            mass = float(samples[0, 2])
            dot = np.maximum(np.sqrt(1 - cosine * cosine) * x + cosine * z[:, None], 0)
            density = dot ** exponent * (exponent + 1) / mass
            integral = float(np.mean(density))
            expected_z = float(np.mean(density * z[:, None]) / integral)
            expected_dot = float(np.mean(density * dot) / integral)
            moments = np.mean(samples[:, :2], axis=0, dtype=np.float64)
            assert abs(integral - 1) < 0.0015, (exponent, cosine, integral)
            assert np.max(np.abs(moments - [expected_z, expected_dot])) < 0.004, (exponent, cosine, moments)
            results.append({
                "exponent": exponent, "axis_cosine": cosine, "mass": mass,
                "pdf_integral": integral, "sample_mean_z": float(moments[0]), "expected_mean_z": expected_z,
                "sample_mean_axis_cosine": float(moments[1]), "expected_mean_axis_cosine": expected_dot,
            })
    device.wait()
    output = ROOT / "build/significance-cache-experiment/lobe-validation.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({"samples_per_case": SAMPLE_COUNT, "cases": results}, indent=2), encoding="utf-8")
    print(f"{len(results)} 组 GPU 波瓣 PDF 和采样矩验证通过：{output}")


if __name__ == "__main__":
    run()

elif "m" in globals():
    import traceback
    try:
        run()
    except Exception:
        traceback.print_exc()
        falcor.exit(1)
    else:
        falcor.exit()
