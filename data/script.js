const modeToggleBtn = document.getElementById('mode-toggle');
const statusContainer = document.getElementById('status-container');
const statusText = document.getElementById('status-text');

// 1. Check local storage when the page first loads
const savedTheme = localStorage.getItem('theme');
if (savedTheme === 'dark') {
    document.body.classList.add('dark-theme');
}

// 2. Save preference and toggle theme when clicked
modeToggleBtn.addEventListener('click', () => {
    document.body.classList.toggle('dark-theme');
    if (document.body.classList.contains('dark-theme')) {
        localStorage.setItem('theme', 'dark');
    } else {
        localStorage.setItem('theme', 'light');
    }
});

// 3. Status update function
function updateStatus(hasError) {
    if(hasError) {
        statusContainer.classList.add('has-error');
        statusText.textContent = 'Disconnected';
    } else {
        statusContainer.classList.remove('has-error');
        statusText.textContent = 'Connected!';
    }
}

// Function to send movement commands to ESP32-CAM
function sendCommand(action) {
    fetch(`/control?action=${action}`)
        .then(response => {
            if (response.ok) updateStatus(false);
            else updateStatus(true);
        })
        .catch(err => updateStatus(true));
}

const keyMap = {
    ArrowUp: 'btn-up',
    KeyW: 'btn-up',
    ArrowDown: 'btn-down',
    KeyS: 'btn-down',
    ArrowLeft: 'btn-left',
    KeyA: 'btn-left',
    ArrowRight: 'btn-right',
    KeyD: 'btn-right'
};

const actionMap = {
    'btn-up': 'forward',
    'btn-down': 'backward',
    'btn-left': 'left',
    'btn-right': 'right'
};

// Keyboard down
document.addEventListener('keydown', (event) => {
    if (event.repeat) return; // Prevent spamming requests while holding key
    const buttonId = keyMap[event.code];
    if(buttonId) {
        event.preventDefault();
        const button = document.getElementById(buttonId);
        if(button) {
            button.classList.add('keyboard-active');
            sendCommand(actionMap[buttonId]);
        }
    }
});

// Keyboard up (Stop)
document.addEventListener('keyup', (event) => {
    const buttonId = keyMap[event.code];
    if(buttonId) {
        const button = document.getElementById(buttonId);
        if(button) {
            button.classList.remove('keyboard-active');
            sendCommand('stop');
        }
    }
});

// Mouse / Touch Event Listeners for UI buttons
['btn-up', 'btn-down', 'btn-left', 'btn-right'].forEach(id => {
    const btn = document.getElementById(id);
    if (btn) {
        btn.addEventListener('mousedown', () => sendCommand(actionMap[id]));
        btn.addEventListener('mouseup', () => sendCommand('stop'));
        btn.addEventListener('touchstart', (e) => { e.preventDefault(); sendCommand(actionMap[id]); });
        btn.addEventListener('touchend', (e) => { e.preventDefault(); sendCommand('stop'); });
    }
});

// Initialize video stream source
window.addEventListener('DOMContentLoaded', () => {
    const vidStream = document.getElementById('vid-stream');
    vidStream.src = `http://${window.location.hostname}/stream`;
    updateStatus(false);
});