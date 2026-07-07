'use strict';

// ── Chrome: nav + session status strip ───────────────────────────────────────

function buildChrome() {
    const pages = [
        { label: 'Session',  href: '/session.html'  },
        { label: 'Script',   href: '/script.html'   },
        { label: 'Messages', href: '/messages.html' },
        { label: 'Raw',      href: '/raw.html'      },
        { label: 'Config',   href: '/config.html'   },
        { label: 'Logs',     href: '/logs.html'      },
    ];

    const chrome = document.createElement('div');
    chrome.id = 'chrome';

    const navRow = document.createElement('div');
    navRow.id = 'nav-row';
    for (const page of pages) {
        const btn = document.createElement('button');
        btn.textContent = page.label;
        btn.onclick = () => { window.location.href = page.href; };
        navRow.appendChild(btn);
    }

    const statusRow = document.createElement('div');
    statusRow.id = 'status-row';
    statusRow.innerHTML = '<span class="status-dot">○</span><span id="status-text">NOT LOGGED ON</span>';

    chrome.appendChild(navRow);
    chrome.appendChild(statusRow);
    document.body.insertBefore(chrome, document.body.firstChild);

    // Company logo, pinned to the bottom-right corner of every page. Ships with a
    // default placeholder at /logo.png; replace that file to brand the client.
    // A PNG is used so a logo with transparency alpha-blends over the page.
    const logo = document.createElement('img');
    logo.id = 'company-logo';
    logo.src = '/logo.png';
    logo.alt = 'Company logo';
    document.body.appendChild(logo);
}

function updateStatusStrip(status) {
    const dot = document.querySelector('#status-row .status-dot');
    const text = document.getElementById('status-text');
    if (!dot || !text) {
        return;
    }
    if (status.loggedOn) {
        dot.textContent = '●';
        dot.style.color = '#090';
        text.textContent = 'LOGGED ON   '
            + status.senderCompId + ' → ' + status.targetCompId
            + '   out: ' + status.nextOutgoingSeqNum
            + '   in: '  + status.nextIncomingSeqNum;
    } else if (status.loggingOn) {
        dot.textContent = '○';
        dot.style.color = '';
        text.textContent = 'CONNECTING…';
    } else {
        dot.textContent = '○';
        dot.style.color = '';
        text.textContent = 'NOT LOGGED ON';
    }
}

let statusPollTimer = null;

function startStatusPoll() {
    async function poll() {
        try {
            const response = await fetch('/api/session');
            if (response.ok) {
                const status = await response.json();
                updateStatusStrip(status);
                window._lastSessionStatus = status;
            }
        } catch (e) {
            // network error during startup/shutdown — ignore
        }
        statusPollTimer = setTimeout(poll, 2000);
    }
    poll();
}

// ── Utilities ─────────────────────────────────────────────────────────────────

async function apiPost(url, params) {
    const body = new URLSearchParams(params);
    const response = await fetch(url, { method: 'POST', body });
    return response.json();
}

async function apiGet(url) {
    const response = await fetch(url);
    return response.json();
}

// ── Page init ─────────────────────────────────────────────────────────────────

document.addEventListener('DOMContentLoaded', () => {
    buildChrome();
    startStatusPoll();
    if (typeof pageInit === 'function') {
        pageInit();
    }
});
