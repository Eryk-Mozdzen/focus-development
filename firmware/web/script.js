const ws = new WebSocket("ws://" + location.host + ":81/data")

ws.binaryType = "arraybuffer"

ws.onerror = (e) => {
    console.error(e)
}

ws.onmessage = (event) => {
    const view = new DataView(event.data)

    document.getElementById("thrust").textContent = view.getFloat32(0, true).toFixed(3)
    document.getElementById("torque").textContent = view.getFloat32(4, true).toFixed(5)
    document.getElementById("velocity").textContent = view.getFloat32(8, true).toFixed(0)
    document.getElementById("temperature").textContent = view.getFloat32(12, true).toFixed(1)
    document.getElementById("voltage").textContent = view.getFloat32(16, true).toFixed(2)
    document.getElementById("current").textContent = view.getFloat32(20, true).toFixed(2)

    document.getElementById("thrust_load").textContent = view.getInt32(24, true) + " LSB"
    document.getElementById("torque_load1").textContent = view.getInt32(28, true) + " LSB"
    document.getElementById("torque_load2").textContent = view.getInt32(32, true) + " LSB"

    document.getElementById("velocity_frequency").textContent = view.getFloat32(36, true).toFixed(2) + " Hz"
}

function sendCommand(cmd) {
    if(ws.readyState === WebSocket.OPEN) {
        ws.send(cmd)
	}
}
