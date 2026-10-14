const modeToggleBtn = document.getElementById('mode-toggle');
const statusContainer = document.getElementById('status-container');
const statusText = document.getElementById('status-text');

// 1. Check local storage when the page first loads
const savedTheme = localStorage.getItem('theme');
if (savedTheme === 'dark') {
    document.body.classList.add('dark-theme');
}

// 2. Save preference and toggle theme when clicked (ONLY ONE LISTENER HERE!)
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
    }
    else {
        statusContainer.classList.remove('has-error');
        statusText.textContent = 'Connected!';
    }
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
}

document.addEventListener('keydown', (event) => {
    event.preventDefault();
    
    const buttonId = keyMap[event.code];

    if(buttonId) {
        const button = document.getElementById(buttonId);

        if(button) {
            button.classList.add('keyboard-active')
        }
    }
})

document.addEventListener('keyup', (event) => {
    const buttonId = keyMap[event.code];

    if(buttonId) {
        const button = document.getElementById(buttonId);

        if(button) {
            button.classList.remove('keyboard-active');
        }
    }
})

// Test initial status (change to false later when live)
updateStatus(false);