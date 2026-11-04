#!/usr/bin/env -S uv run

# /// script
# dependencies = [
#     "asyncio",
#     "websockets",
#     "cobs",
#     "datetime",
#     "PyQt6",
#     "pyqtgraph",
#     "qasync",
# ]
# ///

import asyncio
import websockets
import cobs.cobs
import struct
import datetime
import numpy as np
import pyqtgraph as pg
from PyQt6.QtWidgets import QApplication
from qasync import QEventLoop
from collections import deque
import sys


class Bus:
    def __init__(self):
        self.lock = asyncio.Lock()
        self.values = {
            "true_thrust": None,
            "true_torque": None,
            "true_velocity": None,
            "true_temperature": None,
            "true_voltage": None,
            "true_current": None,
            "focus_voltage": None,
            "focus_velocity": None,
            "setpoint_torque": None,
        }
        self.subscribers = []

    def subscribe(self):
        q = asyncio.Queue()
        self.subscribers.append(q)
        return q

    async def put(self, item):
        timestamp, data = item

        async with self.lock:
            self.values.update(data)
            snapshot = self.values.copy()

        message = (timestamp, snapshot)

        for q in self.subscribers:
            await q.put(message)


class Experiment:
    def __init__(self):
        self.value = 0
        self.value_max = 0.3
        self.step_value = 0.005
        self.step_duration = 2

    async def run(self, queue, focus):
        await focus.set(self.value)
        await asyncio.sleep(3)

        while True:
            if np.abs(self.value) > np.abs(self.value_max):
                return

            self.value += self.step_value

            await focus.set(self.value)
            await queue.put(
                (
                    datetime.datetime.now(datetime.UTC),
                    {
                        "setpoint_torque": self.value,
                    },
                )
            )
            await asyncio.sleep(self.step_duration)


class Focus:
    def __init__(self, ip):
        self.ip = ip

    async def __aenter__(self):
        self.reader, self.writer = await asyncio.open_connection(self.ip, 23)
        self.reader2, self.writer2 = await asyncio.open_connection(self.ip, 8200)

        print((await self.reader.readline()).decode().strip())
        print((await self.reader.readline()).decode().strip())
        print((await self.reader.readline()).decode().strip())

        await self.send("stop")
        await self.send("calib_full")
        await asyncio.sleep(10)

        return self

    async def __aexit__(self, exc_type, exc, tb):
        await self.set(0)
        await self.send("stop")
        self.writer.close()
        await self.writer.wait_closed()
        self.writer2.close()
        await self.writer2.wait_closed()

    async def run(self, queue):
        buffer = bytearray()

        while True:
            data = await self.reader2.read(4096)
            buffer.extend(data)
            delimiter = buffer.find(0)
            if delimiter < 0:
                await asyncio.sleep(0.01)
                continue
            frame = buffer[:delimiter].copy()
            del buffer[: delimiter + 1]
            try:
                decoded = cobs.cobs.decode(frame)
            except Exception as e:
                print(e)
                continue
            if len(decoded) != 9 * 4:
                continue
            values = struct.unpack("<9f", decoded)
            await queue.put(
                (
                    datetime.datetime.now(datetime.UTC),
                    {
                        "focus_voltage": values[2],
                        "focus_velocity": values[1],
                    },
                )
            )

    async def send(self, command, ignore_lines=0):
        message = f"{command}\r\n"
        self.writer.write(message.encode("utf-8"))
        await self.writer.drain()

        for _ in range(ignore_lines):
            print((await self.reader.readline()).decode().strip())
        print((await self.reader.readline()).decode().strip())

    async def set(self, setpoint_torque):
        await self.send(f"tr {setpoint_torque}\r\n", 1)


class Bench:
    def __init__(self, url):
        self.url = url
        self.ws = None

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url)
        await self.ws.send("offset")
        return self

    async def __aexit__(self, exc_type, exc, tb):
        await self.ws.close()

    async def run(self, queue):
        try:
            while True:
                message = await self.ws.recv()

                thrust, torque, velocity, temperature, voltage, current, _, _, _, _ = (
                    struct.unpack("<6f3i1f", message)
                )

                await queue.put(
                    (
                        datetime.datetime.now(datetime.UTC),
                        {
                            "true_thrust": thrust,
                            "true_torque": torque,
                            "true_velocity": velocity,
                            "true_temperature": temperature,
                            "true_voltage": voltage,
                            "true_current": current,
                        },
                    )
                )
        except asyncio.CancelledError:
            raise


class Plotter:
    def __init__(
        self,
        signals,
        max_points=200,
        refresh_rate=10,
        autoscale=True,
    ):
        pg.setConfigOption("background", "w")
        pg.setConfigOption("foreground", "k")
        pg.setConfigOptions(antialias=True)
        self.signals = signals
        self.max_points = max_points
        self.refresh_period = 1.0 / refresh_rate
        self.autoscale = autoscale
        self.time_window = 10
        self.last_draw = 0
        self.start = datetime.datetime.now(datetime.UTC)
        self.win = pg.GraphicsLayoutWidget(show=True, title="Telemetry")
        self.plot = self.win.addPlot()
        self.values = {}
        if isinstance(signals, tuple):
            self.mode = "scatter"
            self.values[signals[0]] = deque(maxlen=max_points)
            self.values[signals[1]] = deque(maxlen=max_points)
            self.scatter = pg.ScatterPlotItem(size=6)
            self.plot.addItem(self.scatter)
            self.plot.setLabel("bottom", signals[0])
            self.plot.setLabel("left", signals[1])
        else:
            self.plot.addLegend()
            colors = [
                "#1f77b4",
                "#d62728",
                "#2ca02c",
                "#ff7f0e",
                "#9467bd",
                "#17becf",
            ]
            self.mode = "line"
            self.lines = []
            for i, signal in enumerate(signals):
                self.values[signal] = deque(maxlen=max_points)
                line = self.plot.plot(
                    pen=pg.mkPen(colors[i % len(colors)], width=2),
                    name=signal,
                )
                self.lines.append(line)
            self.plot.setLabel("bottom", "time [s]")
        self.plot.showGrid(x=True, y=True)
        if not autoscale:
            self.plot.enableAutoRange(False, False)

    def _timestamp_to_float(self, timestamp):
        if isinstance(timestamp, datetime.datetime):
            return (timestamp - self.start).total_seconds()
        return float(timestamp)

    async def run(self, queue):
        try:
            while True:
                timestamp, data = await queue.get()
                if self.mode == "line":
                    t = self._timestamp_to_float(timestamp)
                    for signal in self.signals:
                        value = data.get(signal)
                        if value is not None:
                            self.values[signal].append((t, value))
                else:
                    x = data.get(self.signals[0])
                    y = data.get(self.signals[1])
                    if x is not None and y is not None:
                        self.values[self.signals[0]].append(x)
                        self.values[self.signals[1]].append(y)
                now = asyncio.get_event_loop().time()
                if now - self.last_draw < self.refresh_period:
                    continue
                self.last_draw = now
                self.update()
        except asyncio.CancelledError:
            raise

    def update(self):
        if self.mode == "line":
            latest_time = None
            for line, signal in zip(self.lines, self.signals):
                points = self.values[signal]
                if not points:
                    continue
                arr = np.asarray(points, dtype=np.float64)
                line.setData(arr[:, 0], arr[:, 1])
                latest_time = arr[-1, 0]
            if latest_time is not None:
                self.plot.setXRange(
                    max(0, latest_time - self.time_window), latest_time, padding=0
                )
        else:
            x = np.asarray(self.values[self.signals[0]], dtype=np.float64)
            y = np.asarray(self.values[self.signals[1]], dtype=np.float64)
            if len(x):
                self.scatter.setData(x, y)


class Recorder:
    def __init__(self):
        filepath = f"recording_{datetime.datetime.now(datetime.UTC).strftime("%Y-%m-%d_%H:%M:%S")}.csv"
        self.file = open(filepath, "w")

    async def run(self, queue):
        try:
            header = True

            while True:
                timestamp, data = await queue.get()

                if header:
                    header = False
                    self.file.write("timestamp")
                    for key in data.keys():
                        self.file.write(f",{key}")
                    self.file.write("\n")

                self.file.write(f"{timestamp}")
                for _, value in data.items():
                    self.file.write(f",{value}")
                self.file.write("\n")

                self.file.flush()
        except asyncio.CancelledError:
            self.file.close()
            raise


async def main():
    async with (
        # Focus("192.168.8.1") as focus,
        Bench("ws://192.168.7.2:81/data") as bench,
    ):
        bus = Bus()

        # experiment = Experiment()
        plotter1 = Plotter(["true_velocity", "focus_velocity"])
        plotter2 = Plotter(["setpoint_torque", "true_torque"])
        recorder = Recorder()

        tasks = [
            # asyncio.create_task(experiment.run(bus, focus)),
            # asyncio.create_task(focus.run(bus)),
            asyncio.create_task(bench.run(bus)),
            asyncio.create_task(plotter1.run(bus.subscribe())),
            asyncio.create_task(plotter2.run(bus.subscribe())),
            asyncio.create_task(recorder.run(bus.subscribe())),
        ]

        await tasks[0]
        for task in tasks:
            task.cancel()


if __name__ == "__main__":
    app = QApplication(sys.argv)

    loop = QEventLoop(app)
    asyncio.set_event_loop(loop)

    with loop:
        loop.run_until_complete(main())
