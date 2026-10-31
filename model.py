#!/usr/bin/env -S uv run

# /// script
# dependencies = [
#     "numpy",
#     "pandas",
#     "scipy",
#     "matplotlib",
# ]
# ///

import pandas as pd
import numpy as np
import scipy
import matplotlib.pyplot as plt
import sys


def curve_fit(model, x_data, y_data, **kwargs):
    x_data = np.asarray(x_data, dtype=float)
    y_data = np.asarray(y_data, dtype=float)

    keep = np.isfinite(x_data) & np.isfinite(y_data)
    x_data = x_data[keep]
    y_data = y_data[keep]

    while True:
        try:
            popt, pcov = scipy.optimize.curve_fit(model, x_data, y_data, **kwargs)
        except RuntimeError:
            break

        residuals = y_data - model(x_data, *popt)
        sigma = residuals.std(ddof=1)

        mask = np.abs(residuals) <= (3 * sigma)

        if mask.all():
            break

        keep[np.where(keep)[0][~mask]] = False

        x_data = x_data[mask].copy()
        y_data = y_data[mask].copy()

    return popt, pcov, keep


df = pd.read_csv(sys.argv[1], parse_dates=["timestamp"]).dropna()

w = np.linspace(0, max(df["true_velocity"]), 100)

[kf], _, kf_keep = curve_fit(
    lambda w, k: k * (w**2), df["true_velocity"], df["true_thrust"]
)
[kt], _, kt_keep = curve_fit(
    lambda w, k: k * (w**2), df["true_velocity"], df["true_torque"]
)

print(f"kf = {kf:e}")
print(f"kt = {kt:e}")

plt.figure()
plt.plot(w, kf * (w**2), label="model", c="red")
plt.scatter(
    df["true_velocity"].iloc[kf_keep],
    df["true_thrust"].iloc[kf_keep],
    label="samples ok",
    c="black",
    s=1,
)
plt.autoscale()
xlim = plt.xlim()
ylim = plt.ylim()
plt.scatter(
    df["true_velocity"].iloc[~kf_keep],
    df["true_thrust"].iloc[~kf_keep],
    label="samples rejected",
    c="red",
    s=4,
)
plt.xlabel("angular velocity [rad/s]")
plt.ylabel("propeller thrust [N]")
plt.xlim(xlim)
plt.ylim(ylim)
plt.grid()
plt.legend()

plt.figure()
plt.plot(w, kt * (w**2), label="model", c="red")
plt.scatter(
    df["true_velocity"].iloc[kt_keep],
    df["true_torque"].iloc[kt_keep],
    label="samples ok",
    c="black",
    s=1,
)
plt.autoscale()
xlim = plt.xlim()
ylim = plt.ylim()
plt.scatter(
    df["true_velocity"].iloc[~kt_keep],
    df["true_torque"].iloc[~kt_keep],
    label="samples rejected",
    c="red",
    s=4,
)
plt.xlabel("angular velocity [rad/s]")
plt.ylabel("propeller torque [Nm]")
plt.xlim(xlim)
plt.ylim(ylim)
plt.grid()
plt.legend()

rotor_radius = (0.5 * float(sys.argv[2])) * 0.0254
rotor_field = np.pi * (rotor_radius**2)

# TODO: measure pressure and humidity
dry_air_gas_constant = 287.05
air_pressure = 101325  # sea level
[air_temperature], _, _ = curve_fit(
    lambda _, temperature: temperature,
    df["timestamp"],
    df["true_temperature"],
)
air_density = air_pressure / (dry_air_gas_constant * (air_temperature + 273.15))

print(f"     air temperature {air_temperature:6.3f} deg C")
print(f"         air density {air_density:6.3f} km/m^3")

df["power_electrical"] = df["true_voltage"].abs() * df["true_current"].abs()
df["power_mechanical"] = df["true_torque"].abs() * df["true_velocity"].abs()
df["power_induced"] = df["true_thrust"].abs() * np.sqrt(
    df["true_thrust"].abs() / (2 * air_density * rotor_field)
)
df["drive_efficiency"] = df["power_mechanical"] / df["power_electrical"]
df["propeller_efficiency"] = df["power_induced"] / df["power_mechanical"]


def drive_eff_model(torque, a, b, c):
    return (a * torque) / ((a * torque) + b + (c * (torque**2)))


eff_param, _, eff_keep = curve_fit(
    drive_eff_model,
    df["true_torque"].abs(),
    df["drive_efficiency"],
)
eff_max_tt = np.sqrt(eff_param[1] / eff_param[2])

print(
    f"    drive efficiency {100*drive_eff_model(eff_max_tt, *eff_param):.0f}% (peak) for {eff_max_tt:.5f} Nm"
)

t = np.linspace(0, max(df["true_torque"].abs().max(), eff_max_tt), 100)

plt.figure()
plt.plot(t, 100 * drive_eff_model(t, *eff_param), c="red", label="model")
plt.axvline(x=eff_max_tt, color="red", linestyle="dashed")
plt.axhline(
    y=100 * drive_eff_model(eff_max_tt, *eff_param),
    color="red",
    linestyle="dashed",
    label="peak",
)
plt.scatter(
    df["true_torque"].iloc[eff_keep].abs(),
    100 * df["drive_efficiency"].iloc[eff_keep],
    label="samples ok",
    c="black",
    s=1,
)
plt.scatter(
    df["true_torque"].iloc[~eff_keep].abs(),
    100 * df["drive_efficiency"].iloc[~eff_keep],
    label="samples rejected",
    c="red",
    s=4,
)
plt.xlabel("torque [Nm]")
plt.ylabel("drive efficiency [%]")
plt.xlim([0, None])
plt.ylim([0, 100])
plt.grid()
plt.legend()

[eff], _, eff_keep = curve_fit(
    lambda _, eta: eta,
    df["true_velocity"],
    df["propeller_efficiency"],
)
print(f"propeller efficiency {100*eff:.0f}%")
plt.figure()
plt.plot(w, 100 * eff * np.ones_like(w), c="red", label="model")
plt.scatter(
    df["true_velocity"].iloc[eff_keep],
    100 * df["propeller_efficiency"].iloc[eff_keep],
    label="samples ok",
    c="black",
    s=1,
)
plt.scatter(
    df["true_velocity"].iloc[~eff_keep],
    100 * df["propeller_efficiency"].iloc[~eff_keep],
    label="samples rejected",
    c="red",
    s=4,
)
plt.xlabel("angular velocity [rad/s]")
plt.ylabel("propeller efficiency [%]")
plt.xlim([0, None])
plt.ylim([0, 100])
plt.grid()
plt.legend()

model2, _, model2_keep = curve_fit(
    lambda x, a, b: (a * x) + b, df["setpoint_torque"], df["true_torque"]
)
torque = np.linspace(df["setpoint_torque"].min(), df["setpoint_torque"].max(), 100)
print(f"torque setpoint slope {model2[0]:.3f}")

plt.figure()
plt.plot(torque, torque, label="model y=x+b", c="red", linestyle="--")
plt.plot(torque, (model2[0] * torque) + model2[1], label="model y=ax+b", c="red")
plt.scatter(
    df["setpoint_torque"].iloc[model2_keep],
    df["true_torque"].iloc[model2_keep],
    label="samples ok",
    c="black",
    s=1,
)
plt.autoscale()
xlim = plt.xlim()
ylim = plt.ylim()
plt.scatter(
    df["setpoint_torque"].iloc[~model2_keep],
    df["true_torque"].iloc[~model2_keep],
    label="samples rejected",
    c="red",
    s=4,
)
plt.xlabel("setpoint torque [Nm]")
plt.ylabel("true torque [Nm]")
plt.xlim(xlim)
plt.ylim(ylim)
plt.grid()
plt.legend()

prediction = pd.DataFrame({"desired thrust": [5, 10, 15, 20]})
prediction["motor velocity"] = np.sqrt(prediction["desired thrust"] / kf)
prediction["motor torque"] = (kt / kf) * prediction["desired thrust"]
prediction["power mechanical"] = (
    prediction["motor velocity"] * prediction["motor torque"]
)
prediction["drive efficiency"] = drive_eff_model(prediction["motor torque"], *eff_param)
prediction["power electrical"] = (
    prediction["power mechanical"] / prediction["drive efficiency"]
)
prediction["battery current"] = prediction["power electrical"] / 14.8
print(
    prediction.to_string(
        formatters={
            "desired thrust": lambda x: f"{x:.0f} N",
            "motor velocity": lambda x: f"{x:.0f} rad/s",
            "motor torque": lambda x: f"{x:.2f} Nm",
            "power mechanical": lambda x: f"{x:.0f} W",
            "drive efficiency": lambda x: f"{100*x:.0f}%",
            "power electrical": lambda x: f"{x:.0f} W",
            "battery current": lambda x: f"{x:.0f} A",
        },
        index=False,
    )
)

plt.show()
