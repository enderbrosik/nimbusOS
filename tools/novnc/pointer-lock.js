import RFB from "/core/rfb.js";

const screen = document.getElementById("screen");
const status = document.getElementById("status");
const scheme = location.protocol === "https:" ? "wss" : "ws";
const rfb = new RFB(screen, `${scheme}://${location.host}/websockify`);
rfb.scaleViewport = true;
rfb.focusOnClick = true;

let canvas = null;
let pointerX = 0;
let pointerY = 0;

function setStatus(text) {
    status.textContent = text;
}

function clampPointer(bounds) {
    pointerX = Math.max(0, Math.min(bounds.width - 1, pointerX));
    pointerY = Math.max(0, Math.min(bounds.height - 1, pointerY));
}

function forwardRelativeMove(event) {
    if (!canvas || !event.isTrusted || document.pointerLockElement !== canvas) return;
    const bounds = canvas.getBoundingClientRect();
    if (!bounds.width || !bounds.height) return;

    event.preventDefault();
    event.stopImmediatePropagation();
    pointerX += event.movementX;
    pointerY += event.movementY;
    clampPointer(bounds);

    canvas.dispatchEvent(new MouseEvent("mousemove", {
        bubbles: true,
        cancelable: true,
        clientX: bounds.left + pointerX,
        clientY: bounds.top + pointerY,
    }));
}

rfb.addEventListener("connect", () => {
    canvas = screen.querySelector("canvas");
    if (!canvas) {
        setStatus("Connected, but the display canvas was not found.");
        return;
    }

    screen.addEventListener("mousedown", (event) => {
        if (event.button !== 0 || document.pointerLockElement === canvas) return;
        const bounds = canvas.getBoundingClientRect();
        pointerX = event.clientX - bounds.left;
        pointerY = event.clientY - bounds.top;
        clampPointer(bounds);
        const request = canvas.requestPointerLock();
        if (request && request.catch) request.catch(() => setStatus("Chrome denied mouse capture."));
    }, true);

    screen.addEventListener("mousemove", forwardRelativeMove, true);
    setStatus("Connected. Click the display to capture the mouse; press Esc to release.");
});

rfb.addEventListener("disconnect", (event) => {
    canvas = null;
    setStatus(event.detail.clean ? "Disconnected." : "Connection lost.");
});

document.addEventListener("pointerlockchange", () => {
    if (document.pointerLockElement === canvas) setStatus("Mouse captured. Press Esc or use Release mouse to unlock.");
    else if (canvas) setStatus("Connected. Click the display to capture the mouse.");
});

document.addEventListener("pointerlockerror", () => setStatus("Chrome could not capture the mouse."));
document.getElementById("release").addEventListener("click", () => document.exitPointerLock());