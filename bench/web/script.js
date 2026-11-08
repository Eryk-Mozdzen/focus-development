const ws = new WebSocket("ws://" + location.host + "/websocket")
const slider = document.getElementById("slider")

ws.onmessage = (event) => {
    const data = JSON.parse(event.data)

    document.getElementById("thrust").textContent = data.thrust.toFixed(3)
    document.getElementById("thrust_raw").textContent = data.thrust_raw + " LSB"
    document.getElementById("torque").textContent = data.torque.toFixed(5)
    document.getElementById("torque_raw1").textContent = data.torque_raw1 + " LSB"
    document.getElementById("torque_raw2").textContent = data.torque_raw2 + " LSB"
    document.getElementById("velocity").textContent = data.vel.toFixed(0)
    document.getElementById("velocity_raw").textContent = data.vel_raw.toFixed(2) + " Hz"
    document.getElementById("temperature").textContent = data.temp.toFixed(1)
    document.getElementById("voltage").textContent = data.volt.toFixed(2)
    document.getElementById("current").textContent = data.curr.toFixed(2)

    slider.value = 1000 * data.gen
    document.getElementById("slider_value").textContent = (100 * data.gen).toFixed(0) + "%"
}

function sendCommand(cmd) {
    if(ws.readyState === WebSocket.OPEN) {
        ws.send(cmd)
	}
}

function updateSliderUI() {
    document.getElementById("slider_value").textContent = (0.1 * slider.value).toFixed(0) + "%"
}

slider.addEventListener("input", () => {
    updateSliderUI()
    sendCommand("generator " + Number(slider.value))
})

updateSliderUI()
