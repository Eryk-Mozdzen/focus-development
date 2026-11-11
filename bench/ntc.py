#!/usr/bin/env -S uv run

# /// script
# dependencies = [
#     "numpy",
#     "scipy",
# ]
# ///

import numpy as np
import scipy.optimize

# NTC110 10K
R0 = 10_000
T0 = 273.15 + 25
B = 3950

T = np.linspace(10, 100, 200) + 273.15
Rx = R0 * np.exp(B * ((1 / T) - (1 / T0)))


# Rx NTC on top
# Rf fixed on bottom
# Uout / U_in = R / (R + Rx)
def resolution(Rf):
    dRx_dT = -R0 * B * (1 / T**2) * np.exp(B * ((1 / T) - (1 / T0)))
    dU_dRx = -Rf / (Rf + Rx) ** 2
    dU_dT = dU_dRx * dRx_dT
    return np.abs(dU_dT).min()


res = scipy.optimize.minimize_scalar(
    lambda Rf: -resolution(Rf),
    bounds=(100, 100_000),
    method="bounded",
)

print(res.x, resolution(res.x).min())
