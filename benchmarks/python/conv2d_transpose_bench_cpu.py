import argparse
import math
import time

import mlx.core as mx
import numpy as np
import torch

N_warmup = 1
N_iter_bench = 10
N_iter_func = 5


def bench(f, a, b):
    for i in range(N_warmup):
        f(a, b)

    s = time.perf_counter_ns()
    for i in range(N_iter_bench):
        f(a, b)
    e = time.perf_counter_ns()
    return (e - s) * 1e-9


def make_mx_conv_transpose_2D(strides=(1, 1), padding=(0, 0), groups=1):
    def mx_conv_transpose_2D(a, b):
        ys = []
        for i in range(N_iter_func):
            y = mx.conv_transpose2d(
                a, b, stride=strides, padding=padding, groups=groups, stream=mx.cpu
            )
            ys.append(y)
        mx.eval(ys)
        return ys

    return mx_conv_transpose_2D


def make_pt_conv_transpose_2D(strides=(1, 1), padding=(0, 0), groups=1):
    @torch.no_grad()
    def pt_conv_transpose_2D(a, b):
        ys = []
        for i in range(N_iter_func):
            y = torch.conv_transpose2d(
                a, b, stride=strides, padding=padding, groups=groups
            )
            ys.append(y)
        return ys

    return pt_conv_transpose_2D


def bench_shape(N, H, W, C, kH, kW, O, strides, padding, groups, np_dtype):
    scale = 1.0 / math.sqrt(kH * kH * C)
    a_np = np.random.uniform(0, 0.5, (N, H, W, C)).astype(np_dtype)
    b_np = np.random.uniform(-scale, scale, (int(O / groups), kH, kW, C)).astype(
        np_dtype
    )

    a_mx = mx.array(a_np)
    b_mx = mx.array(b_np)

    a_pt = torch.from_numpy(a_np.transpose((0, 3, 1, 2))).to("cpu")
    b_pt = torch.from_numpy(b_np.transpose((3, 0, 1, 2))).to("cpu")

    f_mx = make_mx_conv_transpose_2D(strides, padding, groups)
    f_pt = make_pt_conv_transpose_2D(strides, padding, groups)

    time_torch = bench(f_pt, a_pt, b_pt)
    time_mlx = bench(f_mx, a_mx, b_mx)

    out_mx = mx.conv_transpose2d(
        a_mx, b_mx, stride=strides, padding=padding, groups=groups, stream=mx.cpu
    )
    out_pt = torch.conv_transpose2d(
        a_pt.to("cpu"), b_pt.to("cpu"), stride=strides, padding=padding, groups=groups
    )
    out_pt = torch.permute(out_pt, (0, 2, 3, 1))
    out_pt = out_pt.numpy(force=True)

    atol = 2e-5 if np_dtype == np.float32 else 1e-4

    if not np.allclose(out_pt, out_mx, atol=atol):
        print(
            f"Failed at {(N, H, W, C)}, {(O, kH, kW, C)} [strides = {strides}, padding = {padding}, groups = {groups}] with max(|a - b|) = {np.max(np.abs(out_pt - out_mx))}"
        )

    return time_mlx, time_torch


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run conv benchmarks")

    dtypes = ("float32",)
    shapes = (
        (4, 32, 32, 32, 5, 5, 32, (1, 1), (2, 2), 1),
        (4, 32, 32, 64, 5, 5, 64, (1, 1), (2, 2), 1),
        (4, 32, 32, 128, 5, 5, 128, (1, 1), (2, 2), 1),
        (4, 32, 32, 256, 5, 5, 256, (1, 1), (2, 2), 1),
        (4, 32, 32, 512, 5, 5, 512, (1, 1), (2, 2), 1),
        (4, 64, 64, 32, 5, 5, 32, (1, 1), (2, 2), 1),
        (4, 64, 64, 64, 5, 5, 64, (1, 1), (2, 2), 1),
        (4, 64, 64, 128, 5, 5, 128, (1, 1), (2, 2), 1),
        (4, 64, 64, 256, 5, 5, 256, (1, 1), (2, 2), 1),
        (4, 128, 128, 32, 5, 5, 32, (1, 1), (2, 2), 1),
        (4, 128, 128, 64, 5, 5, 64, (1, 1), (2, 2), 1),
        (4, 128, 128, 128, 5, 5, 128, (1, 1), (2, 2), 1),
        (4, 256, 256, 32, 5, 5, 3, (1, 1), (2, 2), 1),
        (4, 256, 256, 3, 5, 5, 32, (1, 1), (2, 2), 1),
        (4, 128, 128, 64, 5, 5, 3, (1, 1), (2, 2), 1),
        (4, 128, 128, 3, 5, 5, 64, (1, 1), (2, 2), 1),
    )

    for dtype in dtypes:
        print(
            "(N,   H,   W,   C), (  O, kH, kW,   C),   dtype, stride,   pads,  groups, diff%"
        )
        for N, H, W, C, kH, kW, O, strides, padding, groups in shapes:
            np_dtype = getattr(np, dtype)
            time_mlx, time_torch = bench_shape(
                N, H, W, C, kH, kW, O, strides, padding, groups, np_dtype
            )
            diff = time_torch / time_mlx - 1.0

            print(
                f"({N}, {H:3d}, {W:3d}, {C:3d}), ({O:3d}, {kH:2d}, {kW:2d}, {C:3d}), {dtype}, {strides}, {padding}, {groups:7d}, {100. * diff:+5.2f}%"
            )
            if time_mlx >= 2.0 * time_torch:
                print("ATTENTION ^^^^^^^")
