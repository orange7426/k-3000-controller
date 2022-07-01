var serverStatus = {};

var gateway = `ws://${window.location.hostname}/ws`;
var websocket;
window.addEventListener('load', onload);

function onload(event) {
  initWebSocket();
}

function initWebSocket() {
  console.log('Trying to open a WebSocket connection');
  websocket = new WebSocket(gateway);
  websocket.onopen = onOpen;
  websocket.onclose = onClose;
  websocket.onmessage = onMessage;
}

function pullStatus() {
  websocket.send("status");
}

function onOpen(event) {
  console.log('Connection opened');
  pullStatus();
}

function onClose(event) {
  console.log('Connection closed');
  setTimeout(initWebSocket, 2000);
}

function onMessage(event) {
  serverStatus = JSON.parse(event.data);
  console.log("New status", serverStatus);

  document.getElementById("isMotorEnabled").innerHTML =
    serverStatus.isMotorEnabled === true ? "on" :
    serverStatus.isMotorEnabled === false ? "off" :
    "unknown";

  document.getElementById("numberOfShots").value = serverStatus.numberOfShots;
  document.getElementById("delayBetweenShots").value = serverStatus.delayBetweenShots;
}

// Methods

function refresh() {
  websocket.send("status");
}

function setInfinite() {
  document.getElementById("numberOfShots").value = -1;
}

function turnOn() {
  websocket.send("on");
}

function turnOff() {
  websocket.send("off");
}

function oneShot() {
  websocket.send("os");
}

function save() {
  const numberOfShots = parseInt(document.getElementById("numberOfShots").value, 10);
  const delayBetweenShots = parseFloat(document.getElementById("delayBetweenShots").value);
  const newSettings = {
    numberOfShots: numberOfShots,
    delayBetweenShots: delayBetweenShots,
  };
  websocket.send(JSON.stringify(newSettings));

  return false;
}