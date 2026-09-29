const remoteBridgeState = {
    isRemoteClient: false,
    token: '',
    sid: '',
    pollStarted: false,
    pollErrorShown: false,
    actionErrorShown: false,
    pollAbortController: null
};

function sleep(ms) {
    return new Promise(resolve => setTimeout(resolve, ms));
}

const PageVisibility = {
    isVisible: !document.hidden,
    pauseHandlers: new Set(),
    resumeHandlers: new Set(),
    visibleWaiters: [],

    init() {
        const handleVisibilityChange = () => {
            const nextVisible = !document.hidden;
            if (nextVisible === this.isVisible) {
                this.updateBodyClass();
                return;
            }

            this.isVisible = nextVisible;
            this.updateBodyClass();

            const handlers = Array.from(nextVisible ? this.resumeHandlers : this.pauseHandlers);
            handlers.forEach(handler => {
                try {
                    handler();
                } catch (error) {
                    console.error('Page visibility handler failed:', error);
                }
            });

            if (nextVisible) {
                if (this.visibleWaiters.length > 0) {
                    const waiters = this.visibleWaiters.splice(0, this.visibleWaiters.length);
                    waiters.forEach(resolve => resolve());
                }
                this.triggerResume();
            }
        };

        document.addEventListener('visibilitychange', handleVisibilityChange);
        if (!document.body) {
            document.addEventListener('DOMContentLoaded', () => this.updateBodyClass(), { once: true });
        }
        this.updateBodyClass();
    },

    updateBodyClass() {
        if (!document.body) {
            return;
        }

        document.body.classList.toggle('page-visible', this.isVisible);
        document.body.classList.toggle('page-hidden', !this.isVisible);
    },

    onPause(handler) {
        if (typeof handler === 'function') {
            this.pauseHandlers.add(handler);
        }
    },

    onResume(handler) {
        if (typeof handler === 'function') {
            this.resumeHandlers.add(handler);
        }
    },

    triggerResume() {
        if (this.isVisible && pendingBackendMessages.length > 0) {
            scheduleBackendMessageFlush();
        }
    },

    waitUntilVisible() {
        if (this.isVisible) {
            return Promise.resolve();
        }

        return new Promise(resolve => {
            this.visibleWaiters.push(resolve);
        });
    }
};

PageVisibility.init();

function generateRemoteSessionId() {
    return `sid-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}

function initRemoteBridgeState() {
    try {
        const url = new URL(window.location.href);
        const token = url.searchParams.get('token') || '';
        remoteBridgeState.isRemoteClient = !(window.chrome && window.chrome.webview) && !!token;
        remoteBridgeState.token = token;
        remoteBridgeState.sid = remoteBridgeState.isRemoteClient ? generateRemoteSessionId() : '';

        if (remoteBridgeState.isRemoteClient) {
            document.documentElement.classList.add('remote-browser-client');
            if (document.body) {
                document.body.classList.add('remote-browser-client');
            }
        }
    } catch (error) {
        console.error('Failed to initialize remote bridge state:', error);
    }
}

async function remoteBridgeRequest(path, options = {}) {
    const headers = Object.assign({
        'Cache-Control': 'no-store',
        'X-Remote-Token': remoteBridgeState.token
    }, options.headers || {});

    if (remoteBridgeState.sid) {
        headers['X-Remote-Sid'] = remoteBridgeState.sid;
    }

    const response = await fetch(path, {
        method: options.method || 'GET',
        headers,
        cache: 'no-store',
        body: options.body,
        signal: options.signal
    });

    const text = await response.text();
    let payload = null;
    if (text) {
        try {
            payload = JSON.parse(text);
        } catch (error) {
            payload = { ok: response.ok, error: text };
        }
    }

    if (!response.ok || (payload && payload.ok === false)) {
        throw new Error(payload?.error || `HTTP ${response.status}`);
    }

    return payload || { ok: true };
}

async function pollRemoteBrowserMessages() {
    if (!remoteBridgeState.isRemoteClient || remoteBridgeState.pollStarted) {
        return;
    }

    remoteBridgeState.pollStarted = true;

    try {
        while (remoteBridgeState.isRemoteClient) {
            if (!PageVisibility.isVisible) {
                await PageVisibility.waitUntilVisible();
                continue;
            }

            try {
                const abortController = new AbortController();
                remoteBridgeState.pollAbortController = abortController;

                const payload = await remoteBridgeRequest('/api/poll', {
                    signal: abortController.signal
                });
                remoteBridgeState.pollAbortController = null;
                remoteBridgeState.pollErrorShown = false;

                if (Array.isArray(payload.messages) && payload.messages.length > 0) {
                    dispatchBackendPayload(payload.messages);
                }
            } catch (error) {
                remoteBridgeState.pollAbortController = null;

                if (error && error.name === 'AbortError') {
                    continue;
                }

                console.error('Remote poll failed:', error);
                if (!remoteBridgeState.pollErrorShown) {
                    showToast('error', '远程连接中断', error.message || '无法从远程服务获取消息');
                    remoteBridgeState.pollErrorShown = true;
                }
                await sleep(1500);
            }
        }
    } finally {
        remoteBridgeState.pollStarted = false;
        remoteBridgeState.pollAbortController = null;
    }
}

function initRemoteBrowserClientUi() {
    if (!remoteBridgeState.isRemoteClient) {
        return;
    }

    ['btnRemoteModeApply', 'btnGenerateRemoteToken', 'appBtnRemoteModeApply', 'appBtnGenerateRemoteToken'].forEach(id => {
        const button = document.getElementById(id);
        if (button) {
            button.disabled = true;
            button.title = '远程会话中不允许修改远程浏览器服务配置';
        }
    });
}

initRemoteBridgeState();

// WebView2 message-based API
function postAction(action, params) {
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(JSON.stringify(
            Object.assign({ action: action }, params || {})
        ));
        return;
    }

    if (remoteBridgeState.isRemoteClient) {
        remoteBridgeRequest('/api/action', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json; charset=utf-8'
            },
            body: JSON.stringify(Object.assign({ action: action }, params || {}))
        }).then(() => {
            remoteBridgeState.actionErrorShown = false;
        }).catch(error => {
            console.error('Remote action failed:', action, error);
            if (!remoteBridgeState.actionErrorShown) {
                showToast('error', '远程操作失败', error.message || '无法向远程服务发送操作');
                remoteBridgeState.actionErrorShown = true;
            }
        });
        return;
    }

    console.error('WebView2 not available! window.chrome:', window.chrome, 'window.chrome.webview:', window.chrome?.webview);
    showToast('error', '错误', 'WebView2未初始化，无法与后端通信');
}

// Login Tab Switching
function initLoginTabs() {
    const tabs = document.querySelectorAll('.login-tab[data-tab]');
    const contents = ['login', 'register', 'renew']
        .map(name => document.getElementById('tab-' + name))
        .filter(Boolean);

    tabs.forEach(tab => {
        tab.addEventListener('click', () => {
            const targetTab = tab.getAttribute('data-tab');
            const targetContent = document.getElementById('tab-' + targetTab);
            if (!targetTab || !targetContent) {
                return;
            }

            // Remove active class from all tabs and contents
            tabs.forEach(t => t.classList.remove('active'));
            contents.forEach(c => c.classList.remove('active'));

            // Add active class to clicked tab and corresponding content
            tab.classList.add('active');
            targetContent.classList.add('active');
        });
    });
}

// Register functionality
function initRegister() {
    const btnRegister = $('btnRegisterSubmit');
    const btnText = $('registerBtnText');
    const messageArea = $('registerMessage');

    btnRegister.addEventListener('click', () => {
        const username = $('registerUsername').value.trim();
        const password = $('registerPassword').value.trim();
        const superPassword = $('registerSuperPassword').value.trim();
        const cards = $('registerCards').value.trim();

        if (!username || !password || !superPassword) {
            messageArea.className = 'message-area error';
            messageArea.textContent = '请输入账号、密码和超级密码';
            return;
        }

        btnRegister.disabled = true;
        btnText.textContent = '注册中...';
        messageArea.className = 'message-area';

        postAction('cloud_register', {
            username: username,
            password: password,
            superPassword: superPassword,
            cards: cards
        });
    });
}

// Renew functionality
function initRenew() {
    const btnRenew = $('btnRenewSubmit');
    const btnText = $('renewBtnText');
    const messageArea = $('renewMessage');

    btnRenew.addEventListener('click', () => {
        const username = $('renewUsername').value.trim();
        const password = $('renewPassword').value.trim();
        const cards = $('renewCards').value.trim();

        if (!username || !password) {
            messageArea.className = 'message-area error';
            messageArea.textContent = '请输入账号和密码';
            return;
        }

        btnRenew.disabled = true;
        btnText.textContent = '续费中...';
        messageArea.className = 'message-area';

        postAction('cloud_renew', {
            username: username,
            password: password,
            cards: cards
        });
    });
}

// Window control functions
function minimizeWindow() {
    postAction('window_minimize');
}

function maximizeWindow() {
    postAction('window_maximize');
}

function closeWindow() {
    showConfirmDialog('确认关闭', '确定要关闭程序吗？', function() {
        postAction('window_close');
    });
}

// Confirm Dialog Functions
function showConfirmDialog(title, message, onConfirm) {
    const dialog = $('confirmDialog');
    const confirmTitle = $('confirmTitle');
    const confirmMessage = $('confirmMessage');
    const confirmBtn = $('confirmBtn');

    confirmTitle.textContent = title;
    confirmMessage.textContent = message;
    dialog.style.display = 'flex';

    // Remove old event listeners
    const newConfirmBtn = confirmBtn.cloneNode(true);
    confirmBtn.parentNode.replaceChild(newConfirmBtn, confirmBtn);

    // Add new event listener
    newConfirmBtn.addEventListener('click', function() {
        closeConfirmDialog();
        if (onConfirm) onConfirm();
    });
}

function closeConfirmDialog() {
    const dialog = $('confirmDialog');
    dialog.style.display = 'none';
}

// Titlebar drag functionality
function initTitlebarDrag() {
    if (remoteBridgeState.isRemoteClient) return;

    const topBar = document.getElementById('topBarDragRegion');
    if (!topBar) return;

    let isDragging = false;
    let startX = 0;
    let startY = 0;

    topBar.addEventListener('mousedown', (e) => {
        // 忽略按钮区域和右侧信息区域的点击
        if (e.target.closest('.window-controls') ||
            e.target.closest('.window-btn') ||
            e.target.closest('.top-bar-info') ||
            e.target.closest('button')) {
            return;
        }

        isDragging = true;
        startX = e.clientX;
        startY = e.clientY;

        // 通知后端开始拖动
        postAction('window_drag_start', { x: e.screenX, y: e.screenY });

        e.preventDefault();
    });

    document.addEventListener('mousemove', (e) => {
        if (!isDragging) return;

        const deltaX = e.clientX - startX;
        const deltaY = e.clientY - startY;

        // 通知后端移动窗口
        postAction('window_drag_move', {
            screenX: e.screenX,
            screenY: e.screenY,
            deltaX: deltaX,
            deltaY: deltaY
        });
    });

    document.addEventListener('mouseup', () => {
        if (isDragging) {
            isDragging = false;
            postAction('window_drag_end');
        }
    });
}

// Toast notification system
const toastContainer = document.getElementById('toastContainer');
let toastId = 0;

function showToast(type, title, message, duration = 5000) {
    const id = toastId++;
    const toast = document.createElement('div');
    toast.className = `toast ${type}`;
    toast.id = `toast-${id}`;

    const icons = {
        success: '<svg width="20" height="20" viewBox="0 0 20 20" fill="currentColor" style="color:#10b981"><path fill-rule="evenodd" d="M10 18a8 8 0 100-16 8 8 0 000 16zm3.707-9.293a1 1 0 00-1.414-1.414L9 10.586 7.707 9.293a1 1 0 00-1.414 1.414l2 2a1 1 0 001.414 0l4-4z"/></svg>',
        error: '<svg width="20" height="20" viewBox="0 0 20 20" fill="currentColor" style="color:#ef4444"><path fill-rule="evenodd" d="M10 18a8 8 0 100-16 8 8 0 000 16zM8.707 7.293a1 1 0 00-1.414 1.414L8.586 10l-1.293 1.293a1 1 0 101.414 1.414L10 11.414l1.293 1.293a1 1 0 001.414-1.414L11.414 10l1.293-1.293a1 1 0 00-1.414-1.414L10 8.586 8.707 7.293z"/></svg>',
        warning: '<svg width="20" height="20" viewBox="0 0 20 20" fill="currentColor" style="color:#f59e0b"><path fill-rule="evenodd" d="M8.257 3.099c.765-1.36 2.722-1.36 3.486 0l5.58 9.92c.75 1.334-.213 2.98-1.742 2.98H4.42c-1.53 0-2.493-1.646-1.743-2.98l5.58-9.92zM11 13a1 1 0 11-2 0 1 1 0 012 0zm-1-8a1 1 0 00-1 1v3a1 1 0 002 0V6a1 1 0 00-1-1z"/></svg>',
        info: '<svg width="20" height="20" viewBox="0 0 20 20" fill="currentColor" style="color:#3b82f6"><path fill-rule="evenodd" d="M18 10a8 8 0 11-16 0 8 8 0 0116 0zm-7-4a1 1 0 11-2 0 1 1 0 012 0zM9 9a1 1 0 000 2v3a1 1 0 001 1h1a1 1 0 100-2v-3a1 1 0 00-1-1H9z"/></svg>'
    };

    toast.innerHTML = `
        <div class="toast-icon">${icons[type] || icons.info}</div>
        <div class="toast-content">
            <div class="toast-title">${title}</div>
            <div class="toast-message">${message}</div>
        </div>
        <button class="toast-close" onclick="closeToast(${id})">
            <svg width="16" height="16" viewBox="0 0 16 16" fill="currentColor">
                <path d="M4.646 4.646a.5.5 0 01.708 0L8 7.293l2.646-2.647a.5.5 0 01.708.708L8.707 8l2.647 2.646a.5.5 0 01-.708.708L8 8.707l-2.646 2.647a.5.5 0 01-.708-.708L7.293 8 4.646 5.354a.5.5 0 010-.708z"/>
            </svg>
        </button>
    `;

    toastContainer.appendChild(toast);

    if (duration > 0) {
        setTimeout(() => closeToast(id), duration);
    }
}

function closeToast(id) {
    const toast = document.getElementById(`toast-${id}`);
    if (toast) {
        toast.style.animation = 'slideIn 0.3s ease reverse';
        setTimeout(() => toast.remove(), 300);
    }
}

// Modal system
const modalOverlay = document.getElementById('modalOverlay');
const modalTitle = document.getElementById('modalTitle');
const modalBody = document.getElementById('modalBody');
const modalFooter = document.getElementById('modalFooter');

function showModal(title, bodyHTML, footerHTML) {
    modalTitle.textContent = title;
    modalBody.innerHTML = bodyHTML;
    modalFooter.innerHTML = footerHTML || '';
    modalOverlay.style.display = 'flex';
}

function closeModal() {
    modalOverlay.style.display = 'none';
}

window.__lastCloudUpdateUrl = window.__lastCloudUpdateUrl || '';
window.__pendingCloudLoginAttempt = window.__pendingCloudLoginAttempt || null;
window.__cloudLoginKickInProgressCid = window.__cloudLoginKickInProgressCid || 0;
window.__lastMaxOnlineClients = Array.isArray(window.__lastMaxOnlineClients) ? window.__lastMaxOnlineClients : [];
window.__loginPrefsState = window.__loginPrefsState || {
    savedLoginMode: 'account',
    rememberPassword: false,
    autoLogin: false,
    savedUsername: '',
    savedPassword: ''
};
window.__autoLoginAttempted = window.__autoLoginAttempted || false;
window.__currentCloudLoggedIn = window.__currentCloudLoggedIn || false;
window.__currentCloudLoginType = window.__currentCloudLoginType || 'account';
window.__loginModeState = window.__loginModeState || 'account';
window.__appRuntimeConfig = window.__appRuntimeConfig || {
    fastMode: false,
    enableLogging: true,
    autoScrollLog: true
};
let loginPrefsApplyTimer = null;

function showCloudUpdatePrompt(data) {
    const force = !!data.force;
    const localVer = Number.isFinite(Number(data.localVer)) ? Number(data.localVer) : 0;
    const serverVer = Number.isFinite(Number(data.serverVer)) ? Number(data.serverVer) : 0;
    const minVer = Number.isFinite(Number(data.minVer)) ? Number(data.minVer) : 0;

    if (data.resetToLogin) {
        showLoginScreen();
    }

    window.__lastCloudUpdateUrl = data.url || '';

    const versionItems = [
        `<div><strong>本地版本：</strong>${escapeHtml(localVer || '-')}</div>`,
        `<div><strong>服务端版本：</strong>${escapeHtml(serverVer || '-')}</div>`
    ];
    if (minVer > 0) {
        versionItems.push(`<div><strong>最低版本：</strong>${escapeHtml(minVer)}</div>`);
    }

    const reasonHtml = data.reason
        ? `<p style="margin:0 0 12px 0; color: var(--text-muted);">${escapeHtml(data.reason)}</p>`
        : '';
    const urlHtml = data.url
        ? `
            <div class="notice-area" style="display:block; margin-top: 16px;">
                <div class="notice-header"><span>更新地址</span></div>
                <div class="notice-body" style="word-break: break-all;">${escapeHtml(data.url)}</div>
            </div>
        `
        : `
            <div class="notice-area" style="display:block; margin-top: 16px;">
                <div class="notice-header"><span>更新地址</span></div>
                <div class="notice-body">服务端暂未配置下载地址，请联系管理员。</div>
            </div>
        `;
    const runHtml = (data.runExe || data.runCmd)
        ? `
            <div style="margin-top: 12px; color: var(--text-muted); font-size: 13px; line-height: 1.7;">
                ${data.runExe ? `<div><strong>启动程序：</strong>${escapeHtml(data.runExe)}</div>` : ''}
                ${data.runCmd ? `<div><strong>启动参数：</strong>${escapeHtml(data.runCmd)}</div>` : ''}
            </div>
        `
        : '';

    const bodyHTML = `
        ${reasonHtml}
        <p style="margin:0 0 12px 0; line-height:1.7;">${escapeHtml(data.message || (force ? '检测到强制更新，请先升级客户端。' : '检测到新版本，建议尽快升级客户端。'))}</p>
        <div style="display:grid; gap:8px; padding:12px 14px; border-radius:12px; background:rgba(255,255,255,0.05);">
            ${versionItems.join('')}
        </div>
        ${urlHtml}
        ${runHtml}
    `;

    const footerParts = [];
    if (data.url) {
        footerParts.push('<button class="btn btn-ghost" onclick="copyToClipboard(window.__lastCloudUpdateUrl || \'\')">复制更新地址</button>');
    }
    footerParts.push(`<button class="btn btn-primary" onclick="closeModal()">${force ? '我知道了' : '稍后处理'}</button>`);

    showModal(data.title || (force ? '强制更新' : '发现新版本'), bodyHTML, footerParts.join(''));
}

// DOM refs
const $ = id => document.getElementById(id);
const mobileMenuToggle = $('mobileMenuToggle');
const mobileMenuBackdrop = $('mobileMenuBackdrop');
const MOBILE_NAV_BREAKPOINT_PX = 768;

// Page navigation
let currentMainPage = document.querySelector('.nav-item.active')?.dataset.page ||
    document.querySelector('.page.active')?.id?.replace('page-', '') ||
    'home';
let lastInstancesRenderKey = '';
let lastPoolListRenderKey = '';
let lastAccountPanelRenderKey = '';
let lastProxyListRenderKey = '';
let lastProxyDataRenderKey = '';
let lastLogsRenderKey = '';
let lastOnlineStatsCardsKey = '';
let lastOnlineSelectorKey = '';
let lastOnlineTableRowsKey = '';
let lastOnlineTableRowKeys = [];
let lastProxyPacketRowKeys = [];
let pendingProxyPacketDetailRequest = null;
let lastProxyPacketOwnerInstanceId = '';
let lastProxyInstanceCardKeys = [];
let lastActiveModifyHexCell = null;
window._proxyPackets = Array.isArray(window._proxyPackets) ? window._proxyPackets : [];
window._proxyPacketOwnerInstanceId = window._proxyPacketOwnerInstanceId || '';
window._proxyPacketDetails = window._proxyPacketDetails instanceof Map ? window._proxyPacketDetails : new Map();
window._proxyPacketDetailRequests = window._proxyPacketDetailRequests instanceof Set ? window._proxyPacketDetailRequests : new Set();
window._charlesSessionParseCache = window._charlesSessionParseCache instanceof Map ? window._charlesSessionParseCache : new Map();
let lastFilteredLogsKey = '';
let lastFilteredLogRowKeys = [];
let lastWpeFilterRowKeys = [];
let lastPoolItemKeys = [];
let lastDefaultFilterItemKeys = [];
let lastAccountTableRowsKey = '';
let lastAccountTableRowKeys = [];
let lastInstanceRowKeys = [];
let lastTrafficRuleRowKeys = [];
let currentSslMitmRules = [];
let currentHttpLocalMapRules = [];
let currentUserFilterSubTab = 'webUserState';
let wpeFilterGroups = [];
let currentWpeFilterGroupId = 0;
let lastUserFilterConfigData = null;

let lastAccountFilterItemKeys = [];
let lastWpeTargetInstancesKey = '';
let lastWpeTargetInstancesHtml = '';
let lastLogsDataKey = '';
let lastLogsDataRevision = 0;
let cachedOnlineStatsData = null;
let cachedRemoteBrowserConfig = null;
let cachedAntiCCConfig = null;
let cachedAntiCCGlobalState = null;

function resolveAntiCCInstance() {
    if (currentConfigInstance && currentConfigInstance.type === 'SocksForward') {
        return currentConfigInstance;
    }

    const instances = window.cachedInstances || [];
    const socksInstance = instances.find(inst => inst.type === 'SocksForward');
    if (socksInstance) {
        currentConfigInstance = socksInstance;
        return socksInstance;
    }
    return null;
}

function getSocksForwardInstances() {
    return (window.cachedInstances || []).filter(inst => inst.type === 'SocksForward');
}

function populateAntiCCWhitelistTargets(currentInstanceId, whitelistEntries = []) {
    const targetSelect = $('anticcWhitelistTargetInstances');
    if (!targetSelect) return;

    const socksInstances = getSocksForwardInstances();
    const selectedIds = new Set();
    whitelistEntries.forEach(entry => {
        (entry.targetInstanceIds || []).forEach(id => selectedIds.add(id));
    });

    targetSelect.innerHTML = socksInstances.map(inst => {
        const selected = selectedIds.has(inst.id) ? 'selected' : '';
        const currentMark = inst.id === currentInstanceId ? ' (当前)' : '';
        return `<option value="${inst.id}" ${selected}>${inst.name}${currentMark}</option>`;
    }).join('');
}

function buildAntiCCWhitelistEntries(instanceId) {
    const ips = $('anticcWhitelist').value.split('\n').map(ip => ip.trim()).filter(Boolean);
    const scopeMode = $('anticcWhitelistScope').value;
    const selectedTargetIds = Array.from($('anticcWhitelistTargetInstances').selectedOptions || []).map(opt => opt.value);

    return ips.map(ip => {
        const entry = {
            id: '',
            ip,
            source: 'manual',
            expireUnixSeconds: 0
        };

        if (scopeMode === 'global') {
            entry.scopeType = 0;
            entry.targetInstanceIds = [];
            entry.id = `manual_owner_${instanceId}_global_${ip}`;
        } else if (scopeMode === 'selected') {
            entry.scopeType = 2;
            entry.targetInstanceIds = selectedTargetIds.length ? selectedTargetIds : [instanceId];
            entry.id = `manual_owner_${instanceId}_selected_${ip}`;
        } else {
            entry.scopeType = 2;
            entry.targetInstanceIds = [instanceId];
            entry.id = `manual_owner_${instanceId}_current_${ip}`;
        }

        entry.sourceInstanceId = instanceId;

        return entry;
    });
}

function updateAntiCCWhitelistScopeUi() {
    const scopeMode = $('anticcWhitelistScope').value;
    const targetSelect = $('anticcWhitelistTargetInstances');
    if (!targetSelect) return;
    targetSelect.disabled = scopeMode !== 'selected';
}
let scheduledPageRequestToken = 0;
let instancesDataRevision = 0;
let poolsDataRevision = 0;
let accountPanelDataRevision = 0;
let onlineDataRevision = 0;
let wpeDataRevision = 0;
let lastStatusViewKey = '';

const navItemsByPage = new Map(
    Array.from(document.querySelectorAll('.nav-item[data-page]')).map(item => [item.dataset.page, item])
);
const pageElementsByPage = new Map(
    Array.from(document.querySelectorAll('.page[id]')).map(page => [page.id.replace('page-', ''), page])
);
const PAGE_REQUEST_TTL_MS = Object.freeze({
    instances: 4000,
    accounts: 5000,
    'remote-browser': 5000,
    online: 4000,
    anticc: 6000,
    wpe: 5000,
    logs: 3000
});
const pageRequestState = Object.create(null);

function isMainPageActive(page) {
    return currentMainPage === page;
}

function markPageDataRequested(page) {
    const state = pageRequestState[page] || (pageRequestState[page] = { loaded: false, lastRequestedAt: 0, lastUpdatedAt: 0 });
    state.lastRequestedAt = Date.now();
}

function markPageDataReady(page) {
    const state = pageRequestState[page] || (pageRequestState[page] = { loaded: false, lastRequestedAt: 0, lastUpdatedAt: 0 });
    state.loaded = true;
    state.lastUpdatedAt = Date.now();
}

function shouldRequestPageData(page, force = false) {
    if (force) {
        return true;
    }
    const ttl = PAGE_REQUEST_TTL_MS[page];
    if (ttl === undefined) {
        return true;
    }
    const state = pageRequestState[page];
    if (!state || !state.loaded) {
        return true;
    }
    const referenceTime = Math.max(state.lastUpdatedAt || 0, state.lastRequestedAt || 0);
    return (Date.now() - referenceTime) >= ttl;
}

function renderCachedDataForPage(page) {
    if (page === 'instances') {
        if (Array.isArray(window.cachedInstances)) {
            renderInstances(window.cachedInstances);
        }
    } else if (page === 'accounts') {
        if (Array.isArray(cachedPools)) {
            renderSocks5Pools(cachedPools);
            if (selectedPoolId) {
                const fallbackPool = cachedPools.find(pool => pool.id === selectedPoolId);
                renderAccountPanel(selectedPoolId, selectedPoolName || fallbackPool?.name || '', currentPoolAccounts || []);
            }
        }
    } else if (page === 'online') {
        if (cachedOnlineStatsData) {
            renderOnlineStats(cachedOnlineStatsData);
        }
    } else if (page === 'remote-browser') {
        if (cachedRemoteBrowserConfig) {
            updateRemoteBrowserPanel('', cachedRemoteBrowserConfig);
            updateRemoteBrowserPanel('app', cachedRemoteBrowserConfig);
        }
    } else if (page === 'anticc') {
        if (cachedAntiCCConfig) {
            updateAntiCCForm(cachedAntiCCConfig);
        }
    } else if (page === 'wpe') {
        renderWPEFilters(wpeFilters);
    } else if (page === 'logs') {
        renderLogs(allLogs);
    }
}

function runAfterPagePaint(callback) {
    const raf = window.requestAnimationFrame || function(cb) { return setTimeout(cb, 16); };
    raf(() => {
        setTimeout(callback, 0);
    });
}

function scheduleRequestDataForPage(page, options = {}) {
    const token = ++scheduledPageRequestToken;
    runAfterPagePaint(() => {
        if (token !== scheduledPageRequestToken) {
            return;
        }
        requestDataForPage(page, options);
    });
}

function requestDataForPage(page, options = {}) {
    const force = !!options.force;

    if (page === 'proxydata') {
        initProxyDataPage();
        return true;
    }

    if (!shouldRequestPageData(page, force)) {
        renderCachedDataForPage(page);
        return false;
    }

    markPageDataRequested(page);

    if (page === 'instances') {
        postAction('get_instances');
    } else if (page === 'accounts') {
        postAction('get_socks5_pools');
    } else if (page === 'remote-browser') {
        postAction('remote_browser_get_config');
    } else if (page === 'online') {
        postAction('get_online_stats');
    } else if (page === 'anticc') {
        loadAntiCCConfig();
    } else if (page === 'wpe') {
        postAction('get_wpe_filters');
    } else if (page === 'logs') {
        setLogsRealtimeEnabled(true);
    }

    return true;
}

let lastActiveNav = navItemsByPage.get(currentMainPage) || document.querySelector('.nav-item.active');
let lastActivePage = pageElementsByPage.get(currentMainPage) || document.querySelector('.page.active');

function isMobileNavLayout() {
    return window.innerWidth <= MOBILE_NAV_BREAKPOINT_PX;
}

function setMobileNavOpen(open) {
    const nextOpen = !!open && isMobileNavLayout();
    if (document.body) {
        document.body.classList.toggle('mobile-nav-open', nextOpen);
    }
    if (mobileMenuToggle) {
        mobileMenuToggle.setAttribute('aria-expanded', nextOpen ? 'true' : 'false');
    }
}

function closeMobileNav() {
    setMobileNavOpen(false);
}

function toggleMobileNav() {
    const isOpen = document.body?.classList.contains('mobile-nav-open') === true;
    setMobileNavOpen(!isOpen);
}

function initMobileNav() {
    if (mobileMenuToggle) {
        mobileMenuToggle.setAttribute('aria-expanded', 'false');
        mobileMenuToggle.addEventListener('click', (e) => {
            e.preventDefault();
            toggleMobileNav();
        });
    }

    if (mobileMenuBackdrop) {
        mobileMenuBackdrop.addEventListener('click', () => {
            closeMobileNav();
        });
    }

    window.addEventListener('resize', () => {
        if (!isMobileNavLayout()) {
            closeMobileNav();
        }
    });
}

function navigateTo(page, options = {}) {
    const force = !!options.force;
    const currentPageEl = pageElementsByPage.get(page);
    if (!force && currentMainPage === page && currentPageEl && currentPageEl.classList.contains('active')) {
        return false;
    }

    if (currentMainPage === 'logs' && page !== 'logs') {
        setLogsRealtimeEnabled(false);
    }

    if (lastActiveNav) lastActiveNav.classList.remove('active');
    if (lastActivePage) lastActivePage.classList.remove('active');

    const navItem = navItemsByPage.get(page);
    if (navItem) {
        navItem.classList.add('active');
        lastActiveNav = navItem;
    }

    const pageEl = currentPageEl;
    if (pageEl) {
        pageEl.classList.add('active');
        lastActivePage = pageEl;
    }

    currentMainPage = page;
    closeMobileNav();
    return true;
}

navItemsByPage.forEach((item, page) => {
    item.addEventListener('click', (e) => {
        e.preventDefault();
        const changed = navigateTo(page);
        if (!changed) {
            closeMobileNav();
            return;
        }
        scheduleRequestDataForPage(page);
    });
});

// ========== Login Screen ==========
const loginScreen = $('loginScreen');
const mainApp = $('mainApp');
const btnLoginSubmit = $('btnLoginSubmit');
const btnTrialLoginSubmit = $('btnTrialLoginSubmit');
const loginModeAccount = $('loginModeAccount');
const loginModeCard = $('loginModeCard');
const loginIdentityLabel = $('loginIdentityLabel');
const loginModeHint = $('loginModeHint');
const loginPasswordGroup = $('loginPasswordGroup');
const loginUsername = $('loginUsername');
const loginPassword = $('loginPassword');
const loginPasswordToggle = $('loginPasswordToggle');
const registerPasswordToggle = $('registerPasswordToggle');
const registerSuperPasswordToggle = $('registerSuperPasswordToggle');
const renewPasswordToggle = $('renewPasswordToggle');
const rememberPasswordCheckbox = $('rememberPassword');
const autoLoginCheckbox = $('autoLogin');
const btnRemoteModeApply = $('btnRemoteModeApply');
const btnCopyRemoteUrl = $('btnCopyRemoteUrl');

function setCloudLoginButtonsLoading(loading, trialLoading = false) {
    if (btnLoginSubmit) btnLoginSubmit.disabled = loading;
    if (btnTrialLoginSubmit) btnTrialLoginSubmit.disabled = loading;
    const loginBtnText = $('loginBtnText');
    if (loginBtnText) loginBtnText.textContent = loading ? '登录中...' : '登录';
    const trialText = $('trialLoginBtnText');
    if (trialText) {
        trialText.textContent = trialLoading ? '试用登录中...' : '试用登录';
    }
}

function setLoginPasswordVisibility(visible) {
    if (!loginPassword) return;
    const nextVisible = visible === true;
    loginPassword.type = nextVisible ? 'text' : 'password';
    if (loginPasswordToggle) {
        loginPasswordToggle.classList.toggle('is-visible', nextVisible);
        loginPasswordToggle.setAttribute('aria-label', nextVisible ? '隐藏密码' : '显示密码');
        loginPasswordToggle.setAttribute('title', nextVisible ? '隐藏密码' : '显示密码');
    }
}

function togglePasswordFieldVisibility(inputId, toggleBtnId) {
    const input = $(inputId);
    const toggleBtn = $(toggleBtnId);
    if (!input || !toggleBtn) return;

    const nextVisible = input.type === 'password';
    input.type = nextVisible ? 'text' : 'password';
    toggleBtn.classList.toggle('is-visible', nextVisible);
    toggleBtn.setAttribute('aria-label', nextVisible ? '隐藏密码' : '显示密码');
    toggleBtn.setAttribute('title', nextVisible ? '隐藏密码' : '显示密码');
}

function syncLoginPreferenceCheckboxes() {
    if (!rememberPasswordCheckbox || !autoLoginCheckbox) return;
    if (autoLoginCheckbox.checked) {
        rememberPasswordCheckbox.checked = true;
    }
}

function getCurrentLoginMode() {
    return window.__loginModeState === 'card' ? 'card' : 'account';
}

function applyLoginModeUi(mode) {
    const nextMode = mode === 'card' ? 'card' : 'account';
    window.__loginModeState = nextMode;

    if (loginModeAccount) loginModeAccount.classList.toggle('active', nextMode === 'account');
    if (loginModeCard) loginModeCard.classList.toggle('active', nextMode === 'card');

    if (loginIdentityLabel) {
        loginIdentityLabel.textContent = nextMode === 'card' ? '卡密' : '账号';
    }
    if (loginUsername) {
        loginUsername.placeholder = nextMode === 'card' ? '请输入卡密' : '请输入账号';
    }
    if (loginPasswordGroup) {
        loginPasswordGroup.style.display = nextMode === 'card' ? 'none' : '';
    }
    if (loginModeHint) {
        loginModeHint.textContent = nextMode === 'card'
            ? '当前为卡密模式，直接输入卡密登录。'
            : '当前为账号模式，使用账号和密码登录。';
    }
}

function applyLoginPreferencesToForm() {
    const prefs = window.__loginPrefsState || {};
    applyLoginModeUi(prefs.savedLoginMode || 'account');
    if (rememberPasswordCheckbox) rememberPasswordCheckbox.checked = !!prefs.rememberPassword;
    if (autoLoginCheckbox) autoLoginCheckbox.checked = !!prefs.autoLogin;
    syncLoginPreferenceCheckboxes();
    if (loginUsername) loginUsername.value = prefs.rememberPassword ? (prefs.savedUsername || '') : '';
    if (loginPassword) loginPassword.value = prefs.rememberPassword ? (prefs.savedPassword || '') : '';
}

function scheduleApplyLoginPreferencesToForm() {
    if (loginPrefsApplyTimer) {
        clearTimeout(loginPrefsApplyTimer);
    }
    loginPrefsApplyTimer = setTimeout(() => {
        loginPrefsApplyTimer = null;
        applyLoginPreferencesToForm();
    }, 120);
}

function tryAutoLoginFromStoredPrefs() {
    return;
    if (window.__autoLoginAttempted || window.__currentCloudLoggedIn) {
        return;
    }

    const prefs = window.__loginPrefsState || {};
    const loginMode = prefs.savedLoginMode === 'card' ? 'card' : 'account';
    const missingCredential = !prefs.savedUsername;
    const missingPassword = loginMode !== 'card' && !prefs.savedPassword;
    if (!prefs.autoLogin || !prefs.rememberPassword || missingCredential || missingPassword) {
        return;
    }

    window.__autoLoginAttempted = true;
    applyLoginModeUi(loginMode);
    setTimeout(() => {
        if (window.__currentCloudLoggedIn) {
            return;
        }
        submitCloudLoginRequest(
            prefs.savedUsername,
            prefs.savedPassword,
            { rememberPassword: true, autoLogin: true, source: 'auto', loginType: loginMode }
        );
    }, 0);
}

function handleAppConfigData(data) {
    window.__appRuntimeConfig = {
        fastMode: !!data.fastMode,
        enableLogging: data.enableLogging !== false,
        autoScrollLog: data.autoScrollLog !== false
    };

    applyFastModeUiState(!!data.fastMode);
    updateLogsDisabledNotice();

    const autoScrollCheckbox = $('logAutoScroll');
    if (autoScrollCheckbox) {
        autoScrollCheckbox.checked = window.__appRuntimeConfig.autoScrollLog;
    }

    window.__loginPrefsState = {
        savedLoginMode: data.savedLoginMode === 'card' ? 'card' : 'account',
        rememberPassword: !!data.rememberPassword,
        autoLogin: !!data.autoLogin,
        savedUsername: data.savedUsername || '',
        savedPassword: data.savedPassword || ''
    };
    applyLoginPreferencesToForm();
    scheduleApplyLoginPreferencesToForm();
    tryAutoLoginFromStoredPrefs();
}

function submitCloudLoginRequest(username, password, options = {}) {
    const loginType = options.loginType === 'card' ? 'card' : getCurrentLoginMode();
    const rememberPassword = options.rememberPassword === true;
    const autoLogin = options.autoLogin === true;
    const loginSource = options.source === 'auto' ? 'auto' : 'manual';
    window.__pendingCloudLoginAttempt = {
        username,
        password,
        loginType,
        isTrial: false,
        rememberPassword,
        autoLogin,
        source: loginSource
    };
    setCloudLoginButtonsLoading(true, false);
    const payload = { rememberPassword, autoLogin, loginSource, loginType };
    if (loginType === 'card') {
        payload.card = username;
    } else {
        payload.username = username;
        payload.password = password;
    }
    postAction('cloud_login', payload);
}

function formatCloudInitTime(ts) {
    const value = Number(ts);
    if (!Number.isFinite(value) || value <= 0) {
        return '-';
    }

    const date = new Date(value * 1000);
    if (Number.isNaN(date.getTime())) {
        return '-';
    }
    return date.toLocaleString('zh-CN', { hour12: false });
}

function buildMaxOnlineClientsTableRows(clients) {
    return clients.map(client => {
        const cid = Number(client.cid) || 0;
        const busy = window.__cloudLoginKickInProgressCid === cid;
        const buttonText = busy ? '处理中...' : '踢下线并重试';
        return `
            <tr>
                <td>${escapeHtml(cid)}</td>
                <td>${escapeHtml(client.computerName || '-')}</td>
                <td>${escapeHtml(client.winVer || '-')}</td>
                <td>${escapeHtml(client.pcSignMasked || '-')}</td>
                <td>${escapeHtml(formatCloudInitTime(client.cloudInitTs))}</td>
                <td><button class="btn btn-danger btn-sm" onclick="kickCloudLoginClient(${cid})" ${busy ? 'disabled' : ''}>${buttonText}</button></td>
            </tr>
        `;
    }).join('');
}

function showMaxOnlineKickDialog(data) {
    const clients = Array.isArray(data.onlineClients) ? data.onlineClients : [];
    const hint = escapeHtml(data.error || '已达到最大在线数量，请先踢下线一个在线客户端，再重试登录。');
    const queryError = data.onlineClientsError ? escapeHtml(data.onlineClientsError) : '';

    let bodyHTML = `<p style="margin:0 0 12px 0; line-height:1.7;">${hint}</p>`;
    if (queryError) {
        bodyHTML += `
            <div class="notice-area" style="display:block; margin-bottom: 12px;">
                <div class="notice-header"><span>在线列表获取失败</span></div>
                <div class="notice-body">${queryError}</div>
            </div>
        `;
    }

    if (clients.length > 0) {
        bodyHTML += `
            <div style="max-height: 320px; overflow:auto; border:1px solid rgba(255,255,255,0.08); border-radius:12px;">
                <table class="data-table" style="margin:0;">
                    <thead>
                        <tr>
                            <th>CID</th>
                            <th>计算机名</th>
                            <th>系统</th>
                            <th>机器码</th>
                            <th>上线时间</th>
                            <th>操作</th>
                        </tr>
                    </thead>
                    <tbody>${buildMaxOnlineClientsTableRows(clients)}</tbody>
                </table>
            </div>
        `;
    } else if (!queryError) {
        bodyHTML += `
            <div class="notice-area" style="display:block;">
                <div class="notice-header"><span>暂无在线客户端列表</span></div>
                <div class="notice-body">当前无法列出在线客户端，请稍后重试或联系管理员。</div>
            </div>
        `;
    }

    showModal('达到最大在线数', bodyHTML, '<button class="btn btn-ghost" onclick="closeModal()">稍后处理</button>');
}

function kickCloudLoginClient(cid) {
    const pending = window.__pendingCloudLoginAttempt;
    const loginType = pending?.loginType === 'card' ? 'card' : 'account';
    const missingCredential = !pending || !pending.username;
    const missingPassword = loginType !== 'card' && !pending?.password;
    if (missingCredential || missingPassword) {
        showToast('warning', '提示', loginType === 'card'
            ? '缺少本次卡密登录凭据，请重新输入卡密后再试'
            : '缺少本次登录凭据，请重新输入账号密码后再试');
        closeModal();
        return;
    }

    const numericCid = Number(cid);
    if (!Number.isFinite(numericCid) || numericCid <= 0) {
        showToast('error', '踢下线失败', 'CID 无效');
        return;
    }

    window.__cloudLoginKickInProgressCid = numericCid;
    showMaxOnlineKickDialog({
        error: '正在踢下线所选客户端，请稍候...',
        onlineClients: Array.isArray(window.__lastMaxOnlineClients) ? window.__lastMaxOnlineClients : []
    });
    postAction('cloud_kick_online_by_cid', {
        loginType,
        username: loginType === 'card' ? '' : pending.username,
        password: loginType === 'card' ? '' : pending.password,
        card: loginType === 'card' ? pending.username : '',
        cid: numericCid
    });
}

function handleCloudKickOnlineResult(data) {
    const cid = Number(data.cid) || 0;
    if (window.__cloudLoginKickInProgressCid === cid) {
        window.__cloudLoginKickInProgressCid = 0;
    }

    if (data.success) {
        const pending = window.__pendingCloudLoginAttempt;
        closeModal();
        showToast('success', '踢下线成功', `已踢下线 CID=${cid}，正在重新登录`);
        if (pending && pending.username && pending.password) {
            submitCloudLoginRequest(pending.username, pending.password, { loginType: pending.loginType });
        } else if (pending && pending.loginType === 'card' && pending.username) {
            submitCloudLoginRequest(pending.username, '', { loginType: 'card' });
        }
    } else {
        showToast('error', '踢下线失败', data.error || '未知错误');
        showMaxOnlineKickDialog({
            error: data.error || '踢下线失败',
            onlineClients: Array.isArray(window.__lastMaxOnlineClients) ? window.__lastMaxOnlineClients : []
        });
    }
}

function setLoginScreenState(active) {
    if (loginScreen) loginScreen.style.display = active ? 'flex' : 'none';
    if (mainApp) mainApp.style.display = active ? 'none' : 'flex';
    document.documentElement.classList.toggle('login-screen-active', active);
    if (document.body) {
        document.body.classList.toggle('login-screen-active', active);
    }
}

// Show login screen, hide main app
function showLoginScreen() {
    setLogsRealtimeEnabled(false);
    closeMobileNav();
    setLoginScreenState(false);
}

// Show main app, hide login screen
function showMainApp() {
    setLoginScreenState(false);
    if (currentMainPage === 'logs') {
        setLogsRealtimeEnabled(true);
    }
}

setLoginScreenState(false);
initMobileNav();

function getRemoteBrowserElement(prefix, suffix) {
    if (prefix === 'app') {
        return $(prefix + suffix);
    }

    const localId = suffix.charAt(0).toLowerCase() + suffix.slice(1);
    return $(localId);
}
function updateRemoteBrowserPanel(prefix, data) {
    const id = suffix => getRemoteBrowserElement(prefix, suffix);
    const enabledCheckbox = id('EnableRemoteBrowserMode');
    const bindHostInput = id('RemoteBindHost');
    const bindPortInput = id('RemoteBindPort');
    const accessTokenInput = id('RemoteAccessToken');
    const configArea = id('RemoteModeConfigArea');
    const urlArea = id('RemoteModeUrlArea');
    const urlText = id('RemoteModeUrlText');

    if (enabledCheckbox) enabledCheckbox.checked = data.enabled === true;
    if (bindHostInput) bindHostInput.value = data.bindHost || '0.0.0.0';
    if (bindPortInput) bindPortInput.value = data.port || 18080;
    if (accessTokenInput) accessTokenInput.value = data.accessToken || '';
    if (configArea) configArea.style.display = data.enabled ? 'block' : 'none';

    const accessUrl = data.accessUrl || '';
    if (urlArea) urlArea.style.display = accessUrl ? 'flex' : 'none';
    if (urlText) urlText.textContent = accessUrl;

    const error = data.error || '';
    const errorId = id('RemoteModeError');
    if (errorId) {
        errorId.textContent = error;
        errorId.style.display = error ? 'block' : 'none';
    }
}

function collectRemoteBrowserConfig(prefix) {
    const id = suffix => getRemoteBrowserElement(prefix, suffix);
    return {
        enabled: id('EnableRemoteBrowserMode')?.checked === true,
        bindHost: id('RemoteBindHost')?.value?.trim() || '0.0.0.0',
        port: parseInt(id('RemoteBindPort')?.value, 10) || 18080,
        accessToken: id('RemoteAccessToken')?.value?.trim() || ''
    };
}

function generateRemoteAccessToken() {
    const bytes = new Uint8Array(16);
    if (window.crypto && window.crypto.getRandomValues) {
        window.crypto.getRandomValues(bytes);
    } else {
        for (let i = 0; i < bytes.length; i++) {
            bytes[i] = Math.floor(Math.random() * 256);
        }
    }

    return Array.from(bytes)
        .map(byte => byte.toString(16).padStart(2, '0'))
        .join('');
}

function fillRemoteAccessToken(prefix = '') {
    const input = getRemoteBrowserElement(prefix, 'RemoteAccessToken');
    if (!input) return;
    input.value = generateRemoteAccessToken();
}

function applyRemoteBrowserMode(prefix = '') {
    if (remoteBridgeState.isRemoteClient) {
        showToast('warning', '受限操作', '远程会话中不允许修改远程浏览器服务配置');
        return;
    }

    const config = collectRemoteBrowserConfig(prefix);
    postAction('remote_browser_set_mode', config);
}

function copyRemoteBrowserUrl(prefix = '') {
    const id = suffix => getRemoteBrowserElement(prefix, suffix);
    const url = id('RemoteModeUrlText')?.textContent || '';
    if (!url) return;
    copyToClipboard(url);
}

if (btnRemoteModeApply) {
    btnRemoteModeApply.addEventListener('click', () => applyRemoteBrowserMode(''));
}
if (btnCopyRemoteUrl) {
    btnCopyRemoteUrl.addEventListener('click', () => copyRemoteBrowserUrl(''));
}
if ($('btnGenerateRemoteToken')) {
    $('btnGenerateRemoteToken').addEventListener('click', () => fillRemoteAccessToken(''));
}
if ($('appBtnRemoteModeApply')) {
    $('appBtnRemoteModeApply').addEventListener('click', () => applyRemoteBrowserMode('app'));
}
if ($('appBtnCopyRemoteUrl')) {
    $('appBtnCopyRemoteUrl').addEventListener('click', () => copyRemoteBrowserUrl('app'));
}
if ($('appBtnGenerateRemoteToken')) {
    $('appBtnGenerateRemoteToken').addEventListener('click', () => fillRemoteAccessToken('app'));
}
if ($('enableRemoteBrowserMode')) {
    $('enableRemoteBrowserMode').addEventListener('change', () => {
        const area = $('remoteModeConfigArea');
        if (area) area.style.display = $('enableRemoteBrowserMode').checked ? 'block' : 'none';
    });
}
if ($('appEnableRemoteBrowserMode')) {
    $('appEnableRemoteBrowserMode').addEventListener('change', () => {
        const area = $('appRemoteModeConfigArea');
        if (area) area.style.display = $('appEnableRemoteBrowserMode').checked ? 'block' : 'none';
    });
}
if (loginPasswordToggle) {
    loginPasswordToggle.addEventListener('click', () => {
        setLoginPasswordVisibility(loginPassword?.type === 'password');
    });
}
if (registerPasswordToggle) {
    registerPasswordToggle.addEventListener('click', () => {
        togglePasswordFieldVisibility('registerPassword', 'registerPasswordToggle');
    });
}
if (registerSuperPasswordToggle) {
    registerSuperPasswordToggle.addEventListener('click', () => {
        togglePasswordFieldVisibility('registerSuperPassword', 'registerSuperPasswordToggle');
    });
}
if (renewPasswordToggle) {
    renewPasswordToggle.addEventListener('click', () => {
        togglePasswordFieldVisibility('renewPassword', 'renewPasswordToggle');
    });
}
if (loginModeAccount) {
    loginModeAccount.addEventListener('click', () => applyLoginModeUi('account'));
}
if (loginModeCard) {
    loginModeCard.addEventListener('click', () => applyLoginModeUi('card'));
}
if (autoLoginCheckbox) {
    autoLoginCheckbox.addEventListener('change', () => {
        if (autoLoginCheckbox.checked && rememberPasswordCheckbox) {
            rememberPasswordCheckbox.checked = true;
        }
    });
}
if (rememberPasswordCheckbox) {
    rememberPasswordCheckbox.addEventListener('change', () => {
        if (!rememberPasswordCheckbox.checked && autoLoginCheckbox?.checked) {
            autoLoginCheckbox.checked = false;
        }
    });
}

if (btnLoginSubmit && loginUsername && loginPassword) {
    btnLoginSubmit.addEventListener('click', () => {
        const loginType = getCurrentLoginMode();
        const username = loginUsername.value.trim();
        const password = loginType === 'card' ? '' : loginPassword.value.trim();

        if (!username || (loginType !== 'card' && !password)) {
            showToast('warning', '提示', loginType === 'card' ? '请输入卡密' : '请输入账号和密码');
            return;
        }

        submitCloudLoginRequest(username, password, {
            rememberPassword: !!rememberPasswordCheckbox?.checked,
            autoLogin: !!autoLoginCheckbox?.checked,
            source: 'manual',
            loginType
        });
    });
}

if (btnTrialLoginSubmit) {
    btnTrialLoginSubmit.addEventListener('click', () => {
        window.__pendingCloudLoginAttempt = null;
        setCloudLoginButtonsLoading(true, true);
        postAction('trial_login', {});
    });
}

// Allow Enter key to submit login
if (loginPassword && btnLoginSubmit) {
    loginPassword.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') btnLoginSubmit.click();
    });
}
if (loginUsername && loginPassword) {
    loginUsername.addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
            if (getCurrentLoginMode() === 'card') {
                btnLoginSubmit?.click();
            } else {
                loginPassword.focus();
            }
        }
    });
}

applyLoginModeUi(getCurrentLoginMode());

// ========== Home Page (主菜单) ==========
const btnLogout = $('btnLogout');

if (btnLogout) {
    btnLogout.addEventListener('click', () => {
        if (confirm('确定要登出吗？')) {
            window.__pendingCloudLoginAttempt = null;
            postAction('cloud_logout');
        }
    });
}

function applyFastModeUiState(enabled) {
    const checkbox = $('fastModeEnabled');
    window.__appRuntimeConfig.fastMode = !!enabled;
    window.__appRuntimeConfig.enableLogging = !enabled;
    if (!checkbox) return;

    checkbox.checked = !!enabled;
}

function loadAppConfig() {
    postAction('get_app_config');
}

function toggleFastMode() {
    const checkbox = $('fastModeEnabled');
    if (!checkbox) return;

    const enabled = !!checkbox.checked;
    window.__appRuntimeConfig.fastMode = enabled;
    window.__appRuntimeConfig.enableLogging = !enabled;
    updateLogsDisabledNotice();
    lastFilteredLogsKey = '';
    if (isMainPageActive('logs')) {
        filterLogs();
    }
    postAction('set_fast_mode', { enabled });

    showToast(
        'info',
        '极速模式',
        enabled ? '已开启：停止全部日志输出' : '已关闭：日志输出已恢复'
    );
}

function isLoggingEnabled() {
    return window.__appRuntimeConfig.enableLogging !== false;
}

function updateLogsDisabledNotice() {
    const notice = $('logsDisabledNotice');
    const subtitle = $('logsPageSubtitle');
    const loggingEnabled = isLoggingEnabled();

    if (notice) {
        notice.style.display = loggingEnabled ? 'none' : 'block';
    }

    if (subtitle) {
        subtitle.textContent = loggingEnabled
            ? '查看系统日志'
            : '日志记录已关闭，当前不会产生新的日志';
    }
}

function restoreLoggingFromLogsPage() {
    const checkbox = $('fastModeEnabled');
    if (checkbox) {
        checkbox.checked = false;
        toggleFastMode();
        return;
    }

    window.__appRuntimeConfig.fastMode = false;
    window.__appRuntimeConfig.enableLogging = true;
    updateLogsDisabledNotice();
    postAction('set_fast_mode', { enabled: false });
    showToast('info', '日志恢复', '已请求关闭极速模式，新的日志将重新开始记录');
}

// Recharge button
const btnRecharge = $('btnRecharge');
if (btnRecharge) {
    btnRecharge.addEventListener('click', () => {
    const cards = $('rechargeCards').value.trim();
    const messageArea = $('rechargeMessage');

    if (!cards) {
        messageArea.textContent = '请输入充值卡';
        messageArea.className = 'message-area error';
        messageArea.style.display = 'block';
        return;
    }

    // Disable button during processing
    const btn = $('btnRecharge');
    btn.disabled = true;
    btn.textContent = '充值中...';

    messageArea.style.display = 'none';

    postAction('home_recharge', { cards });
    });
}

// ========== Instances Page (实例管理) ==========
$('btnCreateInstance').addEventListener('click', () => {
    showModal('创建Socks转发实例', `
        <div class="form-group">
            <label for="newInstanceName">实例名称</label>
            <input type="text" id="newInstanceName" class="input" placeholder="请输入实例名称">
        </div>
        <div class="form-group">
            <label for="newInstancePort">监听端口</label>
            <input type="number" id="newInstancePort" class="input" placeholder="例如: 1080" value="1080">
        </div>
        <p style="color: #888; font-size: 0.9em; margin-top: 10px;">
            (纯转发模式，不记录数据包，支持WPE滤镜和二级代理)
        </p>
    `, `
        <button class="btn btn-ghost" onclick="closeModal()">取消</button>
        <button class="btn btn-primary" onclick="createInstance()">创建</button>
    `);
});

function createInstance() {
    const name = $('newInstanceName').value.trim();
    const port = parseInt($('newInstancePort').value);

    if (!name) {
        showToast('warning', '提示', '请输入实例名称');
        return;
    }

    if (!port || port < 1 || port > 65535) {
        showToast('warning', '提示', '请输入有效的端口号(1-65535)');
        return;
    }

    // 固定创建SocksForward类型
    postAction('instance_create', { name, type: 'SocksForward', port });
    closeModal();
}

$('btnRefreshInstances').addEventListener('click', () => {
    postAction('get_instances');
});

const btnExportInstances = $('btnExportInstances');
if (btnExportInstances) {
    btnExportInstances.addEventListener('click', () => {
        postAction('socks_instances_export_file');
    });
}

const btnImportInstances = $('btnImportInstances');
if (btnImportInstances) {
    btnImportInstances.addEventListener('click', () => {
        postAction('socks_instances_import_file');
    });
}

function buildInstanceRowKey(inst) {
    return [
        inst.id,
        inst.type,
        inst.name,
        inst.port,
        inst.state,
        inst.currentConnections || 0
    ].join(',');
}

function buildInstanceRowHtml(inst) {
    return `
        <div class="list-item">
            <div class="list-item-info">
                <div class="list-item-title">${inst.name}</div>
                <div class="list-item-subtitle">
                    类型: ${getInstanceTypeName(inst.type)} | 端口: ${inst.port} |
                    状态: ${getInstanceStateName(inst.state)} |
                    连接数: ${inst.currentConnections || 0}
                </div>
            </div>
            <div class="list-item-actions">
                ${inst.state === 'Stopped' ?
                    `<button class="btn btn-success" onclick="startInstance('${inst.id}')">启动</button>` :
                    `<button class="btn btn-warning" onclick="stopInstance('${inst.id}')">停止</button>`
                }
                <button class="btn btn-ghost" onclick="configInstance('${inst.id}')">配置</button>
                <button class="btn btn-danger" onclick="deleteInstance('${inst.id}')">删除</button>
            </div>
        </div>
    `;
}

function syncInstanceRows(instances) {
    const container = $('instanceList');
    if (!container) return;

    const nextRowKeys = (instances || []).map(buildInstanceRowKey);
    const rows = container.querySelectorAll(':scope > .list-item');

    if (rows.length === nextRowKeys.length && lastInstanceRowKeys.length === nextRowKeys.length) {
        for (let i = 0; i < nextRowKeys.length; i++) {
            if (lastInstanceRowKeys[i] !== nextRowKeys[i]) {
                rows[i].outerHTML = buildInstanceRowHtml(instances[i]);
            }
        }
    } else {
        container.innerHTML = (instances || []).map(buildInstanceRowHtml).join('');
    }

    lastInstanceRowKeys = nextRowKeys;
}

function renderInstances(instances) {
    window.cachedInstances = instances || [];
    renderProxyInstanceList(window.cachedInstances);

    if (!isMainPageActive('instances')) {
        return;
    }

    const container = $('instanceList');
    if (!container) return;

    const filteredInstances = window.cachedInstances.filter(inst => inst.type !== 'Socks5Pool');
    const renderKey = `${instancesDataRevision}|${filteredInstances.length}`;

    if (renderKey === lastInstancesRenderKey) {
        return;
    }
    lastInstancesRenderKey = renderKey;

    if (filteredInstances.length === 0) {
        lastInstanceRowKeys = [];
        container.innerHTML = '<div class="empty-state">暂无实例，点击"创建实例"开始</div>';
        return;
    }

    syncInstanceRows(filteredInstances);
}

function getInstanceTypeName(type) {
    const names = {
        'Collector': '单伪采集',
        'Heartbeat': '单伪伪心跳',
        'AbCollector': 'ab采集',
        'AbHeartbeat': 'ab伪心跳',
        'Socks5Pool': 'SOCKS5账号库',
        'SocksForward': 'Socks转发'
    };
    return names[type] || type;
}

function getInstanceStateName(state) {
    const names = {
        'Stopped': '已停止',
        'Starting': '启动中',
        'Running': '运行中',
        'Stopping': '停止中',
        'Error': '错误'
    };
    return names[state] || state;
}

function startInstance(id) {
    postAction('instance_start', { id });
}

function stopInstance(id) {
    postAction('instance_stop', { id });
}

function deleteInstance(id) {
    if (confirm('确定要删除此实例吗？')) {
        postAction('instance_delete', { id });
    }
}

// Global variable to store current config instance
let currentConfigInstance = null;

function configInstance(id) {
    // Navigate to config page instead of modal
    const instances = window.cachedInstances || [];
    const inst = instances.find(i => i.id === id);
    if (inst) {
        currentConfigInstance = inst;
        showInstanceConfigPage(inst);
    } else {
        postAction('instance_get_config', { id });
    }
}

// Show instance configuration panel
function showInstanceConfig(instanceData) {
    currentConfigInstance = instanceData;
    const type = instanceData.type;

    if (type === 'SocksForward') {
        // Navigate to dedicated config page for SocksForward
        showInstanceConfigPage(instanceData);
        return;
    }

    let configHTML = '';

    if (type === 'Collector' || type === 'AbCollector') {
        configHTML = renderCollectorConfig(instanceData);
    } else if (type === 'Heartbeat' || type === 'AbHeartbeat') {
        configHTML = renderHeartbeatConfig(instanceData);
    } else if (type === 'Socks5Pool') {
        configHTML = renderSocks5PoolConfig(instanceData);
    }

    showModal(`配置实例: ${instanceData.name}`, configHTML, `
        <button class="btn btn-ghost" onclick="closeModal()">关闭</button>
        <button class="btn btn-primary" onclick="saveInstanceConfig()">保存配置</button>
    `);
}

// ========== Collector Instance Config ==========
function renderCollectorConfig(inst) {
    const config = inst.config || {};
    return `
        <div class="config-panel">
            <div class="config-section">
                <h3 class="section-title">基本信息</h3>
                <div class="form-row">
                    <div class="form-group">
                        <label>实例名称</label>
                        <input type="text" class="input" value="${inst.name}" disabled>
                    </div>
                    <div class="form-group">
                        <label>监听端口</label>
                        <input type="number" class="input" value="${inst.port}" disabled>
                    </div>
                </div>
                <div class="form-row">
                    <div class="form-group">
                        <label>当前连接数</label>
                        <input type="text" class="input" value="${inst.currentConnections || 0}" disabled>
                    </div>
                    <div class="form-group">
                        <label>总数据包数</label>
                        <input type="text" class="input" value="${inst.totalPackets || 0}" disabled>
                    </div>
                </div>
            </div>

            <div class="config-section">
                <h3 class="section-title">存储模式</h3>
                <div class="form-group">
                    <label>
                        <input type="radio" name="storageMode" value="memory" ${config.storageMode === 'memory' ? 'checked' : ''}>
                        内存模式（快速，重启丢失）
                    </label>
                </div>
                <div class="form-group">
                    <label>
                        <input type="radio" name="storageMode" value="database" ${config.storageMode !== 'memory' ? 'checked' : ''}>
                        数据库模式（持久化）
                    </label>
                </div>
            </div>

            <div class="config-section">
                <h3 class="section-title">SOCKS5认证</h3>
                <div class="form-group">
                    <label>
                        <input type="checkbox" id="socks5AuthEnabled" ${config.socks5AuthEnabled ? 'checked' : ''}>
                        启用SOCKS5认证
                    </label>
                </div>
                <div class="form-row">
                    <div class="form-group">
                        <label for="socks5Username">用户名</label>
                        <input type="text" id="socks5Username" class="input" value="${config.socks5Username || ''}" placeholder="留空则不需要认证">
                    </div>
                    <div class="form-group">
                        <label for="socks5Password">密码</label>
                        <input type="password" id="socks5Password" class="input" value="${config.socks5Password || ''}" placeholder="留空则不需要认证">
                    </div>
                </div>
            </div>

            <div class="config-section">
                <h3 class="section-title">二级代理</h3>
                <div class="form-group">
                    <label>
                        <input type="checkbox" id="secondaryProxyEnabled" ${config.secondaryProxyEnabled ? 'checked' : ''}>
                        启用二级代理
                    </label>
                </div>
                <div class="form-row">
                    <div class="form-group">
                        <label for="proxyHost">代理地址</label>
                        <input type="text" id="proxyHost" class="input" value="${config.proxyHost || ''}" placeholder="例如: 127.0.0.1">
                    </div>
                    <div class="form-group">
                        <label for="proxyPort">代理端口</label>
                        <input type="number" id="proxyPort" class="input" value="${config.proxyPort || 1080}" placeholder="1080">
                    </div>
                </div>
            </div>

            <div class="config-section">
                <h3 class="section-title">数据包过滤</h3>
                <div class="form-group">
                    <label for="minPacketSize">最小包大小（字节）</label>
                    <input type="number" id="minPacketSize" class="input" value="${config.minPacketSize || 0}" placeholder="0表示不限制">
                </div>
                <div class="form-group">
                    <label for="maxPacketSize">最大包大小（字节）</label>
                    <input type="number" id="maxPacketSize" class="input" value="${config.maxPacketSize || 65535}" placeholder="65535">
                </div>
                <div class="form-group">
                    <label>
                        <input type="checkbox" id="filterDuplicates" ${config.filterDuplicates ? 'checked' : ''}>
                        过滤重复数据包
                    </label>
                </div>
            </div>

            <div class="config-section">
                <h3 class="section-title">性能优化</h3>
                <div class="form-row">
                    <div class="form-group">
                        <label for="workerThreads">工作线程数</label>
                        <input type="number" id="workerThreads" class="input" value="${config.workerThreads || 4}" min="1" max="32">
                    </div>
                    <div class="form-group">
                        <label for="bufferSize">缓冲区大小（KB）</label>
                        <input type="number" id="bufferSize" class="input" value="${config.bufferSize || 64}" min="8" max="1024">
                    </div>
                </div>
            </div>

            <div class="config-section">
                <h3 class="section-title">AntiCC防护</h3>
                <div class="form-group">
                    <label>
                        <input type="checkbox" id="anticcEnabled" ${config.anticcEnabled ? 'checked' : ''}>
                        启用AntiCC防护
                    </label>
                </div>
                <div class="form-row">
                    <div class="form-group">
                        <label for="anticcTimeWindow">时间窗口（秒）</label>
                        <input type="number" id="anticcTimeWindow" class="input" value="${config.anticcTimeWindow || 10}">
                    </div>
                    <div class="form-group">
                        <label for="anticcMaxConnections">最大连接数</label>
                        <input type="number" id="anticcMaxConnections" class="input" value="${config.anticcMaxConnections || 20}">
                    </div>
                </div>
            </div>
        </div>
    `;
}

// ========== Heartbeat Instance Config ==========
function renderHeartbeatConfig(inst) {
    const config = inst.config || {};
    return `
        <div class="config-panel">
            <div class="config-tabs">
                <button class="tab-btn active" onclick="switchConfigTab('basic')">基本配置</button>
                <button class="tab-btn" onclick="switchConfigTab('pattern23')">Pattern 23规则</button>
                <button class="tab-btn" onclick="switchConfigTab('pattern09')">Pattern 09规则</button>
                <button class="tab-btn" onclick="switchConfigTab('pattern62')">Pattern 62规则</button>
                <button class="tab-btn" onclick="switchConfigTab('whitelist')">白名单</button>
                <button class="tab-btn" onclick="switchConfigTab('blacklist')">黑名单</button>
                <button class="tab-btn" onclick="switchConfigTab('replacecount')">替换次数</button>
            </div>

            <div id="tab-basic" class="tab-content active">
                <div class="config-section">
                    <h3 class="section-title">基本信息</h3>
                    <div class="form-row">
                        <div class="form-group">
                            <label>实例名称</label>
                            <input type="text" class="input" value="${inst.name}" disabled>
                        </div>
                        <div class="form-group">
                            <label>监听端口</label>
                            <input type="number" class="input" value="${inst.port}" disabled>
                        </div>
                    </div>
                </div>

                <div class="config-section">
                    <h3 class="section-title">替换模式</h3>
                    <div class="form-group">
                        <label>
                            <input type="radio" name="replaceMode" value="random" ${config.replaceMode === 'random' ? 'checked' : ''}>
                            随机替换
                        </label>
                    </div>
                    <div class="form-group">
                        <label>
                            <input type="radio" name="replaceMode" value="sequential" ${config.replaceMode === 'sequential' ? 'checked' : ''}>
                            顺序替换
                        </label>
                    </div>
                    <div class="form-group">
                        <label>
                            <input type="radio" name="replaceMode" value="fixed" ${config.replaceMode === 'fixed' ? 'checked' : ''}>
                            固定包替换
                        </label>
                    </div>
                </div>

                <div class="config-section">
                    <h3 class="section-title">数据源配置</h3>
                    <div class="form-group">
                        <label for="dataSource">数据源类型</label>
                        <select id="dataSource" class="input">
                            <option value="memory" ${config.dataSource === 'memory' ? 'selected' : ''}>内存池</option>
                            <option value="database" ${config.dataSource === 'database' ? 'selected' : ''}>数据库</option>
                            <option value="remote" ${config.dataSource === 'remote' ? 'selected' : ''}>远程服务器</option>
                        </select>
                    </div>
                    <div class="form-group">
                        <label>
                            <input type="checkbox" id="enableCRC32" ${config.enableCRC32 ? 'checked' : ''}>
                            启用CRC32校验
                        </label>
                    </div>
                </div>
            </div>

            <div id="tab-pattern23" class="tab-content">
                <div class="config-section">
                    <h3 class="section-title">Pattern 23 偏移规则</h3>
                    <button class="btn btn-primary" onclick="addPattern23Rule()">添加规则</button>
                    <div id="pattern23RuleList" class="rule-list">
                        ${renderPattern23Rules(config.pattern23Rules || [])}
                    </div>
                </div>
            </div>

            <div id="tab-pattern09" class="tab-content">
                <div class="config-section">
                    <h3 class="section-title">Pattern 09 偏移规则</h3>
                    <button class="btn btn-primary" onclick="addPattern09Rule()">添加规则</button>
                    <div id="pattern09RuleList" class="rule-list">
                        ${renderPattern09Rules(config.pattern09Rules || [])}
                    </div>
                </div>
            </div>

            <div id="tab-pattern62" class="tab-content">
                <div class="config-section">
                    <h3 class="section-title">Pattern 62 偏移规则</h3>
                    <button class="btn btn-primary" onclick="addPattern62Rule()">添加规则</button>
                    <div id="pattern62RuleList" class="rule-list">
                        ${renderPattern62Rules(config.pattern62Rules || [])}
                    </div>
                </div>
            </div>

            <div id="tab-whitelist" class="tab-content">
                <div class="config-section">
                    <h3 class="section-title">白名单规则</h3>
                    <button class="btn btn-primary" onclick="addWhitelistRule()">添加规则</button>
                    <div id="whitelistRuleList" class="rule-list">
                        ${renderWhitelistRules(config.whitelistRules || [])}
                    </div>
                </div>
            </div>

            <div id="tab-blacklist" class="tab-content">
                <div class="config-section">
                    <h3 class="section-title">黑名单规则</h3>
                    <button class="btn btn-primary" onclick="addBlacklistRule()">添加规则</button>
                    <div id="blacklistRuleList" class="rule-list">
                        ${renderBlacklistRules(config.blacklistRules || [])}
                    </div>
                </div>
            </div>

            <div id="tab-replacecount" class="tab-content">
                <div class="config-section">
                    <h3 class="section-title">替换次数规则</h3>
                    <button class="btn btn-primary" onclick="addReplaceCountRule()">添加规则</button>
                    <div id="replaceCountRuleList" class="rule-list">
                        ${renderReplaceCountRules(config.replaceCountRules || [])}
                    </div>
                </div>
            </div>
        </div>
    `;
}

function switchConfigTab(tabName) {
    document.querySelectorAll('.tab-btn').forEach(btn => btn.classList.remove('active'));
    document.querySelectorAll('.tab-content').forEach(content => content.classList.remove('active'));

    event.target.classList.add('active');
    document.getElementById(`tab-${tabName}`).classList.add('active');
}

function renderPattern23Rules(rules) {
    if (!rules || rules.length === 0) {
        return '<div class="empty-state">暂无规则</div>';
    }
    return rules.map((rule, idx) => `
        <div class="rule-item">
            <div class="rule-info">
                <div>偏移: ${rule.offset} | 长度: ${rule.length} | 值: ${rule.value}</div>
            </div>
            <div class="rule-actions">
                <button class="btn btn-ghost" onclick="editPattern23Rule(${idx})">编辑</button>
                <button class="btn btn-danger" onclick="deletePattern23Rule(${idx})">删除</button>
            </div>
        </div>
    `).join('');
}

function renderPattern09Rules(rules) {
    if (!rules || rules.length === 0) {
        return '<div class="empty-state">暂无规则</div>';
    }
    return rules.map((rule, idx) => `
        <div class="rule-item">
            <div class="rule-info">
                <div>偏移: ${rule.offset} | 长度: ${rule.length} | 值: ${rule.value}</div>
            </div>
            <div class="rule-actions">
                <button class="btn btn-ghost" onclick="editPattern09Rule(${idx})">编辑</button>
                <button class="btn btn-danger" onclick="deletePattern09Rule(${idx})">删除</button>
            </div>
        </div>
    `).join('');
}

function renderPattern62Rules(rules) {
    if (!rules || rules.length === 0) {
        return '<div class="empty-state">暂无规则</div>';
    }
    return rules.map((rule, idx) => `
        <div class="rule-item">
            <div class="rule-info">
                <div>偏移: ${rule.offset} | 长度: ${rule.length} | 值: ${rule.value}</div>
            </div>
            <div class="rule-actions">
                <button class="btn btn-ghost" onclick="editPattern62Rule(${idx})">编辑</button>
                <button class="btn btn-danger" onclick="deletePattern62Rule(${idx})">删除</button>
            </div>
        </div>
    `).join('');
}

function renderWhitelistRules(rules) {
    if (!rules || rules.length === 0) {
        return '<div class="empty-state">暂无规则</div>';
    }
    return rules.map((rule, idx) => `
        <div class="rule-item">
            <div class="rule-info">
                <div>IP: ${rule.ip || '任意'} | 端口: ${rule.port || '任意'}</div>
            </div>
            <div class="rule-actions">
                <button class="btn btn-ghost" onclick="editWhitelistRule(${idx})">编辑</button>
                <button class="btn btn-danger" onclick="deleteWhitelistRule(${idx})">删除</button>
            </div>
        </div>
    `).join('');
}

function renderBlacklistRules(rules) {
    if (!rules || rules.length === 0) {
        return '<div class="empty-state">暂无规则</div>';
    }
    return rules.map((rule, idx) => `
        <div class="rule-item">
            <div class="rule-info">
                <div>IP: ${rule.ip || '任意'} | 端口: ${rule.port || '任意'}</div>
            </div>
            <div class="rule-actions">
                <button class="btn btn-ghost" onclick="editBlacklistRule(${idx})">编辑</button>
                <button class="btn btn-danger" onclick="deleteBlacklistRule(${idx})">删除</button>
            </div>
        </div>
    `).join('');
}

function renderReplaceCountRules(rules) {
    if (!rules || rules.length === 0) {
        return '<div class="empty-state">暂无规则</div>';
    }
    return rules.map((rule, idx) => `
        <div class="rule-item">
            <div class="rule-info">
                <div>包ID: ${rule.packetId} | 替换次数: ${rule.count}</div>
            </div>
            <div class="rule-actions">
                <button class="btn btn-ghost" onclick="editReplaceCountRule(${idx})">编辑</button>
                <button class="btn btn-danger" onclick="deleteReplaceCountRule(${idx})">删除</button>
            </div>
        </div>
    `).join('');
}

// Rule management functions (stubs for now)
function addPattern23Rule() {
    showToast('info', '提示', 'Pattern 23规则添加功能待实现');
}

function addPattern09Rule() {
    showToast('info', '提示', 'Pattern 09规则添加功能待实现');
}

function addPattern62Rule() {
    showToast('info', '提示', 'Pattern 62规则添加功能待实现');
}

function addWhitelistRule() {
    showToast('info', '提示', '白名单规则添加功能待实现');
}

function addBlacklistRule() {
    showToast('info', '提示', '黑名单规则添加功能待实现');
}

function addReplaceCountRule() {
    showToast('info', '提示', '替换次数规则添加功能待实现');
}

// ========== Socks5Pool Instance Config ==========
function renderSocks5PoolConfig(inst) {
    const config = inst.config || {};
    return `
        <div class="config-panel">
            <div class="config-section">
                <h3 class="section-title">基本信息</h3>
                <div class="form-row">
                    <div class="form-group">
                        <label>账号库名称</label>
                        <input type="text" class="input" value="${inst.name}" disabled>
                    </div>
                    <div class="form-group">
                        <label>账号数量</label>
                        <input type="text" class="input" value="${config.accountCount || 0}" disabled>
                    </div>
                </div>
            </div>

            <div class="config-section">
                <h3 class="section-title">CCProxy API服务</h3>
                <div class="form-group">
                    <label>
                        <input type="checkbox" id="ccproxyEnabled" ${config.ccproxyEnabled ? 'checked' : ''}>
                        启用CCProxy API服务
                    </label>
                </div>
                <div class="form-row">
                    <div class="form-group">
                        <label for="ccproxyPort">API端口</label>
                        <input type="number" id="ccproxyPort" class="input" value="${config.ccproxyPort || 8080}">
                    </div>
                    <div class="form-group">
                        <label for="ccproxyKey">API密钥</label>
                        <input type="text" id="ccproxyKey" class="input" value="${config.ccproxyKey || ''}" placeholder="留空则不需要密钥">
                    </div>
                </div>
            </div>

            <div class="config-section">
                <h3 class="section-title">账号管理</h3>
                <div class="info-hint">请在左侧 SOCKS5账号库 面板中管理账号</div>
            </div>
        </div>
    `;
}

// ========== SocksForward Instance Config ==========
function renderSocksForwardConfig(inst) {
    const config = inst.config || {};
    return `
        <div class="config-panel">
            <div class="config-section">
                <h3 class="section-title">基本信息</h3>
                <div class="form-row">
                    <div class="form-group">
                        <label>实例ID</label>
                        <input type="text" class="input" value="${inst.id}" disabled>
                    </div>
                    <div class="form-group">
                        <label>实例名称</label>
                        <input type="text" class="input" value="${inst.name}" disabled>
                    </div>
                </div>
                <div class="form-row">
                    <div class="form-group">
                        <label>实例类型</label>
                        <input type="text" class="input" value="Socks转发 (支持WPE滤镜和二级代理)" disabled>
                    </div>
                    <div class="form-group">
                        <label>监听端口</label>
                        <input type="number" class="input" value="${inst.port}" disabled>
                    </div>
                </div>
                <div class="form-row">
                    <div class="form-group">
                        <label>状态</label>
                        <input type="text" class="input" value="${getInstanceStateName(inst.state)}" disabled>
                    </div>
                    <div class="form-group">
                        <label>当前连接数</label>
                        <input type="text" class="input" value="${inst.currentConnections || 0}" disabled>
                    </div>
                </div>
                <div class="form-row">
                    <div class="form-group">
                        <label>总连接数</label>
                        <input type="text" class="input" value="${inst.totalPackets || 0}" disabled>
                    </div>
                    <div class="form-group">
                        <label>总流量</label>
                        <input type="text" class="input" value="${((inst.totalBytes || 0) / 1024).toFixed(2)} KB" disabled>
                    </div>
                </div>
                <p style="color: #888; font-size: 0.9em; margin-top: 10px;">
                    提示: WPE滤镜规则请在WPE滤镜页面中配置并指定生效实例
                </p>
            </div>

            <div class="config-section">
                <h3 class="section-title">二级代理配置</h3>
                <div class="form-group">
                    <label>
                        <input type="checkbox" id="enableSecondaryProxy" ${config.enableSecondaryProxy ? 'checked' : ''}>
                        启用二级代理
                    </label>
                </div>
                <div class="form-row">
                    <div class="form-group">
                        <label for="secondaryProxyHost">代理地址</label>
                        <input type="text" id="secondaryProxyHost" class="input" value="${config.secondaryProxyHost || '127.0.0.1'}" placeholder="127.0.0.1">
                    </div>
                    <div class="form-group">
                        <label for="secondaryProxyPort">代理端口</label>
                        <input type="number" id="secondaryProxyPort" class="input" value="${config.secondaryProxyPort || 1080}" placeholder="1080">
                    </div>
                </div>
                <div class="form-row">
                    <div class="form-group">
                        <label for="secondaryProxyUsername">用户名（可选）</label>
                        <input type="text" id="secondaryProxyUsername" class="input" value="${config.secondaryProxyUsername || ''}" placeholder="留空表示无需认证">
                    </div>
                    <div class="form-group">
                        <label for="secondaryProxyPassword">密码（可选）</label>
                        <input type="password" id="secondaryProxyPassword" class="input" value="${config.secondaryProxyPassword || ''}" placeholder="留空表示无需认证">
                    </div>
                </div>
            </div>
        </div>
    `;
}

function saveInstanceConfig() {
    if (!currentConfigInstance) return;

    // Collect config data based on instance type
    const type = currentConfigInstance.type;
    let config = {};

    if (type === 'Collector' || type === 'AbCollector') {
        config = {
            storageMode: document.querySelector('input[name="storageMode"]:checked')?.value,
            socks5AuthEnabled: $('socks5AuthEnabled')?.checked,
            socks5Username: $('socks5Username')?.value,
            socks5Password: $('socks5Password')?.value,
            secondaryProxyEnabled: $('secondaryProxyEnabled')?.checked,
            proxyHost: $('proxyHost')?.value,
            proxyPort: parseInt($('proxyPort')?.value),
            minPacketSize: parseInt($('minPacketSize')?.value),
            maxPacketSize: parseInt($('maxPacketSize')?.value),
            filterDuplicates: $('filterDuplicates')?.checked,
            workerThreads: parseInt($('workerThreads')?.value),
            bufferSize: parseInt($('bufferSize')?.value),
            anticcEnabled: $('anticcEnabled')?.checked,
            anticcTimeWindow: parseInt($('anticcTimeWindow')?.value),
            anticcMaxConnections: parseInt($('anticcMaxConnections')?.value)
        };
    } else if (type === 'Heartbeat' || type === 'AbHeartbeat') {
        config = {
            replaceMode: document.querySelector('input[name="replaceMode"]:checked')?.value,
            dataSource: $('dataSource')?.value,
            enableCRC32: $('enableCRC32')?.checked
        };
    } else if (type === 'Socks5Pool') {
        config = {
            ccproxyEnabled: $('ccproxyEnabled')?.checked,
            ccproxyPort: parseInt($('ccproxyPort')?.value),
            ccproxyKey: $('ccproxyKey')?.value
        };
    } else if (type === 'SocksForward') {
        config = {
            enableSecondaryProxy: $('enableSecondaryProxy')?.checked,
            secondaryProxyHost: $('secondaryProxyHost')?.value,
            secondaryProxyPort: parseInt($('secondaryProxyPort')?.value),
            secondaryProxyUsername: $('secondaryProxyUsername')?.value,
            secondaryProxyPassword: $('secondaryProxyPassword')?.value
        };
    }

    postAction('instance_update_config', {
        id: currentConfigInstance.id,
        config: config
    });

    closeModal();
}

// ========== SOCKS5 Account Management (Two-Panel Layout) ==========
let selectedPoolId = null;
let selectedPoolName = null;
let currentPoolAccounts = [];
// true: 将“添加账号 / 导入 / API服务”放到弹窗里（右侧页面更短，减少滚动依赖）
// false: 使用旧版的右侧折叠区块
const USE_SOCKS5_ACCOUNT_TOOL_MODALS = true;
let showAddAccountForm = true;
let showCCProxyImport = false;
let showCCProxyAPI = false;
let editingAccount = null;
let cachedPools = [];

function buildPoolListRenderKey(pools) {
    if (!Array.isArray(pools) || pools.length === 0) {
        return `empty|${selectedPoolId || ''}`;
    }
    return `${selectedPoolId || ''}|` + pools.map(pool => [
        pool.id,
        pool.name,
        pool.accountCount || 0
    ].join(',')).join('|');
}

function buildAccountPanelRenderKey(poolId, poolName, accounts) {
    return [
        poolId || '',
        poolName || '',
        USE_SOCKS5_ACCOUNT_TOOL_MODALS ? 'modal' : 'inline',
        showAddAccountForm ? '1' : '0',
        showCCProxyImport ? '1' : '0',
        showCCProxyAPI ? '1' : '0',
        editingAccount ? (editingAccount.id || editingAccount.username || '') : ''
    ].join('#');
}

function buildAccountTableRowsKey(accounts) {
    return (accounts || []).map(acc => JSON.stringify(acc)).join('|');
}

function buildAccountRowKey(acc) {
    return JSON.stringify(acc);
}

function buildAccountRowHtml(acc) {
    const encodedAccount = JSON.stringify(acc).replace(/'/g, '&#39;');
    return `
        <tr>
            <td>${acc.username}</td>
            <td>${acc.expireTime ? acc.expireTime : '<span style="color:var(--text-muted);">永不过期</span>'}</td>
            <td>${acc.maxConnections === 0 ? '不限' : acc.maxConnections}</td>
            <td><span style="color:${acc.isEnabled !== false ? '#0f0' : '#f44'}; font-weight:500;">${acc.isEnabled !== false ? '启用' : '禁用'}</span></td>
            <td>${acc.createdAt || '-'}</td>
            <td>
                <button class="btn btn-ghost" style="padding:2px 8px; font-size:11px;" onclick='editAccount(${encodedAccount})'>编辑</button>
                <button class="btn btn-danger" style="padding:2px 8px; font-size:11px;" onclick="deleteAccount('${acc.username}')">删除</button>
            </td>
        </tr>
    `;
}

function syncAccountTableRows(accounts) {
    const tbody = $('accountTableBody');
    if (!tbody) return;

    const list = accounts || [];
    if (list.length === 0) {
        lastAccountTableRowKeys = [];
        tbody.innerHTML = '<tr><td colspan="6" style="text-align:center; padding:30px; color:var(--text-muted);">暂无账号，请添加</td></tr>';
        return;
    }

    const nextRowKeys = list.map(buildAccountRowKey);
    const rows = tbody.rows;

    if (rows.length === nextRowKeys.length && lastAccountTableRowKeys.length === nextRowKeys.length) {
        for (let i = 0; i < nextRowKeys.length; i++) {
            if (lastAccountTableRowKeys[i] !== nextRowKeys[i]) {
                rows[i].outerHTML = buildAccountRowHtml(list[i]);
            }
        }
    } else {
        tbody.innerHTML = list.map(buildAccountRowHtml).join('');
    }

    lastAccountTableRowKeys = nextRowKeys;
}

function updateCachedPoolAccountCount(poolId, count) {
    if (!Array.isArray(cachedPools)) {
        return;
    }
    for (const pool of cachedPools) {
        if (pool.id === poolId) {
            pool.accountCount = count;
            break;
        }
    }
}

function createPoolInline() {
    const input = $('newPoolNameInline');
    const name = input.value.trim();
    if (!name) {
        showToast('warning', '提示', '请输入账号库名称');
        return;
    }
    postAction('socks5_pool_create', { name });
    input.value = '';
}

function buildPoolItemKey(pool) {
    return [
        pool.id,
        pool.name,
        pool.accountCount || 0,
        pool.onlineDevicePolicy || '',
        selectedPoolId === pool.id ? 1 : 0
    ].join(',');
}

function buildPoolItemHtml(pool) {
    const isSelected = selectedPoolId === pool.id;
    const policyText = pool.onlineDevicePolicy === 'unlimited' ? '不限制' : '单设备策略';
    return `<div class="pool-item ${isSelected ? 'selected' : ''}"
             onclick="selectPool('${pool.id}')"
             oncontextmenu="showPoolContextMenu(event, '${pool.id}'); return false;">
            <div style="display:flex; flex-direction:column; gap:4px;">
                <span style="font-size:13px; color:var(--text);">${pool.name} (${pool.accountCount || 0}个账号)</span>
                <span style="font-size:11px; color:var(--text-muted);">${policyText}</span>
            </div>
        </div>`;
}

function syncPoolItems(pools) {
    const container = $('poolListPanel');
    if (!container) return;

    const nextItemKeys = (pools || []).map(buildPoolItemKey);
    const items = container.querySelectorAll(':scope > .pool-item');

    if (items.length === nextItemKeys.length && lastPoolItemKeys.length === nextItemKeys.length) {
        for (let i = 0; i < nextItemKeys.length; i++) {
            if (lastPoolItemKeys[i] !== nextItemKeys[i]) {
                items[i].outerHTML = buildPoolItemHtml(pools[i]);
            }
        }
    } else {
        container.innerHTML = (pools || []).map(buildPoolItemHtml).join('');
    }

    lastPoolItemKeys = nextItemKeys;
}

function renderSocks5Pools(pools) {
    cachedPools = pools || [];

    if (!isMainPageActive('accounts')) {
        return;
    }

    const container = $('poolListPanel');
    if (!container) return;

    const renderKey = `${poolsDataRevision}|${selectedPoolId || ''}|${cachedPools.length}`;
    if (renderKey === lastPoolListRenderKey) {
        return;
    }
    lastPoolListRenderKey = renderKey;

    if (!cachedPools || cachedPools.length === 0) {
        lastPoolItemKeys = [];
        container.innerHTML = '<div style="text-align:center; padding:20px; color:var(--text-muted); font-size:13px;">暂无账号库实例<br>请先创建一个账号库</div>';
        return;
    }

    syncPoolItems(cachedPools);
}

function selectPool(poolId) {
    const nextPool = Array.isArray(cachedPools) ? cachedPools.find(pool => pool.id === poolId) : null;
    const samePool = selectedPoolId === poolId;

    selectedPoolId = poolId;
    if (nextPool) {
        selectedPoolName = nextPool.name;
    }

    if (cachedPools.length > 0) {
        lastPoolListRenderKey = '';
        renderSocks5Pools(cachedPools);
    }

    if (samePool && Array.isArray(currentPoolAccounts) && currentPoolAccounts.length > 0) {
        return;
    }

    postAction('socks5_pool_get_accounts', { poolId });
}

function showPoolContextMenu(e, poolId) {
    e.preventDefault();
    if (confirm('确定要删除此账号库吗？')) {
        postAction('socks5_pool_delete', { poolId });
    }
}

function renderAccountPanel(poolId, poolName, accounts, onlineDevicePolicy = 'single_device_single_instance') {
    selectedPoolId = poolId;
    selectedPoolName = poolName;
    currentPoolAccounts = accounts || [];

    const content = $('accountPanelContent');
    if (!content) return;

    const accCount = accounts.length;
    const renderKey = buildAccountPanelRenderKey(poolId, poolName, currentPoolAccounts);
    const tableRowsKey = `${accountPanelDataRevision}|${poolId}|${accCount}`;
    const shouldRenderShell = renderKey !== lastAccountPanelRenderKey || content.dataset.accountPanelKey !== renderKey;

    if (shouldRenderShell) {
        const headerHtml = USE_SOCKS5_ACCOUNT_TOOL_MODALS
            ? `
        <div style="display:flex; align-items:flex-start; justify-content:space-between; gap:12px; margin-bottom:12px; flex-wrap:wrap;">
            <div style="min-width:240px;">
                <span style="color:#6ee7e7; font-size:16px; font-weight:600;">账号库: ${poolName}</span>
                <span style="color:var(--text-muted); font-size:13px; margin-left:8px;">(ID: ${poolId})</span>
            </div>
            <div style="display:flex; gap:8px; flex-wrap:wrap; justify-content:flex-end;">
                <button class="btn btn-primary btn-sm" onclick="openAddAccountModal()">添加账号</button>
                <button class="btn btn-ghost btn-sm" onclick="openCCProxyImportModal()">远程导入</button>
                <button class="btn btn-ghost btn-sm" onclick="openCCProxyApiModal()">远程管理</button>
            </div>
        </div>
        `
            : `
        <div style="margin-bottom:12px;">
            <span style="color:#6ee7e7; font-size:16px; font-weight:600;">账号库: ${poolName}</span>
            <span style="color:var(--text-muted); font-size:13px; margin-left:8px;">(ID: ${poolId})</span>
        </div>
        `;

        const addAccountInlineHtml = USE_SOCKS5_ACCOUNT_TOOL_MODALS ? '' : `
        <!-- 添加新账号 (折叠) -->
        <div style="border:1px solid var(--border); border-radius:var(--radius-sm); margin-bottom:16px; background:var(--bg-input);">
            <div style="padding:10px 14px; cursor:pointer; display:flex; justify-content:space-between; align-items:center;"
                 onclick="toggleAddAccountForm()">
                <span style="font-size:14px; font-weight:500;">添加新账号</span>
                <span id="addAccountFormArrow" style="color:var(--text-muted); font-size:14px;">${showAddAccountForm ? '▼' : '▶'}</span>
            </div>
            <div id="addAccountFormContainer" style="display:${showAddAccountForm ? 'block' : 'none'}; padding:0 14px 14px;">
                <div class="account-inline-row">
                    <span class="account-inline-label">用户名:</span>
                    <div class="account-inline-control">
                        <input type="text" id="newAccUsername" class="input account-inline-input">
                    </div>
                </div>
                <div class="account-inline-row">
                    <span class="account-inline-label">密码:</span>
                    <div class="account-inline-control">
                        <input type="password" id="newAccPassword" class="input account-inline-input">
                        <label class="account-inline-check">
                            <input type="checkbox" onchange="togglePasswordVisibility('newAccPassword')"> 显示
                        </label>
                    </div>
                </div>
                <div class="account-inline-row">
                    <span class="account-inline-label">到期时间:</span>
                    <div class="account-inline-control">
                        <input type="text" id="newAccExpire" class="input account-inline-input" value="${buildBeijingDateTimeString()}">
                        <label class="account-inline-check">
                            <input type="checkbox" id="newAccNeverExpire" onchange="toggleAccountNeverExpire('newAccExpire', 'newAccNeverExpire')"> 永不过期
                        </label>
                        <span class="account-inline-hint">(格式: YYYY-MM-DD HH:MM:SS)</span>
                    </div>
                </div>
                <div class="account-inline-row">
                    <span class="account-inline-label">最大连接:</span>
                    <div class="account-inline-control">
                        <input type="number" id="newAccMaxConn" class="input account-inline-input account-inline-input-sm" value="0" min="0">
                        <span class="account-inline-hint">(0=不限制)</span>
                    </div>
                </div>
                <button class="btn btn-primary" style="padding:6px 20px; font-size:13px;" onclick="submitAddAccount()">添加账号</button>
            </div>
        </div>
        `;

        const ccproxyInlineHtml = USE_SOCKS5_ACCOUNT_TOOL_MODALS ? '' : `
        <!-- 从CCProxy远程导入账号 (折叠) -->
        <div style="border:1px solid var(--border); border-radius:var(--radius-sm); margin-bottom:16px; background:var(--bg-input);">
            <div style="padding:10px 14px; cursor:pointer; display:flex; justify-content:space-between; align-items:center;"
                 onclick="toggleCCProxyImport()">
                <span style="font-size:14px; font-weight:500;">从CCProxy远程导入账号</span>
                <span id="ccproxyImportArrow" style="color:var(--text-muted); font-size:14px;">${showCCProxyImport ? '▼' : '▶'}</span>
            </div>
            <div id="ccproxyImportContainer" style="display:${showCCProxyImport ? 'block' : 'none'}; padding:0 14px 14px;">
                <div style="margin-bottom:8px; display:flex; align-items:center;">
                    <span style="min-width:80px; font-size:13px; color:var(--text-secondary);">远程地址:</span>
                    <input type="text" id="ccHost" class="input" value="127.0.0.1" style="width:150px; height:30px; font-size:13px;">
                </div>
                <div style="margin-bottom:8px; display:flex; align-items:center;">
                    <span style="min-width:80px; font-size:13px; color:var(--text-secondary);">端口:</span>
                    <input type="text" id="ccPort" class="input" value="90" style="width:80px; height:30px; font-size:13px;">
                </div>
                <div style="margin-bottom:8px; display:flex; align-items:center;">
                    <span style="min-width:80px; font-size:13px; color:var(--text-secondary);">用户名:</span>
                    <input type="text" id="ccUsername" class="input" value="admin" style="width:150px; height:30px; font-size:13px;">
                </div>
                <div style="margin-bottom:10px; display:flex; align-items:center;">
                    <span style="min-width:80px; font-size:13px; color:var(--text-secondary);">密码:</span>
                    <input type="password" id="ccPassword" class="input" value="admin" style="width:150px; height:30px; font-size:13px;">
                    <label style="margin-left:8px; font-size:12px; color:var(--text-secondary); cursor:pointer; display:flex; align-items:center; gap:4px;">
                        <input type="checkbox" onchange="togglePasswordVisibility('ccPassword')"> 显示
                    </label>
                </div>
                <button class="btn btn-primary" style="padding:6px 28px; font-size:13px;" onclick="submitCCProxyImport()">导入账号到当前库</button>
            </div>
        </div>

        <!-- CCProxy API兼容服务 (折叠) -->
        <div style="border:1px solid var(--border); border-radius:var(--radius-sm); background:var(--bg-input);">
            <div style="padding:10px 14px; cursor:pointer; display:flex; justify-content:space-between; align-items:center;"
                 onclick="toggleCCProxyAPI()">
                <span style="font-size:14px; font-weight:500;">远程管理</span>
                <span id="ccproxyAPIArrow" style="color:var(--text-muted); font-size:14px;">${showCCProxyAPI ? '▼' : '▶'}</span>
            </div>
            <div id="ccproxyAPIContainer" style="display:${showCCProxyAPI ? 'block' : 'none'}; padding:0 14px 14px;">
                <div style="margin-bottom:8px; display:flex; align-items:center;">
                    <span style="min-width:80px; font-size:13px; color:var(--text-secondary);">API端口:</span>
                    <input type="text" id="ccApiPort" class="input" value="90" style="width:80px; height:30px; font-size:13px;">
                </div>
                <div style="margin-bottom:8px; display:flex; align-items:center;">
                    <span style="min-width:80px; font-size:13px; color:var(--text-secondary);">用户名:</span>
                    <input type="text" id="ccApiUsername" class="input" value="admin" style="width:150px; height:30px; font-size:13px;">
                </div>
                <div style="margin-bottom:10px; display:flex; align-items:center;">
                    <span style="min-width:80px; font-size:13px; color:var(--text-secondary);">密码:</span>
                    <input type="password" id="ccApiPassword" class="input" value="admin" style="width:150px; height:30px; font-size:13px;">
                    <label style="margin-left:8px; font-size:12px; color:var(--text-secondary); cursor:pointer; display:flex; align-items:center; gap:4px;">
                        <input type="checkbox" onchange="togglePasswordVisibility('ccApiPassword')"> 显示
                    </label>
                </div>
                <div style="margin-bottom:10px; display:flex; align-items:center; gap:12px;">
                    <button class="btn btn-primary" style="padding:6px 20px; font-size:13px;" id="btnToggleApi" onclick="toggleCCProxyAPIService()">启动API服务</button>
                    <span id="apiStatusText" style="font-size:13px; color:#f44;">● 已停止</span>
                </div>
                <div style="font-size:12px; color:var(--text-muted); line-height:1.6;">
                    说明: 启动后可通过 http://IP:端口/account 访问当前账号库的账号列表，兼容CCProxy API格式。每个账号库实例可以独立启动自己的API服务。
                </div>
            </div>
        </div>
        `;

        lastAccountPanelRenderKey = renderKey;
        content.dataset.accountPanelKey = renderKey;
        content.innerHTML = `
        ${headerHtml}

        <div style="border:1px solid var(--border); border-radius:var(--radius-sm); margin-bottom:16px; background:var(--bg-input); padding:14px;">
            <div style="font-size:14px; font-weight:600; margin-bottom:10px;">账号库在线设备策略</div>
            <div class="form-group" style="margin-bottom:10px;">
                <label for="poolOnlineDevicePolicy">策略</label>
                <select id="poolOnlineDevicePolicy" class="input">
                    <option value="single_device_single_instance" ${onlineDevicePolicy === 'single_device_single_instance' ? 'selected' : ''}>单账号单设备单实例在线</option>
                    <option value="unlimited" ${onlineDevicePolicy === 'unlimited' ? 'selected' : ''}>不限制</option>
                </select>
            </div>
            <button class="btn btn-primary" style="padding:6px 20px; font-size:13px;" onclick="applyPoolOnlineDevicePolicy()">应用策略</button>
            <div style="margin-top:10px; font-size:12px; color:var(--text-muted); line-height:1.6;">
                说明：
                ${onlineDevicePolicy === 'single_device_single_instance'
                    ? '当前策略为同一账号在共享账号库下同一时刻只允许一个实例、一个IP在线。'
                    : '当前策略为不限制，账号库不再拦截跨实例/跨IP的重复在线。'}
            </div>
        </div>

        ${addAccountInlineHtml}

        <!-- 账号列表 -->
        <div style="margin-bottom:16px;">
            <div style="margin-bottom:8px;">
                <span style="color:#ff0; font-size:14px; font-weight:500;">账号列表:</span>
                <span id="accountCountText" style="color:#ff0; font-size:13px; margin-left:8px;">共 ${accCount} 个账号</span>
            </div>
            <div style="overflow-x:auto; overflow-y:auto; max-height:300px; border:1px solid var(--border); border-radius:var(--radius-sm);">
                <table class="data-table" style="font-size:12px;">
                    <thead>
                        <tr>
                            <th style="width:120px;">用户名</th>
                            <th style="width:150px;">到期时间</th>
                            <th style="width:70px;">最大连接</th>
                            <th style="width:60px;">状态</th>
                            <th style="width:150px;">创建时间</th>
                            <th style="width:120px;">操作</th>
                        </tr>
                    </thead>
                    <tbody id="accountTableBody">
                        ${accCount === 0 ? '<tr><td colspan="6" style="text-align:center; padding:30px; color:var(--text-muted);">暂无账号，请添加</td></tr>' :
                          accounts.map(acc => `
                            <tr>
                                <td>${acc.username}</td>
                                <td>${acc.expireTime ? acc.expireTime : '<span style="color:var(--text-muted);">永不过期</span>'}</td>
                                <td>${acc.maxConnections === 0 ? '不限' : acc.maxConnections}</td>
                                <td><span style="color:${acc.isEnabled !== false ? '#0f0' : '#f44'}; font-weight:500;">${acc.isEnabled !== false ? '启用' : '禁用'}</span></td>
                                <td>${acc.createdAt || '-'}</td>
                                <td>
                                    <button class="btn btn-ghost" style="padding:2px 8px; font-size:11px;" onclick='editAccount(${JSON.stringify(acc).replace(/'/g, "&#39;")})'>编辑</button>
                                    <button class="btn btn-danger" style="padding:2px 8px; font-size:11px;" onclick="deleteAccount('${acc.username}')">删除</button>
                                </td>
                            </tr>
                          `).join('')}
                    </tbody>
                </table>
            </div>
        </div>

        <!-- 绑定提示 -->
        <div style="margin-bottom:16px; padding:10px 14px; font-size:12px; color:var(--text-muted); line-height:1.6;">
            提示: 要将此账号库绑定到采集/伪心跳实例，请在对应实例的 [SOCKS5账号] 子菜单中选择绑定。
        </div>

        ${ccproxyInlineHtml}
    `;
        lastAccountTableRowsKey = '';
        lastAccountTableRowKeys = [];
        if (!USE_SOCKS5_ACCOUNT_TOOL_MODALS) {
            postAction('socks5_api_status', { poolId });
        }
    }

    const countText = $('accountCountText');
    if (countText) {
        countText.textContent = `共 ${accCount} 个账号`;
    }

    if (tableRowsKey !== lastAccountTableRowsKey) {
        lastAccountTableRowsKey = tableRowsKey;
        syncAccountTableRows(currentPoolAccounts);
    }
}

function openAddAccountModal() {
    if (!selectedPoolId) {
        showToast('warning', '提示', '请先选择一个账号库');
        return;
    }

    showModal('添加新账号', `
        <div class="form-group">
            <label for="newAccUsername">用户名</label>
            <input type="text" id="newAccUsername" class="input" placeholder="请输入用户名">
        </div>
        <div class="form-group">
            <label for="newAccPassword">密码</label>
            <div style="display:flex; align-items:center; gap:8px;">
                <input type="password" id="newAccPassword" class="input" placeholder="请输入密码">
                <label style="font-size:12px; color:var(--text-secondary); cursor:pointer; display:flex; align-items:center; gap:4px; white-space:nowrap;">
                    <input type="checkbox" onchange="togglePasswordVisibility('newAccPassword')"> 显示
                </label>
            </div>
        </div>
        <div class="form-group">
            <label for="newAccExpire">到期时间</label>
            <input type="text" id="newAccExpire" class="input" value="${buildBeijingDateTimeString()}" placeholder="YYYY-MM-DD HH:MM:SS">
            <label class="account-inline-check" style="margin-top:8px;">
                <input type="checkbox" id="newAccNeverExpire" onchange="toggleAccountNeverExpire('newAccExpire', 'newAccNeverExpire')"> 永不过期
            </label>
            <div style="margin-top:6px; font-size:12px; color:var(--text-muted); line-height:1.5;">格式: YYYY-MM-DD HH:MM:SS</div>
        </div>
        <div class="form-group">
            <label for="newAccMaxConn">最大连接</label>
            <input type="number" id="newAccMaxConn" class="input" value="0" min="0">
            <div style="margin-top:6px; font-size:12px; color:var(--text-muted); line-height:1.5;">0=不限制</div>
        </div>
    `, `
        <button class="btn btn-ghost" onclick="closeModal()">取消</button>
        <button class="btn btn-primary" onclick="if (submitAddAccount()) closeModal()">添加</button>
    `);

    setTimeout(() => $('newAccUsername')?.focus(), 0);
}

function openCCProxyImportModal() {
    if (!selectedPoolId) {
        showToast('warning', '提示', '请先选择一个账号库');
        return;
    }

    showModal('从CCProxy远程导入账号', `
        <div class="form-group">
            <label for="ccHost">远程地址</label>
            <input type="text" id="ccHost" class="input" value="127.0.0.1" placeholder="127.0.0.1">
        </div>
        <div class="form-group">
            <label for="ccPort">端口</label>
            <input type="text" id="ccPort" class="input" value="90" placeholder="90">
        </div>
        <div class="form-group">
            <label for="ccUsername">用户名</label>
            <input type="text" id="ccUsername" class="input" value="admin" placeholder="admin">
        </div>
        <div class="form-group">
            <label for="ccPassword">密码</label>
            <div style="display:flex; align-items:center; gap:8px;">
                <input type="password" id="ccPassword" class="input" value="admin" placeholder="admin">
                <label style="font-size:12px; color:var(--text-secondary); cursor:pointer; display:flex; align-items:center; gap:4px; white-space:nowrap;">
                    <input type="checkbox" onchange="togglePasswordVisibility('ccPassword')"> 显示
                </label>
            </div>
        </div>
        <div style="font-size:12px; color:var(--text-muted); line-height:1.6;">
            说明: 该功能会通过 CCProxy 管理端拉取账号列表并导入到当前账号库。
        </div>
    `, `
        <button class="btn btn-ghost" onclick="closeModal()">取消</button>
        <button class="btn btn-primary" onclick="if (submitCCProxyImport()) closeModal()">导入</button>
    `);
}

function openCCProxyApiModal() {
    if (!selectedPoolId) {
        showToast('warning', '提示', '请先选择一个账号库');
        return;
    }

    showModal('远程管理', `
        <div class="form-group">
            <label for="ccApiPort">API端口</label>
            <input type="text" id="ccApiPort" class="input" value="90" placeholder="90">
        </div>
        <div class="form-group">
            <label for="ccApiUsername">用户名</label>
            <input type="text" id="ccApiUsername" class="input" value="admin" placeholder="admin">
        </div>
        <div class="form-group">
            <label for="ccApiPassword">密码</label>
            <div style="display:flex; align-items:center; gap:8px;">
                <input type="password" id="ccApiPassword" class="input" value="admin" placeholder="admin">
                <label style="font-size:12px; color:var(--text-secondary); cursor:pointer; display:flex; align-items:center; gap:4px; white-space:nowrap;">
                    <input type="checkbox" onchange="togglePasswordVisibility('ccApiPassword')"> 显示
                </label>
            </div>
        </div>
        <div style="margin-bottom:10px; display:flex; align-items:center; gap:12px;">
            <button class="btn btn-primary" id="btnToggleApi" onclick="toggleCCProxyAPIService()">启动API服务</button>
            <span id="apiStatusText" style="font-size:13px; color:#f44;">● 已停止</span>
        </div>
        <div style="font-size:12px; color:var(--text-muted); line-height:1.6;">
            说明: 启动后可通过 http://IP:端口/account 访问当前账号库的账号列表，兼容CCProxy API格式。每个账号库实例可以独立启动自己的API服务。
        </div>
    `, `
        <button class="btn btn-ghost" onclick="closeModal()">关闭</button>
    `);

    postAction('socks5_api_status', { poolId: selectedPoolId });
}

function applyPoolOnlineDevicePolicy() {
    const select = $('poolOnlineDevicePolicy');
    if (!select || !selectedPoolId) return;
    postAction('socks5_pool_set_online_policy', {
        poolId: selectedPoolId,
        policy: select.value
    });
}

function setSectionExpanded(containerId, arrowId, expanded) {
    const container = $(containerId);
    const arrow = $(arrowId);
    if (container) {
        container.style.display = expanded ? 'block' : 'none';
    }
    if (arrow) {
        arrow.textContent = expanded ? '▼' : '▶';
    }
}

function toggleAddAccountForm() {
    showAddAccountForm = !showAddAccountForm;
    setSectionExpanded('addAccountFormContainer', 'addAccountFormArrow', showAddAccountForm);
}

function toggleCCProxyImport() {
    showCCProxyImport = !showCCProxyImport;
    setSectionExpanded('ccproxyImportContainer', 'ccproxyImportArrow', showCCProxyImport);
}

function toggleCCProxyAPI() {
    showCCProxyAPI = !showCCProxyAPI;
    setSectionExpanded('ccproxyAPIContainer', 'ccproxyAPIArrow', showCCProxyAPI);
}

function toggleCCProxyAPIService() {
    const port = $('ccApiPort')?.value.trim() || '90';
    const username = $('ccApiUsername')?.value.trim() || 'admin';
    const password = $('ccApiPassword')?.value.trim() || 'admin';

    const btn = $('btnToggleApi');
    const statusText = $('apiStatusText');

    if (btn && btn.textContent.includes('停止')) {
        // Currently running, stop it
        postAction('socks5_api_stop', { poolId: selectedPoolId });
    } else {
        // Currently stopped, start it
        const portNum = parseInt(port);
        if (isNaN(portNum) || portNum <= 0 || portNum > 65535) {
            showToast('error', '错误', '端口号无效！');
            return;
        }
        postAction('socks5_api_start', {
            poolId: selectedPoolId,
            port: portNum,
            username: username,
            password: password
        });
    }
}

function togglePasswordVisibility(inputId) {
    const input = $(inputId);
    if (input) input.type = input.type === 'password' ? 'text' : 'password';
}

function buildBeijingDateTimeString() {
    const formatter = new Intl.DateTimeFormat('zh-CN', {
        timeZone: 'Asia/Shanghai',
        year: 'numeric',
        month: '2-digit',
        day: '2-digit',
        hour: '2-digit',
        minute: '2-digit',
        second: '2-digit',
        hour12: false
    });
    const parts = formatter.formatToParts(new Date());
    const valueByType = {};
    for (const part of parts) {
        if (part.type !== 'literal') {
            valueByType[part.type] = part.value;
        }
    }
    return `${valueByType.year || '0000'}-${valueByType.month || '00'}-${valueByType.day || '00'} ` +
        `${valueByType.hour || '00'}:${valueByType.minute || '00'}:${valueByType.second || '00'}`;
}

function toggleAccountNeverExpire(expireInputId, checkboxId) {
    const expireInput = $(expireInputId);
    const checkbox = $(checkboxId);
    if (!expireInput || !checkbox) return;

    if (checkbox.checked) {
        expireInput.dataset.previousValue = expireInput.value || '';
        expireInput.value = '';
        expireInput.disabled = true;
        expireInput.placeholder = '永不过期';
    } else {
        expireInput.disabled = false;
        expireInput.placeholder = 'YYYY-MM-DD HH:MM:SS';
        const previousValue = expireInput.dataset.previousValue || '';
        expireInput.value = previousValue || buildBeijingDateTimeString();
    }
}

function submitAddAccount() {
    if (!selectedPoolId) {
        showToast('warning', '提示', '请先选择一个账号库');
        return false;
    }

    const username = $('newAccUsername')?.value.trim();
    const password = $('newAccPassword')?.value.trim();
    const expireTime = $('newAccNeverExpire')?.checked ? '' : ($('newAccExpire')?.value.trim() || '');
    const maxConnections = parseInt($('newAccMaxConn')?.value) || 0;

    if (!username || !password) {
        showToast('warning', '提示', '请输入用户名和密码');
        return false;
    }

    postAction('socks5_account_add', {
        poolId: selectedPoolId,
        account: { username, password, expireTime, maxConnections }
    });
    return true;
}

function editAccount(acc) {
    editingAccount = acc;
    showModal('编辑账号', `
        <div class="form-group">
            <label>用户名（只读）</label>
            <input type="text" class="input" value="${acc.username}" readonly style="background:var(--bg); cursor:not-allowed;">
        </div>
        <div class="form-group">
            <label for="editAccPassword">密码</label>
            <div style="display:flex; align-items:center; gap:8px;">
                <input type="password" id="editAccPassword" class="input" value="${acc.password}">
                <label style="font-size:12px; color:var(--text-secondary); cursor:pointer; display:flex; align-items:center; gap:4px; white-space:nowrap;">
                    <input type="checkbox" onchange="togglePasswordVisibility('editAccPassword')"> 显示
                </label>
            </div>
        </div>
        <div class="form-group">
            <label for="editAccExpire">到期时间</label>
            <input type="text" id="editAccExpire" class="input" value="${acc.expireTime || ''}" placeholder="${acc.expireTime ? 'YYYY-MM-DD HH:MM:SS' : '永不过期'}" ${acc.expireTime ? '' : 'disabled'}>
            <label class="account-inline-check" style="margin-top:8px;">
                <input type="checkbox" id="editAccNeverExpire" onchange="toggleAccountNeverExpire('editAccExpire', 'editAccNeverExpire')" ${acc.expireTime ? '' : 'checked'}> 永不过期
            </label>
        </div>
        <div class="form-group">
            <label for="editAccMaxConn">最大连接</label>
            <input type="number" id="editAccMaxConn" class="input" value="${acc.maxConnections || 0}">
        </div>
        <div class="form-group">
            <label><input type="checkbox" id="editAccEnabled" ${acc.isEnabled !== false ? 'checked' : ''}> 启用账号</label>
        </div>
    `, `
        <button class="btn btn-ghost" onclick="closeModal()">取消</button>
        <button class="btn btn-primary" onclick="submitEditAccount()">保存</button>
    `);
}

function submitEditAccount() {
    if (!editingAccount) return;

    const password = $('editAccPassword')?.value.trim();
    const expireTime = $('editAccNeverExpire')?.checked ? '' : ($('editAccExpire')?.value.trim() || '');
    const maxConnections = parseInt($('editAccMaxConn')?.value) || 0;
    const isEnabled = $('editAccEnabled')?.checked;

    postAction('socks5_account_update', {
        poolId: selectedPoolId,
        username: editingAccount.username,
        account: { password, expireTime, maxConnections, isEnabled }
    });
    closeModal();
    editingAccount = null;
}

function deleteAccount(username) {
    if (confirm('确定要删除此账号吗？')) {
        postAction('socks5_account_delete', { poolId: selectedPoolId, username });
    }
}

function submitCCProxyImport() {
    if (!selectedPoolId) {
        showToast('warning', '提示', '请先选择一个账号库');
        return false;
    }

    const host = $('ccHost')?.value.trim();
    const port = $('ccPort')?.value.trim();
    const username = $('ccUsername')?.value.trim();
    const password = $('ccPassword')?.value.trim();

    if (!host || !port || !username || !password) {
        showToast('warning', '提示', '请填写完整的CCProxy连接信息');
        return false;
    }

    postAction('socks5_ccproxy_import', {
        poolId: selectedPoolId,
        host, port, username, password
    });
    return true;
}

// ========== WPE Filter Management ==========
let wpeFilters = [];
let wpeSelectedIds = new Set();
let currentEditFilterId = null;
let hexGridSearchOffset = 500; // fixed grid index, 0=pos(-500), 500=pos(0)
let hexGridModifyOffset = 500;
let hexGridEnableOffset = 500;
let hexGridDisableOffset = 500;
let disconnectHexGridOffset = 500;
const WPE_GRID_COLS = 24; // 搜索/修改每行显示24列
const WPE_ADV_GRID_COLS = 16; // 高级触发每行显示16列
const WPE_DEFAULT_GRID_MIN_POS = -500;
const WPE_DEFAULT_GRID_MAX_POS = 500;
const WPE_GRID_MIN_POS = -500;
const WPE_GRID_MAX_POS = 500;
const WPE_GRID_TOTAL = 1001; // -500 to +500
const WPE_MODIFY_RANGE_MODE_STANDARD = 'standard';
const WPE_MODIFY_RANGE_MODE_CUSTOM = 'custom';
let wpeModifyRangeMode = WPE_MODIFY_RANGE_MODE_STANDARD;
let wpeModifyRangeMin = WPE_DEFAULT_GRID_MIN_POS;
let wpeModifyRangeMax = WPE_DEFAULT_GRID_MAX_POS;

const WPE_MODE_NAMES = { 'normal': '普通', 'advanced': '高级' };
const WPE_ACTION_NAMES = {
    'replace': '替换', 'intercept': '拦截',
    'display': '不修改(显示)', 'nodisplay': '不修改(不显示)',
    'none': '无', 'change': '换包'
};
const WPE_PRIORITY_NAMES = { 'before': '滤镜优先', 'after': '心跳优先' };

function getDirectionText(dir) {
    if (!dir) return '双向';
    if (dir.request && dir.response) return '双向';
    if (dir.request) return '发送';
    if (dir.response) return '接收';
    return '无';
}

// Parse pattern string "pos|hex,pos|hex,..." into Map<position, hexString>
function parsePatternString(pattern) {
    const map = new Map();
    if (!pattern) return map;
    pattern.split(',').forEach(entry => {
        const parts = entry.trim().split('|');
        if (parts.length === 2) {
            const pos = parseInt(parts[0]);
            const val = parts[1].trim().toUpperCase();
            if (!isNaN(pos)) map.set(pos, val);
        }
    });
    return map;
}

function serializePatternMap(map) {
    const entries = [];
    const sortedKeys = [...map.keys()].sort((a, b) => a - b);
    for (const pos of sortedKeys) {
        entries.push(`${pos}|${map.get(pos)}`);
    }
    return entries.join(',');
}

function normalizeModifyRangeMode(mode) {
    return mode === WPE_MODIFY_RANGE_MODE_CUSTOM
        ? WPE_MODIFY_RANGE_MODE_CUSTOM
        : WPE_MODIFY_RANGE_MODE_STANDARD;
}

function normalizeModifyRangeNumber(value, fallback) {
    const parsed = parseInt(value, 10);
    return Number.isFinite(parsed) ? parsed : fallback;
}

function resolveModifyRangeConfig(filter) {
    let mode = normalizeModifyRangeMode(filter?.modifyRangeMode);
    let min = normalizeModifyRangeNumber(filter?.modifyRangeMin, WPE_DEFAULT_GRID_MIN_POS);
    let max = normalizeModifyRangeNumber(filter?.modifyRangeMax, WPE_DEFAULT_GRID_MAX_POS);

    if (mode !== WPE_MODIFY_RANGE_MODE_CUSTOM) {
        min = WPE_DEFAULT_GRID_MIN_POS;
        max = WPE_DEFAULT_GRID_MAX_POS;
    }

    if (min >= 0) min = WPE_DEFAULT_GRID_MIN_POS;
    if (max <= 0) max = WPE_DEFAULT_GRID_MAX_POS;
    if (min >= max) {
        mode = WPE_MODIFY_RANGE_MODE_STANDARD;
        min = WPE_DEFAULT_GRID_MIN_POS;
        max = WPE_DEFAULT_GRID_MAX_POS;
    }

    return { mode, min, max };
}

function getGridMinPos(gridType) {
    return gridType === 'modify' ? wpeModifyRangeMin : WPE_GRID_MIN_POS;
}

function getGridMaxPos(gridType) {
    return gridType === 'modify' ? wpeModifyRangeMax : WPE_GRID_MAX_POS;
}

function getGridTotal(gridType) {
    return getGridMaxPos(gridType) - getGridMinPos(gridType) + 1;
}

function getGridMaxOffset(gridType) {
    return Math.max(0, getGridTotal(gridType) - getGridCols(gridType));
}

function clampGridOffset(gridType, offset) {
    return Math.max(0, Math.min(getGridMaxOffset(gridType), offset));
}

function getGridDefaultOffset(gridType) {
    return clampGridOffset(gridType, -getGridMinPos(gridType));
}

function clipPatternMapToRange(patternMap, minPos, maxPos) {
    const nextMap = new Map();
    let removedCount = 0;
    for (const [pos, val] of patternMap.entries()) {
        if (pos >= minPos && pos <= maxPos) {
            nextMap.set(pos, val);
        } else {
            removedCount++;
        }
    }
    return { map: nextMap, removedCount };
}

function trimModifyPatternToCurrentRange(notify) {
    const textInput = $('wpeModifyPattern');
    if (!textInput) return 0;

    const { map, removedCount } = clipPatternMapToRange(
        parsePatternString(textInput.value || ''),
        wpeModifyRangeMin,
        wpeModifyRangeMax
    );
    const nextText = serializePatternMap(map);
    if (textInput.value !== nextText) {
        textInput.value = nextText;
    }

    if (notify && removedCount > 0) {
        showToast('info', '提示', `已裁剪 ${removedCount} 个超出当前修改范围的字节`);
    }

    return removedCount;
}

function updateModifyRangeUi() {
    const modeSelect = $('wpeModifyRangeMode');
    const customPanel = $('wpeModifyRangeCustomPanel');
    const negativeInput = $('wpeModifyRangeMin');
    const positiveInput = $('wpeModifyRangeMax');
    const hint = $('modifyRangeModeHint');

    if (modeSelect) modeSelect.value = wpeModifyRangeMode;
    if (customPanel) {
        customPanel.style.display = wpeModifyRangeMode === WPE_MODIFY_RANGE_MODE_CUSTOM ? 'grid' : 'none';
    }
    if (negativeInput) negativeInput.value = String(wpeModifyRangeMin);
    if (positiveInput) positiveInput.value = String(wpeModifyRangeMax);
    if (hint) {
        hint.textContent = `当前范围: ${wpeModifyRangeMin} ~ ${wpeModifyRangeMax}`;
    }
}

function applyModifyRangeSettings(mode, minPos, maxPos, options = {}) {
    const notifyTrim = !!options.notifyTrim;
    const resetToZero = options.resetToZero !== false;

    wpeModifyRangeMode = normalizeModifyRangeMode(mode);
    if (wpeModifyRangeMode !== WPE_MODIFY_RANGE_MODE_CUSTOM) {
        wpeModifyRangeMin = WPE_DEFAULT_GRID_MIN_POS;
        wpeModifyRangeMax = WPE_DEFAULT_GRID_MAX_POS;
    } else {
        wpeModifyRangeMin = minPos;
        wpeModifyRangeMax = maxPos;
    }

    trimModifyPatternToCurrentRange(notifyTrim);
    updateModifyRangeUi();

    if (resetToZero) {
        hexGridModifyOffset = getGridDefaultOffset('modify');
        const jumpInput = $('modifyGridJump');
        if (jumpInput) jumpInput.value = '0';
    } else {
        hexGridModifyOffset = clampGridOffset('modify', hexGridModifyOffset);
    }

    syncGridFromText('modify');
}

function onModifyRangeModeChange() {
    const mode = normalizeModifyRangeMode($('wpeModifyRangeMode')?.value);
    if (mode === WPE_MODIFY_RANGE_MODE_CUSTOM) {
        const minPos = normalizeModifyRangeNumber($('wpeModifyRangeMin')?.value, wpeModifyRangeMin);
        const maxPos = normalizeModifyRangeNumber($('wpeModifyRangeMax')?.value, wpeModifyRangeMax);
        if (minPos >= 0 || maxPos <= 0 || minPos >= maxPos) {
            showToast('warning', '提示', '自定义范围要求负数小于0，正数大于0');
            updateModifyRangeUi();
            return;
        }
        applyModifyRangeSettings(mode, minPos, maxPos, { notifyTrim: true, resetToZero: true });
    } else {
        applyModifyRangeSettings(mode, WPE_DEFAULT_GRID_MIN_POS, WPE_DEFAULT_GRID_MAX_POS, { notifyTrim: true, resetToZero: true });
    }
}

function onModifyRangeBoundsChange() {
    if (wpeModifyRangeMode !== WPE_MODIFY_RANGE_MODE_CUSTOM) {
        updateModifyRangeUi();
        return;
    }

    const minPos = normalizeModifyRangeNumber($('wpeModifyRangeMin')?.value, wpeModifyRangeMin);
    const maxPos = normalizeModifyRangeNumber($('wpeModifyRangeMax')?.value, wpeModifyRangeMax);

    if (minPos >= 0) {
        showToast('warning', '提示', '负向范围必须小于 0');
        updateModifyRangeUi();
        return;
    }
    if (maxPos <= 0) {
        showToast('warning', '提示', '正向范围必须大于 0');
        updateModifyRangeUi();
        return;
    }
    if (minPos >= maxPos) {
        showToast('warning', '提示', '负向范围必须小于正向范围');
        updateModifyRangeUi();
        return;
    }

    applyModifyRangeSettings(WPE_MODIFY_RANGE_MODE_CUSTOM, minPos, maxPos, { notifyTrim: true, resetToZero: true });
}

function syncModifyRangeStateBeforeSave() {
    const mode = normalizeModifyRangeMode($('wpeModifyRangeMode')?.value);
    if (mode !== WPE_MODIFY_RANGE_MODE_CUSTOM) {
        applyModifyRangeSettings(WPE_MODIFY_RANGE_MODE_STANDARD, WPE_DEFAULT_GRID_MIN_POS, WPE_DEFAULT_GRID_MAX_POS, {
            notifyTrim: false,
            resetToZero: false
        });
        return true;
    }

    const minPos = normalizeModifyRangeNumber($('wpeModifyRangeMin')?.value, wpeModifyRangeMin);
    const maxPos = normalizeModifyRangeNumber($('wpeModifyRangeMax')?.value, wpeModifyRangeMax);

    if (minPos >= 0) {
        showToast('warning', '提示', '负向范围必须小于 0');
        return false;
    }
    if (maxPos <= 0) {
        showToast('warning', '提示', '正向范围必须大于 0');
        return false;
    }
    if (minPos >= maxPos) {
        showToast('warning', '提示', '负向范围必须小于正向范围');
        return false;
    }

    applyModifyRangeSettings(WPE_MODIFY_RANGE_MODE_CUSTOM, minPos, maxPos, {
        notifyTrim: false,
        resetToZero: false
    });
    return true;
}

function renderWpeTargetInstances(selectedIds) {
    const instances = window.cachedInstances || [];
    const socksForward = instances.filter(i => i.type === 'SocksForward');
    if (socksForward.length === 0) {
        lastWpeTargetInstancesKey = 'empty';
        lastWpeTargetInstancesHtml = '<div style="padding:8px; text-align:center; color:var(--text-muted); font-size:12px;">暂无Socks转发实例</div>';
        return lastWpeTargetInstancesHtml;
    }

    const selectedKey = Array.isArray(selectedIds) ? selectedIds.slice().sort().join(',') : '';
    const instancesKey = socksForward.map(inst => [inst.id, inst.name].join(',')).join('|');
    const cacheKey = instancesKey + '#' + selectedKey;
    if (cacheKey === lastWpeTargetInstancesKey) {
        return lastWpeTargetInstancesHtml;
    }

    lastWpeTargetInstancesKey = cacheKey;
    lastWpeTargetInstancesHtml = socksForward.map(inst => {
        const checked = selectedIds.includes(inst.id) ? 'checked' : '';
        return `<label style="display:flex; align-items:center; gap:6px; padding:4px 8px; font-size:13px; cursor:pointer;">
            <input type="checkbox" class="wpeTargetInst" value="${inst.id}" ${checked}>
            <span>${inst.id} (${inst.name})</span>
        </label>`;
    }).join('');
    return lastWpeTargetInstancesHtml;
}
function wpeToggleGlobalTarget() {
    const global = $('wpeGlobal')?.checked;
    const list = $('wpeTargetInstanceList');
    if (list) list.style.display = global ? 'none' : 'block';
}

function renderWPEHexGrid(gridType, gridOffset, patternMap, cols) {
    cols = cols || WPE_GRID_COLS;
    const gridId = {
        search:'searchHexGridView',
        modify:'modifyHexGridView',
        enable:'enableHexGridView',
        disable:'disableHexGridView',
        disconnect:'disconnectHexGridView'
    }[gridType];
    const container = $(gridId);
    if (!container) return;

    const minPos = getGridMinPos(gridType);
    const total = getGridTotal(gridType);
    const safeOffset = clampGridOffset(gridType, gridOffset);
    const startPos = safeOffset + minPos;
    const endPos = Math.min(getGridMaxPos(gridType), startPos + cols - 1);

    // 位置编号行
    let html = '<div class="hex-grid-compact">';
    html += '<div class="hex-row">';
    for (let col = 0; col < cols; col++) {
        const idx = safeOffset + col;
        if (idx >= total) break;
        const pos = idx + minPos;
        html += `<div class="hex-pos-label">${pos}</div>`;
    }
    html += '</div>';

    // 十六进制输入框行
    html += '<div class="hex-row">';
    for (let col = 0; col < cols; col++) {
        const idx = safeOffset + col;
        if (idx >= total) break;
        const pos = idx + minPos;
        const val = patternMap.get(pos) || '';
        const cls = val === '??' ? 'hex-cell wildcard' : (val ? 'hex-cell filled' : 'hex-cell');
        html += `<input type="text" class="${cls}" maxlength="2"
            data-pos="${pos}" data-grid="${gridType}"
            value="${val}" placeholder=""
            oninput="onHexCellInput(this)"
            onpaste="onHexCellPaste(event, this)"
            onfocus="onHexCellFocus(this)"
            onmousedown="onHexCellFocus(this)"
            onchange="onHexCellChange(this)"
            onkeydown="onHexCellKeydown(event, this)">`;
    }
    html += '</div></div>';
    container.innerHTML = html;

    // 更新范围显示
    const rangeId = {
        search:'searchGridRange',
        modify:'modifyGridRange',
        enable:'enableGridRange',
        disable:'disableGridRange',
        disconnect:'disconnectGridRange'
    }[gridType];
    const rangeEl = $(rangeId);
    if (rangeEl) rangeEl.textContent = `[${startPos} ~ ${endPos}]`;
}

function onHexCellChange(cell) {
    const val = cell.value.trim().toUpperCase();
    const allowRandom = cell.dataset.grid === 'modify';
    if (val && val !== '??' && !(allowRandom && val === 'RR') && !/^[0-9A-F?]{1,2}$/.test(val)) {
        cell.value = '';
        return;
    }
    cell.value = val;
    // 更新样式
    cell.className = val === '??' ? 'hex-cell wildcard' : (allowRandom && val === 'RR') ? 'hex-cell random' : (val ? 'hex-cell filled' : 'hex-cell');
    if (cell.dataset.grid) {
        syncTextFromGrid(cell.dataset.grid);
    }
}

function onHexCellFocus(cell) {
    if (cell && cell.dataset && cell.dataset.grid === 'modify') {
        if (lastActiveModifyHexCell && document.body.contains(lastActiveModifyHexCell)) {
            lastActiveModifyHexCell.classList.remove('hex-cell-active');
        }
        lastActiveModifyHexCell = cell;
        lastActiveModifyHexCell.classList.add('hex-cell-active');
    }
}

function onHexCellInput(cell) {
    const allowRandom = cell.dataset.grid === 'modify';
    let val = cell.value.trim().toUpperCase().replace(allowRandom ? /[^0-9A-F?R]/g : /[^0-9A-F?]/g, '');
    if (val.length > 2) {
        val = val.slice(0, 2);
    }
    if (allowRandom && val === 'R') {
        // 允许用户继续输入第二个 R
    } else if (allowRandom && val.length === 2 && val !== 'RR' && !/^[0-9A-F?]{2}$/.test(val)) {
        val = val.replace(/R/g, '');
    }
    cell.value = val;
    cell.className = val === '??' ? 'hex-cell wildcard' : (allowRandom && val === 'RR') ? 'hex-cell random' : (val ? 'hex-cell filled' : 'hex-cell');

    if (cell.dataset.grid) {
        syncTextFromGrid(cell.dataset.grid);
    }

    if (val.length === 2) {
        const next = cell.nextElementSibling;
        if (next && next.classList.contains('hex-cell')) {
            next.focus();
            next.select();
        }
    }
}

function markModifyCellRandom() {
    const active = (lastActiveModifyHexCell && document.body.contains(lastActiveModifyHexCell))
        ? lastActiveModifyHexCell
        : document.activeElement;
    if (!active || !active.classList || !active.classList.contains('hex-cell') || active.dataset.grid !== 'modify') {
        showToast('warning', '提示', '请先选中修改内容中的一个字节格');
        return;
    }
    active.value = 'RR';
    onHexCellChange(active);
    lastActiveModifyHexCell = active;
    const next = active.nextElementSibling;
    if (next && next.classList.contains('hex-cell')) {
        next.focus();
        next.select();
    }
}

function parseClipboardHexBytes(text) {
    const hexBytes = [];
    const cleaned = (text || '').trim();
    if (!cleaned) return hexBytes;

    if (/^[0-9a-fA-F\s]+$/.test(cleaned) && cleaned.includes(' ')) {
        cleaned.split(/\s+/).forEach(h => {
            if (h.length === 2) hexBytes.push(h.toUpperCase());
        });
    } else if (/^[0-9a-fA-F,]+$/.test(cleaned) && cleaned.includes(',')) {
        cleaned.split(',').forEach(h => {
            h = h.trim();
            if (h.length === 2) hexBytes.push(h.toUpperCase());
        });
    } else {
        const compact = cleaned.replace(/[^0-9a-fA-F]/g, '');
        if ((compact.length % 2) === 0) {
            for (let i = 0; i < compact.length; i += 2) {
                hexBytes.push(compact.substr(i, 2).toUpperCase());
            }
        }
    }

    return hexBytes;
}

function setHexBytesToGrid(gridType, startPos, hexBytes) {
    if (!Array.isArray(hexBytes) || hexBytes.length === 0) return 0;

    const textInput = getGridPatternInput(gridType);
    const patternMap = parsePatternString(textInput?.value || '');
    for (let i = 0; i < hexBytes.length; i++) {
        patternMap.set(startPos + i, hexBytes[i]);
    }

    const clipped = clipPatternMapToRange(patternMap, getGridMinPos(gridType), getGridMaxPos(gridType));

    if (textInput) {
        textInput.value = serializePatternMap(clipped.map);
    }
    renderWPEHexGrid(gridType, getGridOffset(gridType), clipped.map, getGridCols(gridType));
    return Math.max(0, hexBytes.length - clipped.removedCount);
}

function onHexCellPaste(event, cell) {
    event.preventDefault();
    const text = event.clipboardData?.getData('text') || '';
    const hexBytes = parseClipboardHexBytes(text);
    if (hexBytes.length === 0) {
        showToast('warning', '提示', '无法解析剪贴板中的 Hex 内容');
        return;
    }

    const gridType = cell.dataset.grid;
    const startPos = parseInt(cell.dataset.pos, 10);
    setHexBytesToGrid(gridType, startPos, hexBytes);

    const focusPos = startPos + hexBytes.length;
    const next = document.querySelector(`.hex-cell[data-grid="${gridType}"][data-pos="${focusPos}"]`);
    if (next) {
        next.focus();
        next.select();
    }
}

function onHexCellKeydown(e, cell) {
    if (e.key === 'ArrowLeft') {
        e.preventDefault();
        const prev = cell.previousElementSibling;
        if (prev && prev.classList.contains('hex-cell')) {
            prev.focus();
            prev.select();
        }
        return;
    }

    if (e.key === 'ArrowRight') {
        e.preventDefault();
        const next = cell.nextElementSibling;
        if (next && next.classList.contains('hex-cell')) {
            next.focus();
            next.select();
        }
        return;
    }

    if (e.key === 'Backspace' && cell.value.length === 0) {
        const prev = cell.previousElementSibling;
        if (prev && prev.classList.contains('hex-cell')) {
            prev.focus();
            prev.select();
        }
        return;
    }

    // 当前格已满时，把新输入字符送到下一个格，避免必须按到第三个字符才跳格
    if (cell.value.length >= 2 && e.key.length === 1 && /[0-9a-fA-F?]/.test(e.key)) {
        e.preventDefault();
        const next = cell.nextElementSibling;
        if (next && next.classList.contains('hex-cell')) {
            next.focus();
            next.value = e.key.toUpperCase();
            onHexCellInput(next);
        }
    }
}

function getGridCols(gridType) {
    return (gridType === 'enable' || gridType === 'disable') ? WPE_ADV_GRID_COLS : WPE_GRID_COLS;
}
function getGridOffset(gridType) {
    return {
        search: hexGridSearchOffset,
        modify: hexGridModifyOffset,
        enable: hexGridEnableOffset,
        disable: hexGridDisableOffset,
        disconnect: disconnectHexGridOffset
    }[gridType];
}
function setGridOffset(gridType, val) {
    const safeVal = clampGridOffset(gridType, val);
    if (gridType === 'search') hexGridSearchOffset = safeVal;
    else if (gridType === 'modify') hexGridModifyOffset = safeVal;
    else if (gridType === 'enable') hexGridEnableOffset = safeVal;
    else if (gridType === 'disable') hexGridDisableOffset = safeVal;
    else if (gridType === 'disconnect') disconnectHexGridOffset = safeVal;
}
function getGridPatternInput(gridType) {
    return {
        search: $('wpeSearchPattern'),
        modify: $('wpeModifyPattern'),
        enable: $('wpeAdvEnablePattern'),
        disable: $('wpeAdvDisablePattern'),
        disconnect: $('disconnectRuleHexPattern')
    }[gridType];
}

function scrollHexGrid(gridType, delta) {
    syncTextFromGrid(gridType);
    const cols = getGridCols(gridType);
    let offset = getGridOffset(gridType);
    offset = Math.max(0, Math.min(getGridTotal(gridType) - cols, offset + delta));
    setGridOffset(gridType, offset);
    const textInput = getGridPatternInput(gridType);
    renderWPEHexGrid(gridType, offset, parsePatternString(textInput?.value || ''), cols);
}

function jumpHexGrid(gridType) {
    syncTextFromGrid(gridType);
    const inputId = {
        search:'searchGridJump',
        modify:'modifyGridJump',
        enable:'enableGridJump',
        disable:'disableGridJump',
        disconnect:'disconnectGridJump'
    }[gridType];
    const val = parseInt($(inputId)?.value);
    if (isNaN(val)) return;
    const cols = getGridCols(gridType);
    const target = Math.max(0, Math.min(getGridTotal(gridType) - cols, val - getGridMinPos(gridType)));
    setGridOffset(gridType, target);
    const textInput = getGridPatternInput(gridType);
    renderWPEHexGrid(gridType, target, parsePatternString(textInput?.value || ''), cols);
}

function centerHexGrid(gridType) {
    syncTextFromGrid(gridType);
    const center = getGridDefaultOffset(gridType); // 默认从 0 位置开始显示
    setGridOffset(gridType, center);
    const cols = getGridCols(gridType);
    const textInput = getGridPatternInput(gridType);
    renderWPEHexGrid(gridType, center, parsePatternString(textInput?.value || ''), cols);
}

function pasteHexGrid(gridType) {
    navigator.clipboard.readText().then(text => {
        if (!text) return;
        const hexBytes = parseClipboardHexBytes(text);
        if (hexBytes.length === 0) { showToast('warning', '提示', '无法解析剪贴板内容'); return; }

        const focusedCell = document.activeElement && document.activeElement.classList && document.activeElement.classList.contains('hex-cell')
            && document.activeElement.dataset.grid === gridType
            ? document.activeElement
            : null;
        const startPos = focusedCell
            ? parseInt(focusedCell.dataset.pos, 10)
            : (getGridOffset(gridType) + getGridMinPos(gridType));
        const filled = setHexBytesToGrid(gridType, startPos, hexBytes);
        showToast('success', '粘贴成功', `填充了 ${filled} 个字节`);
    }).catch(() => { showToast('error', '粘贴失败', '无法读取剪贴板'); });
}

function collectHexGridToPattern(gridType) {
    const cells = document.querySelectorAll(`.hex-cell[data-grid="${gridType}"]`);
    const map = new Map();
    const textInput = getGridPatternInput(gridType);
    if (textInput) {
        const existing = parsePatternString(textInput.value);
        for (const [k, v] of existing) map.set(k, v);
    }
    cells.forEach(cell => {
        const pos = parseInt(cell.dataset.pos);
        const val = cell.value.trim().toUpperCase();
        if (val && val !== '??') {
            map.set(pos, val.padStart(2, '0'));
        } else if (val === '??') {
            map.set(pos, '??');
        } else {
            map.delete(pos);
        }
    });
    const clipped = clipPatternMapToRange(map, getGridMinPos(gridType), getGridMaxPos(gridType));
    return serializePatternMap(clipped.map);
}

function syncGridFromText(gridType) {
    const textInput = getGridPatternInput(gridType);
    const offset = getGridOffset(gridType);
    const cols = getGridCols(gridType);
    if (textInput) {
        let patternMap = parsePatternString(textInput.value);
        if (gridType === 'modify') {
            const clipped = clipPatternMapToRange(patternMap, wpeModifyRangeMin, wpeModifyRangeMax);
            patternMap = clipped.map;
            const nextText = serializePatternMap(patternMap);
            if (textInput.value !== nextText) {
                textInput.value = nextText;
            }
        }
        renderWPEHexGrid(gridType, offset, patternMap, cols);
    }
}

function syncTextFromGrid(gridType) {
    const pattern = collectHexGridToPattern(gridType);
    const textInput = getGridPatternInput(gridType);
    if (textInput) textInput.value = pattern;
}

function clearWPEHexGrid(gridType) {
    const cells = document.querySelectorAll(`.hex-cell[data-grid="${gridType}"]`);
    cells.forEach(cell => { cell.value = ''; cell.className = 'hex-cell'; });
    const textInput = getGridPatternInput(gridType);
    if (textInput) textInput.value = '';
}

function showWPEFilterEditor(filter) {
    currentEditFilterId = filter.id;
    const editPage = document.getElementById('page-wpe-edit');
    if (!editPage) return;
    document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
    editPage.classList.add('active');

    currentMainPage = 'wpe-edit';
    lastActivePage = editPage;
    const wpeNav = navItemsByPage.get('wpe');
    if (wpeNav) {
        if (lastActiveNav && lastActiveNav !== wpeNav) lastActiveNav.classList.remove('active');
        wpeNav.classList.add('active');
        lastActiveNav = wpeNav;
    }

    const title = $('wpeEditTitle');
    if (title) title.textContent = `编辑滤镜: ${filter.name} (ID: ${filter.id})`;

    const content = $('wpeEditContent');
    if (!content) return;

    hexGridSearchOffset = 500;
    hexGridEnableOffset = 500;
    hexGridDisableOffset = 500;
    const modifyRangeConfig = resolveModifyRangeConfig(filter);
    wpeModifyRangeMode = modifyRangeConfig.mode;
    wpeModifyRangeMin = modifyRangeConfig.min;
    wpeModifyRangeMax = modifyRangeConfig.max;
    hexGridModifyOffset = getGridDefaultOffset('modify');

    const mode = filter.mode || 'normal';
    const action = filter.action || 'replace';
    const startFrom = filter.startFrom || 'head';
    const dirReq = filter.direction ? filter.direction.request !== false : true;
    const dirResp = filter.direction ? filter.direction.response !== false : true;
    const globalAll = filter.target ? filter.target.allInstances : false;
    const targetIds = filter.target ? (filter.target.targetInstanceIds || []) : [];
    const priority = filter.priority || 'before';
    const collectorPriority = filter.collectorPriority || 'after';
    const prog = filter.progression || {};
    const adv = filter.advancedToggle || {};

    content.innerHTML = `
        <div class="card glass" style="margin-bottom:16px;">
            <div class="card-body">
                <h3 class="section-title">基本信息</h3>
                <div style="display:grid; grid-template-columns:1fr 1fr; gap:12px;">
                    <div class="form-group">
                        <label for="wpeFilterName">滤镜名称</label>
                        <input type="text" id="wpeFilterName" class="input" value="${filter.name || ''}">
                    </div>
                    <div class="form-group">
                        <label>滤镜ID: ${filter.id} | 执行次数: ${filter.executionCount || 0}</label>
                        <label><input type="checkbox" id="wpeFilterEnabled" ${filter.enabled ? 'checked' : ''}> 启用滤镜</label>
                    </div>
                </div>
            </div>
        </div>

        <div class="card glass" style="margin-bottom:16px;">
            <div class="card-body">
                <h3 class="section-title">条件判断</h3>
                <div style="display:grid; grid-template-columns:1fr 1fr 1fr; gap:12px;">
                    <div class="form-group">
                        <label><input type="checkbox" id="wpeAppointHeader" ${filter.appointHeader ? 'checked' : ''}> 指定包头</label>
                        <input type="text" id="wpeHeaderContent" class="input" value="${filter.headerContent || ''}" placeholder="十六进制包头内容">
                    </div>
                    <div class="form-group">
                        <label><input type="checkbox" id="wpeAppointLength" ${filter.appointLength ? 'checked' : ''}> 指定长度范围</label>
                        <div style="display:flex; gap:8px;">
                            <input type="number" id="wpeMinLength" class="input" value="${filter.minLength || 0}" placeholder="最小" style="width:100px;">
                            <span style="line-height:36px;">~</span>
                            <input type="number" id="wpeMaxLength" class="input" value="${filter.maxLength || 65535}" placeholder="最大" style="width:100px;">
                        </div>
                    </div>
                    <div class="form-group">
                        <label><input type="checkbox" id="wpeAppointPort" ${filter.appointPort ? 'checked' : ''}> 指定端口</label>
                        <input type="number" id="wpePortContent" class="input" value="${filter.portContent || 0}" placeholder="端口号">
                    </div>
                </div>
            </div>
        </div>

        <div class="card glass" style="margin-bottom:16px;">
            <div class="card-body">
                <h3 class="section-title">滤镜配置</h3>
                <div style="display:grid; grid-template-columns:1fr 1fr 1fr; gap:12px;">
                    <div class="form-group">
                        <label for="wpeMode">滤镜模式</label>
                        <select id="wpeMode" class="input">
                            <option value="normal" ${mode === 'normal' ? 'selected' : ''}>普通模式</option>
                            <option value="advanced" ${mode === 'advanced' ? 'selected' : ''}>高级模式</option>
                        </select>
                    </div>
                    <div class="form-group">
                        <label for="wpeAction">滤镜动作</label>
                        <select id="wpeAction" class="input">
                            <option value="replace" ${action === 'replace' ? 'selected' : ''}>替换</option>
                            <option value="intercept" ${action === 'intercept' ? 'selected' : ''}>拦截</option>
                            <option value="display" ${action === 'display' ? 'selected' : ''}>不修改(显示)</option>
                            <option value="nodisplay" ${action === 'nodisplay' ? 'selected' : ''}>不修改(不显示)</option>
                            <option value="change" ${action === 'change' ? 'selected' : ''}>换包</option>
                        </select>
                    </div>
                    <div class="form-group">
                        <label for="wpeStartFrom">修改起始位置</label>
                        <select id="wpeStartFrom" class="input">
                            <option value="head" ${startFrom === 'head' ? 'selected' : ''}>从包头开始</option>
                            <option value="position" ${startFrom === 'position' ? 'selected' : ''}>从匹配位置开始</option>
                        </select>
                    </div>
                </div>
                <div style="display:grid; grid-template-columns:1fr 1fr 1fr; gap:12px; margin-top:12px;">
                    <div class="form-group">
                        <label>数据方向</label>
                        <label><input type="checkbox" id="wpeDirRequest" ${dirReq ? 'checked' : ''}> 请求(发送)</label>
                        <label><input type="checkbox" id="wpeDirResponse" ${dirResp ? 'checked' : ''}> 响应(接收)</label>
                    </div>
                    <div class="form-group">
                        <label>生效目标</label>
                        <label><input type="checkbox" id="wpeGlobal" ${globalAll ? 'checked' : ''} onchange="wpeToggleGlobalTarget()"> 对所有Socks转发实例生效</label>
                        <div id="wpeTargetInstanceList" style="display:${globalAll ? 'none' : 'block'}; margin-top:8px;">
                            <div style="font-size:12px; color:var(--text-muted); margin-bottom:6px;">选择Socks转发实例（可多选）:</div>
                            <div id="wpeTargetInstances" style="max-height:120px; overflow-y:auto; border:1px solid var(--border); border-radius:var(--radius-sm); background:var(--bg-input); padding:4px;">
                                ${renderWpeTargetInstances(targetIds)}
                            </div>
                        </div>
                    </div>
                    <div class="form-group" style="display:none;">
                        <label for="wpePriority">伪心跳端优先级</label>
                        <select id="wpePriority" class="input">
                            <option value="before" ${priority === 'before' ? 'selected' : ''}>滤镜优先(先执行滤镜)</option>
                            <option value="after" ${priority === 'after' ? 'selected' : ''}>心跳优先(先执行心跳)</option>
                        </select>
                        <label for="wpeCollectorPriority" style="margin-top:8px;">采集端优先级</label>
                        <select id="wpeCollectorPriority" class="input">
                            <option value="before" ${collectorPriority === 'before' ? 'selected' : ''}>滤镜优先</option>
                            <option value="after" ${collectorPriority === 'after' ? 'selected' : ''}>采集优先</option>
                        </select>
                    </div>
                </div>
            </div>
        </div>

        <div class="card glass" style="margin-bottom:16px;">
            <div class="card-body">
                <h3 class="section-title">搜索内容</h3>
                <div class="form-group">
                    <input type="text" id="wpeSearchPattern" class="input" value="${filter.searchPattern || ''}"
                        placeholder="格式: 位置|十六进制值,...  例如: 0|01,1|0A,2|00" onchange="syncGridFromText('search')">
                </div>
                <div class="toolbar" style="margin:4px 0; flex-wrap:wrap; gap:4px; align-items:center;">
                    <span style="font-size:12px; color:var(--text-muted);">位置:</span>
                    <input type="number" id="searchGridJump" class="input" style="width:70px; height:28px; font-size:12px;" value="0"
                        onchange="jumpHexGrid('search')">
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('search', -${WPE_GRID_COLS})">&lt;&lt;</button>
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('search', -1)">&lt;</button>
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('search', 1)">&gt;</button>
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('search', ${WPE_GRID_COLS})">&gt;&gt;</button>
                    <button class="btn btn-ghost btn-sm" onclick="centerHexGrid('search')">0</button>
                    <button class="btn btn-ghost btn-sm" style="color:#6f6;" onclick="pasteHexGrid('search')">粘贴</button>
                    <button class="btn btn-ghost btn-sm" style="color:#f66;" onclick="clearWPEHexGrid('search')">清空</button>
                    <span style="font-size:11px; color:var(--text-muted);">(留空=跳过, ??=通配符)</span>
                    <span id="searchGridRange" style="font-size:11px; color:#6c6;"></span>
                </div>
                <div id="searchHexGridView"></div>
            </div>
        </div>

        <div class="card glass" style="margin-bottom:16px;">
            <div class="card-body">
                <h3 class="section-title">修改内容</h3>
                <div class="form-group">
                    <input type="text" id="wpeModifyPattern" class="input" value="${filter.modifyPattern || ''}"
                        placeholder="格式: 位置|十六进制值,...  例如: 0|FF,1|00" onchange="syncGridFromText('modify')">
                </div>
                <div style="display:grid; grid-template-columns:minmax(180px, 240px) minmax(200px, 1fr); gap:12px; align-items:end; margin-bottom:8px;">
                    <div class="form-group" style="margin-bottom:0;">
                        <label for="wpeModifyRangeMode">修改模式</label>
                        <select id="wpeModifyRangeMode" class="input" onchange="onModifyRangeModeChange()">
                            <option value="${WPE_MODIFY_RANGE_MODE_STANDARD}" ${wpeModifyRangeMode === WPE_MODIFY_RANGE_MODE_STANDARD ? 'selected' : ''}>常规修改模式</option>
                            <option value="${WPE_MODIFY_RANGE_MODE_CUSTOM}" ${wpeModifyRangeMode === WPE_MODIFY_RANGE_MODE_CUSTOM ? 'selected' : ''}>自定义修改模式</option>
                        </select>
                    </div>
                    <div id="wpeModifyRangeCustomPanel" style="display:${wpeModifyRangeMode === WPE_MODIFY_RANGE_MODE_CUSTOM ? 'grid' : 'none'}; grid-template-columns:repeat(2, minmax(120px, 160px)); gap:12px;">
                        <div class="form-group" style="margin-bottom:0;">
                            <label for="wpeModifyRangeMin">负向范围</label>
                            <input type="number" id="wpeModifyRangeMin" class="input" value="${wpeModifyRangeMin}" onchange="onModifyRangeBoundsChange()">
                        </div>
                        <div class="form-group" style="margin-bottom:0;">
                            <label for="wpeModifyRangeMax">正向范围</label>
                            <input type="number" id="wpeModifyRangeMax" class="input" value="${wpeModifyRangeMax}" onchange="onModifyRangeBoundsChange()">
                        </div>
                    </div>
                </div>
                <div class="toolbar" style="margin:4px 0; flex-wrap:wrap; gap:4px; align-items:center;">
                    <span style="font-size:12px; color:var(--text-muted);">位置:</span>
                    <input type="number" id="modifyGridJump" class="input" style="width:70px; height:28px; font-size:12px;" value="0"
                        onchange="jumpHexGrid('modify')">
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('modify', -${WPE_GRID_COLS})">&lt;&lt;</button>
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('modify', -1)">&lt;</button>
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('modify', 1)">&gt;</button>
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('modify', ${WPE_GRID_COLS})">&gt;&gt;</button>
                    <button class="btn btn-ghost btn-sm" onclick="centerHexGrid('modify')">0</button>
                    <button class="btn btn-ghost btn-sm" style="color:#6f6;" onclick="pasteHexGrid('modify')">粘贴</button>
                    <button class="btn btn-ghost btn-sm" style="color:#ffd700;" onclick="markModifyCellRandom()">随机</button>
                    <button class="btn btn-ghost btn-sm" style="color:#f66;" onclick="clearWPEHexGrid('modify')">清空</button>
                    <span style="font-size:11px; color:var(--text-muted);">(留空=跳过，RR=随机字节)</span>
                    <span id="modifyRangeModeHint" style="font-size:11px; color:var(--text-muted);">当前范围: ${wpeModifyRangeMin} ~ ${wpeModifyRangeMax}</span>
                    <span id="modifyGridRange" style="font-size:11px; color:#6c6;"></span>
                </div>
                <div id="modifyHexGridView"></div>
            </div>
        </div>

        <div class="card glass" style="margin-bottom:16px;">
            <div class="card-body">
                <h3 class="section-title">递进配置</h3>
                <div class="form-group">
                    <label><input type="checkbox" id="wpeProgEnabled" ${prog.isEnabled ? 'checked' : ''}> 启用递进</label>
                </div>
                <div style="display:grid; grid-template-columns:1fr 1fr 1fr; gap:12px;">
                    <div class="form-group">
                        <label><input type="checkbox" id="wpeProgContinuous" ${prog.isContinuous ? 'checked' : ''}> 连续递进</label>
                    </div>
                    <div class="form-group">
                        <label for="wpeProgStep">递进步长</label>
                        <input type="number" id="wpeProgStep" class="input" value="${prog.step || 1}">
                    </div>
                    <div class="form-group">
                        <label><input type="checkbox" id="wpeProgCarry" ${prog.enableCarry ? 'checked' : ''}> 启用进位</label>
                        <label for="wpeProgCarryDigits" style="margin-top:4px;">进位位数</label>
                        <input type="number" id="wpeProgCarryDigits" class="input" value="${prog.carryDigits || 1}">
                    </div>
                </div>
                <div class="form-group">
                    <label for="wpeProgPositions">递进位置（逗号分隔）</label>
                    <input type="text" id="wpeProgPositions" class="input" value="${prog.positions || ''}" placeholder="例如: 0,1,2">
                </div>
            </div>
        </div>

        <div class="card glass" style="margin-bottom:16px;">
            <div class="card-body">
                <h3 class="section-title">高级设置（账号级别动态开关）</h3>
                <div class="form-group">
                    <label><input type="checkbox" id="wpeAdvEnabled" ${adv.isEnabled ? 'checked' : ''}> 启用账号级别动态开关</label>
                </div>
                <div style="color:#f5c842; font-size:12px; margin:8px 0 12px;">说明：为每个SOCKS5账号独立维护滤镜开关状态，通过特定数据包触发开启或关闭</div>
                <div class="form-group">
                    <label for="wpeAdvDefaultState">默认状态</label>
                    <select id="wpeAdvDefaultState" class="input" style="width:120px;">
                        <option value="off" ${!adv.defaultState ? 'selected' : ''}>关闭</option>
                        <option value="on" ${adv.defaultState ? 'selected' : ''}>开启</option>
                    </select>
                </div>

                <h4 style="font-size:14px; margin:16px 0 8px; color:var(--text); border-top:1px solid var(--border); padding-top:12px;">开启触发条件（十六进制）</h4>
                <div class="form-group">
                    <label><input type="checkbox" id="wpeAdvEnableTrigger" ${adv.enableTriggerEnabled ? 'checked' : ''}> 启用开启触发</label>
                </div>
                <input type="hidden" id="wpeAdvEnablePattern" value="${adv.enablePattern || ''}">
                <div class="toolbar" style="margin:4px 0; flex-wrap:wrap; gap:4px; align-items:center;">
                    <span style="font-size:12px; color:var(--text-muted);">位置:</span>
                    <input type="number" id="enableGridJump" class="input" style="width:70px; height:28px; font-size:12px;" value="0"
                        onchange="jumpHexGrid('enable')">
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('enable', -${WPE_ADV_GRID_COLS})"><<</button>
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('enable', -1)"><</button>
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('enable', 1)">></button>
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('enable', ${WPE_ADV_GRID_COLS})">>></button>
                    <button class="btn btn-ghost btn-sm" onclick="centerHexGrid('enable')">0</button>
                    <button class="btn btn-ghost btn-sm" style="color:#6f6;" onclick="pasteHexGrid('enable')">粘贴</button>
                    <button class="btn btn-ghost btn-sm" style="color:#f66;" onclick="clearWPEHexGrid('enable')">清空</button>
                    <span id="enableGridRange" style="font-size:11px; color:#6c6;"></span>
                </div>
                <div id="enableHexGridView"></div>
                <div class="form-group" style="margin-top:8px;">
                    <label><input type="checkbox" id="wpeAdvApplyOnEnable" ${adv.applyOnEnableTrigger ? 'checked' : ''}> 触发开启时，触发包本身执行滤镜</label>
                </div>

                <h4 style="font-size:14px; margin:16px 0 8px; color:var(--text); border-top:1px solid var(--border); padding-top:12px;">关闭触发条件（十六进制）</h4>
                <div class="form-group">
                    <label><input type="checkbox" id="wpeAdvDisableTrigger" ${adv.disableTriggerEnabled ? 'checked' : ''}> 启用关闭触发</label>
                </div>
                <input type="hidden" id="wpeAdvDisablePattern" value="${adv.disablePattern || ''}">
                <div class="toolbar" style="margin:4px 0; flex-wrap:wrap; gap:4px; align-items:center;">
                    <span style="font-size:12px; color:var(--text-muted);">位置:</span>
                    <input type="number" id="disableGridJump" class="input" style="width:70px; height:28px; font-size:12px;" value="0"
                        onchange="jumpHexGrid('disable')">
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('disable', -${WPE_ADV_GRID_COLS})"><<</button>
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('disable', -1)"><</button>
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('disable', 1)">></button>
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('disable', ${WPE_ADV_GRID_COLS})">>></button>
                    <button class="btn btn-ghost btn-sm" onclick="centerHexGrid('disable')">0</button>
                    <button class="btn btn-ghost btn-sm" style="color:#6f6;" onclick="pasteHexGrid('disable')">粘贴</button>
                    <button class="btn btn-ghost btn-sm" style="color:#f66;" onclick="clearWPEHexGrid('disable')">清空</button>
                    <span id="disableGridRange" style="font-size:11px; color:#6c6;"></span>
                </div>
                <div id="disableHexGridView"></div>
                <div class="form-group" style="margin-top:8px;">
                    <label><input type="checkbox" id="wpeAdvApplyOnDisable" ${adv.applyOnDisableTrigger !== false ? 'checked' : ''}> 触发关闭时，触发包本身执行滤镜</label>
                </div>
                </div>
            </div>
        </div>

        <div class="toolbar" style="justify-content:flex-end; padding:12px 0;">
            <button class="btn btn-ghost" onclick="backToWPEFilters()">取消</button>
            <button class="btn btn-primary" onclick="saveWPEFilterFull()">保存滤镜</button>
        </div>
    `;

    updateModifyRangeUi();
    trimModifyPatternToCurrentRange(false);
    renderWPEHexGrid('search', hexGridSearchOffset, parsePatternString(filter.searchPattern || ''));
    renderWPEHexGrid('modify', hexGridModifyOffset, parsePatternString($('wpeModifyPattern')?.value || filter.modifyPattern || ''));
    renderWPEHexGrid('enable', hexGridEnableOffset, parsePatternString(adv.enablePattern || ''), WPE_ADV_GRID_COLS);
    renderWPEHexGrid('disable', hexGridDisableOffset, parsePatternString(adv.disablePattern || ''), WPE_ADV_GRID_COLS);
}

function saveWPEFilterFull() {
    const name = $('wpeFilterName')?.value?.trim();
    if (!name) {
        showToast('warning', '提示', '请输入滤镜名称');
        return;
    }

    if (!syncModifyRangeStateBeforeSave()) {
        return;
    }

    syncTextFromGrid('search');
    syncTextFromGrid('modify');
    syncTextFromGrid('enable');
    syncTextFromGrid('disable');

    const filter = {
        name: name,
        enabled: $('wpeFilterEnabled')?.checked || false,
        mode: $('wpeMode')?.value || 'normal',
        action: $('wpeAction')?.value || 'replace',
        startFrom: $('wpeStartFrom')?.value || 'head',
        direction: {
            request: $('wpeDirRequest')?.checked || false,
            response: $('wpeDirResponse')?.checked || false
        },
        target: {
            allInstances: $('wpeGlobal')?.checked || false,
            targetInstanceIds: $('wpeGlobal')?.checked ? [] :
                Array.from(document.querySelectorAll('.wpeTargetInst:checked')).map(cb => cb.value)
        },
        priority: $('wpePriority')?.value || 'before',
        collectorPriority: $('wpeCollectorPriority')?.value || 'after',
        searchPattern: $('wpeSearchPattern')?.value || '',
        modifyPattern: $('wpeModifyPattern')?.value || '',
        modifyRangeMode: wpeModifyRangeMode,
        modifyRangeMin: wpeModifyRangeMin,
        modifyRangeMax: wpeModifyRangeMax,
        appointHeader: $('wpeAppointHeader')?.checked || false,
        headerContent: $('wpeHeaderContent')?.value || '',
        appointLength: $('wpeAppointLength')?.checked || false,
        minLength: parseInt($('wpeMinLength')?.value) || 0,
        maxLength: parseInt($('wpeMaxLength')?.value) || 65535,
        appointPort: $('wpeAppointPort')?.checked || false,
        portContent: parseInt($('wpePortContent')?.value) || 0,
        progression: {
            isEnabled: $('wpeProgEnabled')?.checked || false,
            isContinuous: $('wpeProgContinuous')?.checked || false,
            step: parseInt($('wpeProgStep')?.value) || 1,
            enableCarry: $('wpeProgCarry')?.checked || false,
            carryDigits: parseInt($('wpeProgCarryDigits')?.value) || 1,
            positions: $('wpeProgPositions')?.value || ''
        },
        advancedToggle: {
            isEnabled: $('wpeAdvEnabled')?.checked || false,
            defaultState: $('wpeAdvDefaultState')?.value === 'on',
            enableTriggerEnabled: $('wpeAdvEnableTrigger')?.checked || false,
            enablePattern: $('wpeAdvEnablePattern')?.value || '',
            disableTriggerEnabled: $('wpeAdvDisableTrigger')?.checked || false,
            disablePattern: $('wpeAdvDisablePattern')?.value || '',
            applyOnEnableTrigger: $('wpeAdvApplyOnEnable')?.checked || false,
            applyOnDisableTrigger: $('wpeAdvApplyOnDisable')?.checked || false
        }
    };

    if (wpeIsNewFilter) {
        postAction('wpe_filter_create_full', { filter });
    } else {
        postAction('wpe_filter_update', { filterId: currentEditFilterId, filter });
    }
    showToast('info', '提示', '正在保存...');
}



// ========== Online Stats Page (在线统计) ==========
$('btnRefreshOnline').addEventListener('click', () => {
    const instanceId = $('onlineInstanceSelector').value;
    postAction('get_online_stats', { instanceId: instanceId });
});

// 实例选择器变化时自动刷新
$('onlineInstanceSelector').addEventListener('change', () => {
    const instanceId = $('onlineInstanceSelector').value;
    postAction('get_online_stats', { instanceId: instanceId });
});

function buildOnlineStatsRowKey(acc) {
    return [
        acc.username,
        acc.currentConnections || 0,
        acc.maxConnections || 0,
        acc.expireTime || '',
        acc.lastLoginTime || '',
        acc.lastLoginIP || '',
        acc.onlineDuration || '',
        acc.instanceId || ''
    ].join(',');
}

function buildOnlineStatsRowsKey(accounts) {
    return (accounts || []).map(buildOnlineStatsRowKey).join('|');
}

function buildOnlineStatsRowHtml(acc) {
    const isOnline = acc.currentConnections > 0;
    const statusColor = isOnline ? '#4ade80' : '#94a3b8';
    const statusText = isOnline ? '在线' : '离线';
    return `
        <tr>
            <td>${acc.username || '未知'}</td>
            <td><span style="color:${statusColor}">● ${statusText}</span></td>
            <td>${acc.currentConnections || 0}</td>
            <td>${acc.maxConnections || 0}</td>
            <td>${acc.expireTime || '-'}</td>
            <td>${acc.lastLoginTime || '-'}</td>
            <td>${acc.lastLoginIP || '-'}</td>
            <td>${acc.onlineDuration || '-'}</td>
            <td>
                ${isOnline ? `<button class="btn btn-danger btn-sm" onclick="kickOnlineUser('${acc.username}', '${acc.instanceId}')">踢出</button>` : '-'}
            </td>
        </tr>
    `;
}

function syncOnlineStatsTableRows(accounts) {
    const tbody = $('onlineStatsTableBody');
    if (!tbody) return;

    const nextRowKeys = (accounts || []).map(buildOnlineStatsRowKey);
    const rows = tbody.rows;

    if (rows.length === nextRowKeys.length && lastOnlineTableRowKeys.length === nextRowKeys.length) {
        for (let i = 0; i < nextRowKeys.length; i++) {
            if (lastOnlineTableRowKeys[i] !== nextRowKeys[i]) {
                rows[i].outerHTML = buildOnlineStatsRowHtml(accounts[i]);
            }
        }
    } else {
        tbody.innerHTML = (accounts || []).map(buildOnlineStatsRowHtml).join('');
    }

    lastOnlineTableRowKeys = nextRowKeys;
}

function renderOnlineStats(data) {
    cachedOnlineStatsData = data || null;

    if (!isMainPageActive('online')) {
        return;
    }

    const container = $('onlineList');
    const accountsList = data.accounts || [];
    const statsCard = $('onlineStatsCards');
    const selector = $('onlineInstanceSelector');

    const statsKey = `${onlineDataRevision}|stats`;

    if (statsCard && statsKey !== lastOnlineStatsCardsKey) {
        lastOnlineStatsCardsKey = statsKey;
        if (data.stats) {
            statsCard.innerHTML = `
                <div class="stats-grid">
                    <div class="stat-card">
                        <div class="stat-value">${data.stats.totalAccounts || 0}</div>
                        <div class="stat-label">总账号数</div>
                    </div>
                    <div class="stat-card">
                        <div class="stat-value">${data.stats.onlineCount || 0}</div>
                        <div class="stat-label">在线账号</div>
                    </div>
                    <div class="stat-card">
                        <div class="stat-value">${data.stats.offlineCount || 0}</div>
                        <div class="stat-label">离线账号</div>
                    </div>
                    <div class="stat-card">
                        <div class="stat-value">${data.stats.totalConnections || 0}</div>
                        <div class="stat-label">总连接数</div>
                    </div>
                </div>
            `;
        }
    }

    if (data.instances) {
        const selectorKey = `${onlineDataRevision}|selector|${(data.instances || []).length}`;
        if (selector && selectorKey !== lastOnlineSelectorKey) {
            lastOnlineSelectorKey = selectorKey;
            const currentValue = selector.value;
            selector.innerHTML = '<option value="">请选择实例</option>' +
                data.instances.map(inst => `
                    <option value="${inst.id}" ${currentValue === inst.id ? 'selected' : ''}>${inst.name}</option>
                `).join('');
        }
    }

    if (accountsList.length === 0) {
        if (container.dataset.mode !== 'empty') {
            container.innerHTML = '<div class="empty-state">请先选择一个 SOCKS 转发实例</div>';
            container.dataset.mode = 'empty';
        }
        lastOnlineTableRowsKey = '';
        lastOnlineTableRowKeys = [];
        return;
    }

    if (container.dataset.mode !== 'table') {
        container.innerHTML = `
            <table class="data-table">
                <thead>
                    <tr>
                        <th>用户名</th>
                        <th>状态</th>
                        <th>当前连接</th>
                        <th>最大连接</th>
                        <th>到期时间</th>
                        <th>最后登录时间</th>
                        <th>最后登录IP</th>
                        <th>累计在线时长</th>
                        <th>操作</th>
                    </tr>
                </thead>
                <tbody id="onlineStatsTableBody"></tbody>
            </table>
        `;
        container.dataset.mode = 'table';
        lastOnlineTableRowsKey = '';
        lastOnlineTableRowKeys = [];
    }

    const rowsKey = buildOnlineStatsRowsKey(accountsList);
    if (rowsKey === lastOnlineTableRowsKey) {
        return;
    }
    lastOnlineTableRowsKey = rowsKey;

    syncOnlineStatsTableRows(accountsList);
}

function kickOnlineUser(username, instanceId) {
    if (!confirm('确定要踢出用户 ' + username + ' 吗？')) {
        return;
    }
    postAction('online_kick', { username: username, instanceId: instanceId });
}

function kickClient(clientId) {
    if (confirm('确定要踢出此客户端吗？')) {
        postAction('online_kick', { clientId });
    }
}

function formatBytes(bytes) {
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1048576) return (bytes / 1024).toFixed(1) + ' KB';
    if (bytes < 1073741824) return (bytes / 1048576).toFixed(1) + ' MB';
    return (bytes / 1073741824).toFixed(2) + ' GB';
}

// ========== AntiCC Page (防CC配置) ==========
$('btnSaveAntiCC').addEventListener('click', () => {
    const antiCCInstance = resolveAntiCCInstance();
    if (!antiCCInstance) {
        toast('请先选择一个Socks转发实例', 'error');
        return;
    }

    const config = {
        instanceId: antiCCInstance.id,
        enabled: $('anticcEnabled').checked,
        timeWindow: parseInt($('anticcTimeWindow').value),
        maxRequests: parseInt($('anticcMaxRequests').value),
        banTime: parseInt($('anticcBanTime').value),
        maxConnectionsTotal: parseInt($('anticcMaxConnectionsTotal').value),
        authFailBanTime: parseInt($('anticcAuthFailBanTime').value),
        noAuthBanTime: parseInt($('anticcNoAuthBanTime').value),
        whitelistDuration: parseInt($('anticcWhitelistDuration').value),
        useBlacklist: $('anticcUseBlacklist').checked,
        useWhitelist: $('anticcUseWhitelist').checked,
        useFirewall: $('anticcFirewallEnabled').checked,
        rateLimitEnabled: $('anticcRateLimitEnabled').checked,
        rateLimit: parseInt($('anticcMaxBytesPerSecond').value),
        rateTimeWindow: parseInt($('anticcMaxPacketsPerSecond').value),
        blockNonSocks: $('anticcBlockNonSocks').checked,
        enableAuthPriorityAdmission: $('anticcAutoBlockEnabled').checked,
        authPriorityQueueLimit: parseInt($('anticcAuthTimeout').value),
        enableLowPriorityEviction: $('anticcEnableLowPriorityEviction').checked,
        lowPriorityEvictionThreshold: parseInt($('anticcMaxConnectionsPerIP').value),
        whitelistEntries: buildAntiCCWhitelistEntries(antiCCInstance.id),
        whitelist: $('anticcWhitelist').value.split('\n').filter(ip => ip.trim()),
        blacklist: $('anticcBlacklist').value.split('\n').filter(ip => ip.trim())
    };
    postAction('anticc_save_config', config);
});

$('btnResetAntiCC').addEventListener('click', () => {
    const antiCCInstance = resolveAntiCCInstance();
    if (!antiCCInstance) {
        toast('请先选择一个Socks转发实例', 'error');
        return;
    }
    if (confirm('确定要重置为默认配置吗？')) {
        postAction('anticc_reset_config', { instanceId: antiCCInstance.id });
    }
});

$('anticcWhitelistScope').addEventListener('change', () => {
    updateAntiCCWhitelistScopeUi();
});

// Load AntiCC config when page is shown
function loadAntiCCConfig() {
    const antiCCInstance = resolveAntiCCInstance();
    if (!antiCCInstance) {
        toast('暂无可配置的Socks转发实例', 'warning');
        return;
    }
    populateAntiCCWhitelistTargets(antiCCInstance.id);
    updateAntiCCWhitelistScopeUi();
    postAction('anticc_get_config', { instanceId: antiCCInstance.id });
    postAction('anticc_get_global_state');
}

function updateAntiCCForm(config) {
    if (!config) return;

    const antiCCInstance = resolveAntiCCInstance();
    $('anticcInstanceSubtitle').textContent = antiCCInstance
        ? `当前实例: ${antiCCInstance.name} (ID: ${antiCCInstance.id})`
        : '请先选择一个 Socks 转发实例';

    $('anticcEnabled').checked = config.enabled || false;
    $('anticcTimeWindow').value = config.timeWindow || 10;
    $('anticcMaxRequests').value = config.maxRequests || 20;
    $('anticcBanTime').value = config.banTime || 300;
    $('anticcMaxConnectionsTotal').value = config.maxConnections || 100;
    $('anticcAuthFailBanTime').value = config.authFailBanTime || 60;
    $('anticcNoAuthBanTime').value = config.noAuthBanTime || 30;
    $('anticcWhitelistDuration').value = config.whitelistDuration || 3600;
    $('anticcUseBlacklist').checked = config.useBlacklist !== false;
    $('anticcUseWhitelist').checked = config.useWhitelist !== false;
    $('anticcFirewallEnabled').checked = config.useFirewall || false;
    $('anticcRateLimitEnabled').checked = config.rateLimitEnabled || false;
    $('anticcMaxBytesPerSecond').value = config.rateLimit || 100;
    $('anticcMaxPacketsPerSecond').value = config.rateTimeWindow || 1;
    $('anticcBlockNonSocks').checked = config.blockNonSocks || false;
    $('anticcAutoBlockEnabled').checked = config.enableAuthPriorityAdmission !== false;
    $('anticcAuthTimeout').value = config.authPriorityQueueLimit || 128;
    $('anticcEnableLowPriorityEviction').checked = config.enableLowPriorityEviction !== false;
    $('anticcMaxConnectionsPerIP').value = config.lowPriorityEvictionThreshold || 80;
    $('anticcThreadPoolMode').value = 'iocp';
    $('anticcWhitelist').value = (config.whitelist || []).join('\n');
    $('anticcBlacklist').value = (config.blacklist || []).join('\n');

    const whitelistEntries = (config.whitelistEntries || []).filter(entry => {
        if (!antiCCInstance) return false;
        return (entry.sourceInstanceId || antiCCInstance.id) === antiCCInstance.id;
    });
    let scopeMode = 'current';
    if (whitelistEntries.length > 0) {
        const hasGlobal = whitelistEntries.some(entry => entry.scopeType === 0);
        if (hasGlobal) {
            scopeMode = 'global';
        } else {
            const currentOnly = whitelistEntries.every(entry => {
                const targets = entry.targetInstanceIds || [];
                return targets.length === 1 && antiCCInstance && targets[0] === antiCCInstance.id;
            });
            scopeMode = currentOnly ? 'current' : 'selected';
        }
    }
    $('anticcWhitelistScope').value = scopeMode;
    populateAntiCCWhitelistTargets(antiCCInstance ? antiCCInstance.id : '', whitelistEntries);
    updateAntiCCWhitelistScopeUi();

    if (cachedAntiCCGlobalState) {
        updateAntiCCGlobalState(cachedAntiCCGlobalState);
    }
}

function updateAntiCCGlobalState(state) {
    if (!state) return;

    const pressureNames = {
        0: 'Normal',
        1: 'Busy',
        2: 'Overloaded',
        3: 'Critical'
    };

    $('anticcGlobalConnectionCount').value = state.globalConnectionCount || 0;
    $('anticcInstanceConnectionCount').value = (cachedAntiCCConfig && cachedAntiCCConfig.instanceConnectionCount) || 0;
    $('anticcGlobalPressure').value = pressureNames[state.globalPressure] || 'Unknown';

    const blacklistView = $('anticcGlobalBlacklistView');
    if (blacklistView) {
        const blacklist = state.blacklist || [];
        blacklistView.textContent = blacklist.length ? blacklist.join('\n') : '暂无全局黑名单';
    }

    const whitelistView = $('anticcGlobalWhitelistEntriesView');
    if (whitelistView) {
        const entries = state.whitelistEntries || [];
        whitelistView.textContent = entries.length
            ? entries.map(entry => {
                const scopeMap = { 0: '全局', 1: '实例', 2: '选定实例' };
                const targets = (entry.targetInstanceIds || []).join(', ') || '-';
                return `${entry.ip} | ${scopeMap[entry.scopeType] || '未知'} | ${entry.source || '-'} | 来源:${entry.sourceInstanceId || '-'} | ${targets}`;
            }).join('\n')
            : '暂无白名单作用域条目';
    }

    const topIpView = $('anticcTopIpConnectionsView');
    if (topIpView) {
        const topIpConnections = state.topIpConnections || [];
        topIpView.textContent = topIpConnections.length
            ? topIpConnections.map(item => `${item.ip} | ${item.connections}`).join('\n')
            : '暂无跨实例活跃IP';
    }
}

// ========== WPE Filter Page (WPE滤镜) ==========
let wpeIsNewFilter = false;

$('btnCreateFilter').addEventListener('click', () => {
    // 直接进入编辑页面，使用默认值创建新滤镜
    wpeIsNewFilter = true;
    currentEditFilterId = null;
    showWPEFilterEditor({
        id: 0,
        name: '',
        enabled: true,
        executionCount: 0,
        mode: 'normal',
        action: 'replace',
        startFrom: 'head',
        direction: { request: true, response: true },
        target: { allInstances: false, targetInstanceIds: [] },
        priority: 'before',
        collectorPriority: 'after',
        searchPattern: '',
        modifyPattern: '',
        appointHeader: false,
        headerContent: '',
        appointLength: false,
        minLength: 0,
        maxLength: 65535,
        appointPort: false,
        portContent: 0,
        modifyRangeMode: WPE_MODIFY_RANGE_MODE_STANDARD,
        modifyRangeMin: WPE_DEFAULT_GRID_MIN_POS,
        modifyRangeMax: WPE_DEFAULT_GRID_MAX_POS,
        progression: {},
        advancedToggle: {}
    });
});

function editSelectedFilter() {
    if (wpeSelectedIds.size === 0) {
        showToast('warning', '提示', '请先选择一个滤镜');
        return;
    }
    wpeIsNewFilter = false;
    const firstId = [...wpeSelectedIds][0];
    postAction('wpe_filter_get', { filterId: firstId });
}

function deleteSelectedFilter() {
    if (wpeSelectedIds.size === 0) {
        showToast('warning', '提示', '请先选择要删除的滤镜');
        return;
    }
    if (!confirm(`确定要删除选中的 ${wpeSelectedIds.size} 个滤镜吗？`)) return;
    for (const id of wpeSelectedIds) {
        postAction('wpe_filter_delete', { filterId: id });
    }
    wpeSelectedIds.clear();
}

function moveFilter(direction) {
    if (wpeSelectedIds.size !== 1) {
        showToast('warning', '提示', '请选择一个滤镜进行移动');
        return;
    }
    const filterId = [...wpeSelectedIds][0];
    postAction('wpe_filter_move', { filterId, direction });
}

function showExportDialog() {
    const selectedCount = wpeSelectedIds.size;
    const totalCount = wpeFilters.length;
    showModal('导出滤镜(加密)', `
        <p>已选: ${selectedCount} / 总数: ${totalCount}</p>
        <div class="form-group">
            <label>
                <input type="checkbox" id="wpeExportUsePassword"> 使用密码保护
            </label>
        </div>
        <div class="form-group" id="wpeExportPasswordGroup" style="display:none;">
            <label for="wpeExportPassword">密码</label>
            <input type="password" id="wpeExportPassword" class="input" placeholder="请输入加密密码">
        </div>
        <div class="toolbar" style="margin-top:8px;">
            <button class="btn btn-ghost" onclick="wpeExportSelectAll()">全选</button>
            <button class="btn btn-ghost" onclick="wpeExportSelectNone()">全不选</button>
            <button class="btn btn-ghost" onclick="wpeExportSelectEnabled()">仅选启用</button>
        </div>
    `, `
        <button class="btn btn-ghost" onclick="closeModal()">关闭</button>
        <button class="btn btn-primary" onclick="doExportFilters()">选择文件并导出</button>
    `);
    setTimeout(() => {
        const cb = $('wpeExportUsePassword');
        if (cb) cb.addEventListener('change', () => {
            const pg = $('wpeExportPasswordGroup');
            if (pg) pg.style.display = cb.checked ? 'block' : 'none';
        });
    }, 50);
}

function wpeExportSelectAll() {
    wpeSelectedIds.clear();
    wpeFilters.forEach(f => wpeSelectedIds.add(f.id));
    renderWPEFilters(wpeFilters);
    showExportDialog();
}
function wpeExportSelectNone() {
    wpeSelectedIds.clear();
    renderWPEFilters(wpeFilters);
    showExportDialog();
}
function wpeExportSelectEnabled() {
    wpeSelectedIds.clear();
    wpeFilters.filter(f => f.enabled).forEach(f => wpeSelectedIds.add(f.id));
    renderWPEFilters(wpeFilters);
    showExportDialog();
}

function doExportFilters() {
    const ids = wpeSelectedIds.size > 0 ? [...wpeSelectedIds] : [];
    if (ids.length === 0) {
        showToast('warning', '提示', '未选择任何滤镜');
        return;
    }
    const usePassword = $('wpeExportUsePassword')?.checked || false;
    const password = $('wpeExportPassword')?.value || '';
    if (usePassword && !password) {
        showToast('warning', '提示', '已启用密码保护但密码为空');
        return;
    }
    postAction('wpe_filter_export_file', { filterIds: ids, usePassword, password });
    closeModal();
}

function showImportDialog() {
    // 调用后端打开文件对话框
    postAction('wpe_filter_import_file');
}

function handleWpeImportOptions(data) {
    // 后端已读取文件，显示导入选项
    const encrypted = data.encrypted;
    const needPassword = data.needPassword;
    const filePath = data.filePath || '';

    let statusHtml = '';
    if (!encrypted) {
        statusHtml = '<p style="color:#FFE066;">提示：未检测到加密头，将按旧JSON格式导入</p>';
    } else {
        statusHtml = '<p style="color:#99D6FF;">检测到加密文件：AES-256-CBC</p>';
    }

    showModal('导入滤镜(解密)', `
        <p>文件: ${filePath || '-'}</p>
        ${statusHtml}
        <div class="form-group" style="margin-top:12px;">
            <label>导入模式:</label>
            <div style="padding-left:20px; margin-top:4px;">
                <label style="display:block; margin-bottom:4px;">
                    <input type="radio" name="wpeImportMode" value="merge" checked> 合并追加 (重新分配ID，执行次数清零)
                </label>
                <label style="display:block;">
                    <input type="radio" name="wpeImportMode" value="overwrite"> 覆盖导入 (保留ID和执行次数)
                </label>
            </div>
        </div>
        ${(encrypted && needPassword) ? `
        <div class="form-group" style="margin-top:8px;">
            <label for="wpeImportPassword">密码</label>
            <input type="password" id="wpeImportPassword" class="input" placeholder="请输入解密密码">
        </div>
        ` : ''}
    `, `
        <button class="btn btn-ghost" onclick="closeModal()">取消</button>
        <button class="btn btn-primary" onclick="doImportFilters()">开始导入</button>
    `);
}

function doImportFilters() {
    const modeRadio = document.querySelector('input[name="wpeImportMode"]:checked');
    const mode = modeRadio ? modeRadio.value : 'merge';
    const password = $('wpeImportPassword')?.value || '';
    postAction('wpe_filter_import_confirm', { mode, password });
    closeModal();
}

function clearFilterStats() {
    showConfirmDialog('确认清空', '确定要清空所有滤镜的执行统计吗？', function() {
        postAction('wpe_filter_clear_stats');
    });
}

function backToWPEFilters() {
    const editPage = document.getElementById('page-wpe-edit');
    const wpePage = document.getElementById('page-wpe');
    if (editPage) editPage.classList.remove('active');
    if (wpePage) wpePage.classList.add('active');

    currentMainPage = 'wpe';
    if (wpePage) lastActivePage = wpePage;
    const wpeNav = navItemsByPage.get('wpe');
    if (wpeNav) {
        if (lastActiveNav && lastActiveNav !== wpeNav) lastActiveNav.classList.remove('active');
        wpeNav.classList.add('active');
        lastActiveNav = wpeNav;
    }
    postAction('get_wpe_filters');
}

function toggleWpeFilterEnabled(filterId) {
    const filter = wpeFilters.find(f => f.id === filterId);
    if (filter) {
        postAction('wpe_filter_toggle', { filterId, enabled: !filter.enabled });
    }
}

function toggleWpeFilterSelect(filterId) {
    if (wpeSelectedIds.has(filterId)) {
        wpeSelectedIds.delete(filterId);
    } else {
        wpeSelectedIds.add(filterId);
    }
    const cb = document.getElementById(`wpeSelect_${filterId}`);
    if (cb) cb.checked = wpeSelectedIds.has(filterId);
}

function buildWpeFilterRowKey(filter) {
    return [
        filter.id,
        filter.name,
        filter.enabled ? 1 : 0,
        wpeSelectedIds.has(filter.id) ? 1 : 0,
        filter.mode || '',
        filter.action || '',
        filter.direction || '',
        filter.executionCount || 0,
        filter.global ? 1 : 0,
        Array.isArray(filter.targetInstances) ? filter.targetInstances.join(',') : ''
    ].join(',');
}

function buildWpeFilterRowHtml(filter) {
    const isSelected = wpeSelectedIds.has(filter.id);
    const modeText = WPE_MODE_NAMES[filter.mode] || filter.mode || '普通';
    const actionText = WPE_ACTION_NAMES[filter.action] || filter.action || '替换';
    const dirText = getDirectionText(filter.direction);
    const globalText = filter.global ? '是' : '否';
    let targetText = '未指定';
    if (filter.global) {
        targetText = '全部实例';
    } else if (filter.targetInstances && filter.targetInstances.length > 0) {
        targetText = filter.targetInstances.join(', ');
    }

    return `
        <tr ondblclick="wpeIsNewFilter=false; postAction('wpe_filter_get', {filterId: ${filter.id}})" style="cursor:pointer;">
            <td><input type="checkbox" ${filter.enabled ? 'checked' : ''} onchange="toggleWpeFilterEnabled(${filter.id})"></td>
            <td><input type="checkbox" id="wpeSelect_${filter.id}" ${isSelected ? 'checked' : ''} onchange="toggleWpeFilterSelect(${filter.id})"></td>
            <td>${filter.id}</td>
            <td>${filter.name}</td>
            <td>${modeText}</td>
            <td>${actionText}</td>
            <td>${dirText}</td>
            <td>${filter.executionCount || 0}</td>
            <td>${globalText}</td>
            <td>${targetText}</td>
        </tr>`;
}

function syncWpeFilterRows(filters) {
    const tbody = $('wpeFilterBody');
    if (!tbody) return;

    const nextRowKeys = (filters || []).map(buildWpeFilterRowKey);
    const rows = tbody.rows;

    if (rows.length === nextRowKeys.length && lastWpeFilterRowKeys.length === nextRowKeys.length) {
        for (let i = 0; i < nextRowKeys.length; i++) {
            if (lastWpeFilterRowKeys[i] !== nextRowKeys[i]) {
                rows[i].outerHTML = buildWpeFilterRowHtml(filters[i]);
            }
        }
    } else {
        tbody.innerHTML = (filters || []).map(buildWpeFilterRowHtml).join('');
    }

    lastWpeFilterRowKeys = nextRowKeys;
}

function renderWPEFilters(filters) {
    wpeFilters = filters || [];
    const renderKey = `${wpeDataRevision}|${wpeFilters.length}|${wpeSelectedIds.size}`;

    if (!isMainPageActive('wpe')) {
        return;
    }
    if (renderKey === (window.__lastWpeFiltersRenderKey || '')) {
        return;
    }
    window.__lastWpeFiltersRenderKey = renderKey;

    const enabledCount = wpeFilters.filter(f => f.enabled).length;
    const totalExec = wpeFilters.reduce((sum, f) => sum + (f.executionCount || 0), 0);
    const statusEl = $('wpeFilterStatus');
    if (statusEl) {
        statusEl.textContent = `已启用: ${enabledCount} | 总数: ${wpeFilters.length} | 总执行次数: ${totalExec}`;
    }

    const tbody = $('wpeFilterBody');
    if (!tbody) return;

    if (wpeFilters.length === 0) {
        lastWpeFilterRowKeys = [];
        tbody.innerHTML = '<tr><td colspan="10" style="text-align:center; color:var(--text-muted); padding:30px;">暂无滤镜，点击"新建滤镜"开始</td></tr>';
        return;
    }

    syncWpeFilterRows(wpeFilters);
}

function editFilter(filterId) {
    wpeIsNewFilter = false;
    postAction('wpe_filter_get', { filterId });
}

function deleteFilter(filterId) {
    if (confirm('确定要删除此滤镜吗？')) {
        postAction('wpe_filter_delete', { filterId });
    }
}

// ========== Logs Page (日志) ==========
function clearLogs() {
    if (confirm('确定要清空所有日志吗？')) {
        postAction('log_clear');
    }
}

function refreshLogs() {
    postAction('get_logs');
}

let logFilterDebounceTimer = null;

function scheduleFilterLogs() {
    if (logFilterDebounceTimer) {
        clearTimeout(logFilterDebounceTimer);
    }
    logFilterDebounceTimer = setTimeout(() => {
        logFilterDebounceTimer = null;
        filterLogs();
    }, 150);
}

function copyLogs() {
    const level = $('logLevelFilter').value;
    const category = $('logCategoryFilter').value;
    const searchText = ($('logSearchText').value || '').toLowerCase();

    let filtered = allLogs;

    if (level !== 'all') {
        filtered = filtered.filter(log => log.level === level || (level === 'WARNING' && log.level === 'WARN'));
    }

    if (category !== 'all') {
        filtered = filtered.filter(log => log.category === category);
    }

    if (searchText) {
        filtered = filtered.filter(log =>
            (log.message || '').toLowerCase().includes(searchText) ||
            (log.category || '').toLowerCase().includes(searchText)
        );
    }

    if (filtered.length === 0) {
        showToast('warning', '提示', '没有可复制的日志');
        return;
    }

    const logText = filtered.map(log =>
        `[${log.timestamp}] [${log.level}] [${log.category || '系统'}] ${log.message}`
    ).join('\n');

    // 使用传统方法复制到剪贴板
    const textarea = document.createElement('textarea');
    textarea.value = logText;
    textarea.style.position = 'fixed';
    textarea.style.opacity = '0';
    document.body.appendChild(textarea);
    textarea.select();

    try {
        const successful = document.execCommand('copy');
        if (successful) {
            showToast('success', '成功', `已复制 ${filtered.length} 条日志到剪贴板`);
        } else {
            showToast('error', '错误', '复制失败');
        }
    } catch (err) {
        showToast('error', '错误', '复制失败: ' + err.message);
    } finally {
        document.body.removeChild(textarea);
    }
}

function renderLogs(logs) {
    allLogs = logs || [];
    updateLogsDisabledNotice();

    const renderKey = `${allLogs.length}|${allLogs[0]?.timestamp || ''}|${allLogs[0]?.level || ''}|${allLogs[allLogs.length - 1]?.timestamp || ''}|${allLogs[allLogs.length - 1]?.message || ''}`;
    if (renderKey !== lastLogsDataKey) {
        lastLogsDataKey = renderKey;
        lastLogsDataRevision++;
    }

    if (!isMainPageActive('logs')) {
        return;
    }
    if (renderKey === lastLogsRenderKey) {
        return;
    }
    lastLogsRenderKey = renderKey;

    updateLogStats();
    filterLogs();
}

function updateLogStats() {
    let errorCount = 0;
    let warningCount = 0;
    let infoCount = 0;

    for (const log of allLogs) {
        if (log.level === 'ERROR') {
            errorCount++;
        } else if (log.level === 'WARNING' || log.level === 'WARN') {
            warningCount++;
        } else if (log.level === 'INFO') {
            infoCount++;
        }
    }

    $('logStatTotal').textContent = allLogs.length;
    $('logStatError').textContent = errorCount;
    $('logStatWarning').textContent = warningCount;
    $('logStatInfo').textContent = infoCount;
}

function escapeHtml(text) {
    return String(text ?? '')
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
}

// Store all logs for filtering
let allLogs = [];
let logsRealtimeSubscribed = false;

function setLogsRealtimeEnabled(enabled) {
    const normalized = enabled === true;
    if (logsRealtimeSubscribed === normalized) {
        return;
    }
    logsRealtimeSubscribed = normalized;
    postAction('set_logs_realtime', { enabled: normalized });
}

function appendLogs(logs) {
    if (!Array.isArray(logs) || logs.length === 0) {
        return;
    }
    renderLogs(allLogs.concat(logs));
}

function buildLogRowKey(log) {
    return [
        log.timestamp || '',
        log.level || '',
        log.category || '',
        log.message || ''
    ].join(',');
}

function buildLogRowHtml(log, levelBadgeClass, levelText) {
    return `
        <tr>
            <td style="width: 160px; color: var(--text-muted); font-size: 12px;">${escapeHtml(log.timestamp || '')}</td>
            <td style="width: 80px;">
                <span class="log-level-badge ${levelBadgeClass[log.level] || 'log-level-info'}">
                    ${levelText[log.level] || log.level}
                </span>
            </td>
            <td style="width: 120px;">
                <span class="log-category-badge">${escapeHtml(log.category || '系统')}</span>
            </td>
            <td style="color: var(--text);">${escapeHtml(log.message || '')}</td>
        </tr>
    `;
}

function syncLogTableRows(filtered, levelBadgeClass, levelText) {
    const tbody = $('logTableBody');
    if (!tbody) return;
    const rows = tbody.rows;

    if (rows.length === filtered.length && lastFilteredLogRowKeys.length === filtered.length) {
        const nextRowKeys = new Array(filtered.length);
        for (let i = 0; i < filtered.length; i++) {
            const rowKey = buildLogRowKey(filtered[i]);
            nextRowKeys[i] = rowKey;
            if (lastFilteredLogRowKeys[i] !== rowKey) {
                rows[i].outerHTML = buildLogRowHtml(filtered[i], levelBadgeClass, levelText);
            }
        }
        lastFilteredLogRowKeys = nextRowKeys;
    } else {
        tbody.innerHTML = filtered.map(log => buildLogRowHtml(log, levelBadgeClass, levelText)).join('');
        lastFilteredLogRowKeys = filtered.map(buildLogRowKey);
    }
}

function filterLogs() {
    const level = $('logLevelFilter').value;
    const category = $('logCategoryFilter').value;
    const searchText = ($('logSearchText').value || '').toLowerCase();

    let filtered = allLogs;

    if (level !== 'all') {
        filtered = filtered.filter(log => log.level === level || (level === 'WARNING' && log.level === 'WARN'));
    }

    if (category !== 'all') {
        filtered = filtered.filter(log => log.category === category);
    }

    if (searchText) {
        filtered = filtered.filter(log =>
            (log.message || '').toLowerCase().includes(searchText) ||
            (log.category || '').toLowerCase().includes(searchText)
        );
    }

    const tbody = $('logTableBody');
    const container = $('logList');
    if (!tbody || !container) {
        return;
    }

    const firstRowKey = filtered.length > 0 ? buildLogRowKey(filtered[0]) : '';
    const lastRowKey = filtered.length > 0 ? buildLogRowKey(filtered[filtered.length - 1]) : '';
    const filteredKey = `${lastLogsDataRevision}|${level}|${category}|${searchText}|${filtered.length}|${firstRowKey}|${lastRowKey}`;
    if (filteredKey === lastFilteredLogsKey) {
        return;
    }
    lastFilteredLogsKey = filteredKey;

    if (filtered.length === 0) {
        lastFilteredLogRowKeys = [];
        let emptyMessage = '无匹配日志';
        if (allLogs.length === 0) {
            emptyMessage = isLoggingEnabled()
                ? '暂无日志'
                : '日志记录当前已关闭，请先关闭极速模式后再查看新日志';
        }
        tbody.innerHTML = `<tr><td colspan="4" class="empty-state">${emptyMessage}</td></tr>`;
        return;
    }

    const levelBadgeClass = {
        'ERROR': 'log-level-error',
        'WARNING': 'log-level-warning',
        'WARN': 'log-level-warning',
        'INFO': 'log-level-info'
    };

    const levelText = {
        'ERROR': '错误',
        'WARNING': '警告',
        'WARN': '警告',
        'INFO': '信息'
    };

    syncLogTableRows(filtered, levelBadgeClass, levelText);

    if ($('logAutoScroll').checked) {
        container.scrollTop = container.scrollHeight;
    }
}

// ========== WebView2 Message Listener ==========
const pendingBackendMessages = [];
let backendMessageFlushScheduled = false;
let backendUiReady = false;

function scheduleBackendMessageFlush() {
    if (backendMessageFlushScheduled) {
        return;
    }

    if (!backendUiReady) {
        return;
    }

    if (!PageVisibility.isVisible) {
        return;
    }

    backendMessageFlushScheduled = true;
    if (typeof window.requestAnimationFrame === 'function') {
        window.requestAnimationFrame(flushBackendMessages);
    } else {
        setTimeout(flushBackendMessages, 16);
    }
}

function flushBackendMessages() {
    backendMessageFlushScheduled = false;
    if (!backendUiReady) {
        return;
    }
    if (pendingBackendMessages.length === 0) {
        return;
    }

    if (!PageVisibility.isVisible) {
        return;
    }

    const batch = pendingBackendMessages.splice(0, pendingBackendMessages.length);
    batch.forEach(handleBackendMessage);

    if (pendingBackendMessages.length > 0) {
        scheduleBackendMessageFlush();
    }
}

function dispatchBackendPayload(payload) {
    if (Array.isArray(payload)) {
        payload.forEach(item => {
            if (item) {
                pendingBackendMessages.push(item);
            }
        });
    } else if (payload) {
        pendingBackendMessages.push(payload);
    }

    if (pendingBackendMessages.length > 0) {
        scheduleBackendMessageFlush();
    }
}

function handleBackendMessage(data) {
    if (!data || !data.type) return;

    try {
        switch (data.type) {
            case 'status':
                updateStatus(data);
                break;
            case 'message':
                showToast(data.messageType, data.title, data.message);
                break;
            case 'instances':
                instancesDataRevision++;
                markPageDataReady('instances');
                markPageDataReady('proxydata');
                renderInstances(data.instances);
                break;
            case 'instance_config':
                showInstanceConfig(data.instance);
                break;
            case 'socks5_pools':
                poolsDataRevision++;
                markPageDataReady('accounts');
                renderSocks5Pools(data.pools);
                break;
            case 'socks5_pool_accounts':
                accountPanelDataRevision++;
                markPageDataReady('accounts');
                updateCachedPoolAccountCount(data.poolId, Array.isArray(data.accounts) ? data.accounts.length : 0);
                lastPoolListRenderKey = '';
                renderSocks5Pools(cachedPools);
                renderAccountPanel(data.poolId, data.poolName, data.accounts, data.onlineDevicePolicy);
                break;
            case 'socks5_api_status': {
                const btn = $('btnToggleApi');
                const statusText = $('apiStatusText');
                if (btn && statusText) {
                    if (data.running) {
                        btn.textContent = '停止API服务';
                        btn.className = 'btn btn-danger';
                        statusText.innerHTML = '<span style="color:#0f0;">● 运行中 (端口: ' + data.port + ')</span>';
                    } else {
                        btn.textContent = '启动API服务';
                        btn.className = 'btn btn-primary';
                        statusText.innerHTML = '<span style="color:#f44;">● 已停止</span>';
                    }
                }
                break;
            }
            case 'online_stats':
                onlineDataRevision++;
                markPageDataReady('online');
                renderOnlineStats(data);
                break;
            case 'wpe_filters':
                wpeDataRevision++;
                markPageDataReady('wpe');
                renderWPEFilters(data.filters);
                renderWpeWebDisplayNameSettings(data.filters || []);
                break;
            case 'wpe_filter_detail':
                wpeIsNewFilter = false;
                showWPEFilterEditor(data.filter);
                break;
            case 'wpe_filter_groups':
                renderWpeFilterGroups(data.groups);
                break;
            case 'wpe_import_options':
                handleWpeImportOptions(data);
                break;
            case 'logs':
                markPageDataReady('logs');
                renderLogs(data.logs);
                break;
            case 'logs_append':
                markPageDataReady('logs');
                appendLogs(data.logs);
                break;
            case 'app_config':
                handleAppConfigData(data);
                break;
            case 'cloud_login_result':
                handleLoginResult(data);
                break;
            case 'cloud_kick_online_result':
                handleCloudKickOnlineResult(data);
                break;
            case 'cloud_update_prompt':
                showCloudUpdatePrompt(data);
                break;
            case 'cloud_register_result':
                handleRegisterResult(data);
                break;
            case 'cloud_renew_result':
                handleRenewResult(data);
                break;
            case 'home_recharge_result':
                handleHomeRechargeResult(data);
                break;
            case 'notice':
                handleNotice(data);
                break;
            case 'remote_browser_config':
                markPageDataReady('remote-browser');
                cachedRemoteBrowserConfig = data;
                updateRemoteBrowserPanel('', data);
                updateRemoteBrowserPanel('app', data);
                break;
            case 'anticc_config':
                markPageDataReady('anticc');
                cachedAntiCCConfig = data.config || null;
                updateAntiCCForm(data.config);
                break;
            case 'anticc_global_state':
                cachedAntiCCGlobalState = data.state || null;
                updateAntiCCGlobalState(data.state);
                break;
            case 'config_proxy':
                handleProxyConfigData(data);
                break;
            case 'config_thread':
                handleThreadConfigData(data);
                break;
            case 'config_auth':
                handleAuthConfigData(data);
                break;
            case 'config_packet':
                handlePacketConfigData(data);
                break;
            case 'config_sslproxy':
                handleSSLProxyConfigData(data);
                break;
            case 'config_localmap':
                handleLocalMapConfigData(data);
                break;
            case 'config_traffic':
                handleTrafficConfigData(data);
                break;
            case 'config_disconnect':
                handleDisconnectConfigData(data);
                break;
            case 'config_userfilter':
                handleUserFilterConfigData(data);
                break;
            case 'config_accountfilter':
                handleAccountFilterData(data);
                break;
            case 'proxydata_list':
                markPageDataReady('proxydata');
                handleProxyDataList(data);
                break;
            case 'proxydata_detail':
                handleProxyDataDetail(data);
                break;
        }
    } catch (error) {
        console.error('Failed to handle backend message:', data?.type, error, data);
    }
}

if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', event => {
        dispatchBackendPayload(event.data);
    });
}

function updateStatus(data) {
    const statusBadge = $('statusBadge');
    const statusText = $('statusText');

    // Sidebar elements
    const sidebarUsername = $('sidebarUsername');
    const sidebarExpire = $('sidebarExpire');
    const sidebarRemaining = $('sidebarRemaining');
    const sidebarInstances = $('sidebarInstances');

    const statusViewKey = [
        data.loggedIn ? 1 : 0,
        data.username || '',
        data.loginType || '',
        data.expireTime || '',
        data.remainingDays ?? '',
        data.runningInstances ?? '',
        data.totalInstances ?? ''
    ].join('|');
    if (statusViewKey === lastStatusViewKey) {
        return;
    }
    lastStatusViewKey = statusViewKey;

    statusBadge.className = 'status-badge';

    if (data.loggedIn) {
        window.__currentCloudLoggedIn = true;
        window.__currentCloudLoginType = data.loginType === 'card' ? 'card' : 'account';
        // Show main app, hide login screen
        showMainApp();

        statusBadge.classList.add('logged-in');
        statusText.textContent = '已登录';

        // Update sidebar user info
        if (sidebarUsername) sidebarUsername.textContent = data.username || '-';
        if (sidebarExpire) sidebarExpire.textContent = data.expireTime || '-';
        if (sidebarRemaining) sidebarRemaining.textContent = data.remainingDays !== undefined ? `${data.remainingDays}天` : '-';

        // Update home page user info
        $('homeUsername').textContent = `${window.__currentCloudLoginType === 'card' ? '卡密' : '账号'}: ${data.username}`;
        $('homeExpireInfo').textContent = `到期时间: ${data.expireTime} | 剩余: ${data.remainingDays}天`;
        const rechargeCard = $('rechargeCard');
        if (rechargeCard) {
            rechargeCard.style.display = window.__currentCloudLoginType === 'card' ? 'none' : 'block';
        }

        // Load app-level config once after login (fast mode, etc.)
        if (!window.__appConfigLoadedOnce) {
            window.__appConfigLoadedOnce = true;
            loadAppConfig();
        }
    } else {
        const wasLoggedIn = window.__currentCloudLoggedIn;
        window.__currentCloudLoggedIn = false;
        window.__currentCloudLoginType = 'account';
        showLoginScreen();

        statusText.textContent = '未登录';

        // Clear sidebar user info
        if (sidebarUsername) sidebarUsername.textContent = '-';
        if (sidebarExpire) sidebarExpire.textContent = '-';
        if (sidebarRemaining) sidebarRemaining.textContent = '-';
        const rechargeCard = $('rechargeCard');
        if (rechargeCard) {
            rechargeCard.style.display = 'block';
        }
        if (wasLoggedIn) {
            loadAppConfig();
        }
    }

    if (data.runningInstances !== undefined) {
        const instanceText = `${data.runningInstances}/${data.totalInstances}`;
        if (sidebarInstances) sidebarInstances.textContent = instanceText;
    }
}

function handleLoginResult(data) {
    setCloudLoginButtonsLoading(false, false);

    if (data.success) {
        window.__loginModeState = data.loginType === 'card' ? 'card' : 'account';
        window.__autoLoginAttempted = true;
        window.__pendingCloudLoginAttempt = null;
        window.__cloudLoginKickInProgressCid = 0;
        window.__lastMaxOnlineClients = [];
        closeModal();
        loadAppConfig();
        const preempted = !!data.preempted;
        const preemptedCount = Number(data.preemptedCount) || 0;
        const successTitle = preempted
            ? '抢占登录成功'
            : (data.loginType === 'card' ? '卡密登录成功' : '登录成功');
        const successMessage = preempted
            ? `欢迎回来，${data.username}。已自动抢占 ${preemptedCount > 0 ? preemptedCount : 1} 个旧会话`
            : `欢迎回来，${data.username}`;
        showToast('success', successTitle, successMessage);
        // Status update will trigger showMainApp()
    } else {
        if (data.maxOnline) {
            window.__lastMaxOnlineClients = Array.isArray(data.onlineClients) ? data.onlineClients : [];
            showMaxOnlineKickDialog(data);
        } else {
            window.__cloudLoginKickInProgressCid = 0;
            window.__lastMaxOnlineClients = [];
            closeModal();
            showToast('error', '登录失败', data.error || '未知错误');
        }
    }
}

function handleRegisterResult(data) {
    const btnRegister = $('btnRegisterSubmit');
    const btnText = $('registerBtnText');
    const messageArea = $('registerMessage');
    if (!btnRegister || !btnText || !messageArea) return;

    btnRegister.disabled = false;
    btnText.textContent = '注册';

    if (data.success) {
        messageArea.className = 'message-area success';
        messageArea.textContent = data.message || '注册成功！';
        // Clear form
        $('registerUsername').value = '';
        $('registerPassword').value = '';
        $('registerSuperPassword').value = '';
        $('registerCards').value = '';
    } else {
        messageArea.className = 'message-area error';
        messageArea.textContent = data.error || '注册失败';
    }
}

function handleRenewResult(data) {
    const btnRenew = $('btnRenewSubmit');
    const btnText = $('renewBtnText');
    const messageArea = $('renewMessage');
    if (!btnRenew || !btnText || !messageArea) return;

    btnRenew.disabled = false;
    btnText.textContent = '续费';

    if (data.success) {
        messageArea.className = 'message-area success';
        messageArea.textContent = data.message || '续费成功！';
        // Clear form
        $('renewUsername').value = '';
        $('renewPassword').value = '';
        $('renewCards').value = '';
    } else {
        messageArea.className = 'message-area error';
        messageArea.textContent = data.error || '续费失败';
    }
}

function handleHomeRechargeResult(data) {
    const btn = $('btnRecharge');
    const messageArea = $('rechargeMessage');
    if (!btn || !messageArea) return;

    btn.disabled = false;
    btn.textContent = '续费/充值';

    if (data.success) {
        messageArea.textContent = data.message || '充值成功';
        messageArea.className = 'message-area success';
        messageArea.style.display = 'block';
        $('rechargeCards').value = '';

        // Show toast notification
        showToast('success', '充值成功', data.message || '充值成功');
    } else {
        messageArea.textContent = data.error || '充值失败';
        messageArea.className = 'message-area error';
        messageArea.style.display = 'block';

        showToast('error', '充值失败', data.error || '充值失败');
    }
}

function handleNotice(data) {
    const noticeText = data?.notice || '';

    // Show on login screen
    const loginNoticeArea = $('loginNoticeArea');
    const loginNoticeContent = $('loginNoticeContent');
    const loginNoticeEmpty = $('loginNoticeEmpty');
    if (loginNoticeContent && loginNoticeArea) {
        loginNoticeContent.textContent = noticeText;
        loginNoticeArea.style.display = noticeText ? 'block' : 'none';
    }
    if (loginNoticeEmpty) {
        loginNoticeEmpty.style.display = noticeText ? 'none' : 'block';
    }

    // Show on home page
    const noticeCard = $('noticeCard');
    const noticeContent = $('noticeContent');
    if (noticeContent) {
        noticeContent.textContent = noticeText;
    }
    if (noticeCard) {
        noticeCard.style.display = noticeText ? 'block' : 'none';
    }
}

// ========== Instance Config Page Functions ==========
// currentConfigInstance is already declared at line 274
let currentConfigMenu = 'basic';
let currentConfigMenuRenderCache = {};
let currentDisconnectRules = [];

function resetCurrentConfigMenuCache() {
    currentConfigMenuRenderCache = {};
}

function getInstanceConfigMenuCacheKey(inst, menuId) {
    return [
        inst?.id || '',
        inst?.state || '',
        inst?.port || '',
        inst?.currentConnections || 0,
        menuId
    ].join('|');
}

function renderInstanceConfigMenu(inst, isRunning, menuId) {
    switch (menuId) {
        case 'basic':
            return renderBasicInfo(inst);
        case 'proxy':
            return renderProxyConfig(inst, isRunning);
        case 'thread':
            return renderThreadConfig(inst, isRunning);
        case 'auth':
            return renderAuthConfig(inst, isRunning);
        case 'packet':
            return renderPacketConfig(inst, isRunning);
        case 'sslproxy':
            return renderSSLProxyConfig(inst, isRunning);
        case 'localmap':
            return renderLocalMapConfig(inst, isRunning);
        case 'traffic':
            return renderTrafficConfig(inst, isRunning);
        case 'disconnect':
            return renderDisconnectConfig(inst, isRunning);
        case 'userfilter':
            return renderUserFilterConfig(inst, isRunning);
        case 'accountfilter':
            return renderAccountFilterConfig(inst, isRunning);
        default:
            return '<p>未选择菜单</p>';
    }
}

function requestInstanceConfigMenuData(menuId, instId) {
    switch (menuId) {
        case 'proxy':
            loadProxyConfig(instId);
            break;
        case 'thread':
            loadThreadConfig(instId);
            break;
        case 'auth':
            loadAuthConfig(instId);
            break;
        case 'packet':
            loadPacketConfig(instId);
            break;
        case 'sslproxy':
            loadSSLProxyConfig(instId);
            break;
        case 'localmap':
            loadLocalMapConfig(instId);
            break;
        case 'traffic':
            loadTrafficConfig(instId);
            break;
        case 'disconnect':
            loadDisconnectConfig(instId);
            break;
        case 'userfilter':
            loadUserFilterConfig(instId);
            break;
        case 'accountfilter':
            loadAccountFilterConfig(instId);
            break;
    }
}

function showInstanceConfigPage(inst) {
    currentConfigInstance = inst;
    currentConfigMenu = 'basic';
    resetCurrentConfigMenuCache();

    $('configInstanceTitle').textContent = `配置实例: ${inst.name}`;
    $('configInstanceSubtitle').textContent = `实例ID: ${inst.id} | 类型: Socks转发`;

    navigateTo('instance-config');
    switchConfigMenu('basic', { force: true });
}

function backToInstances() {
    navigateTo('instances');
    postAction('get_instances');
}

function switchConfigMenu(menuId, options = {}) {
    currentConfigMenu = menuId;

    document.querySelectorAll('.menu-item').forEach(item => {
        item.classList.remove('active');
    });
    const menuItem = document.querySelector(`.menu-item[data-menu="${menuId}"]`);
    if (menuItem) menuItem.classList.add('active');

    const contentDiv = $('configContent');
    if (!currentConfigInstance) {
        contentDiv.innerHTML = '<p>未选择实例</p>';
        return;
    }

    const inst = currentConfigInstance;
    const isRunning = inst.state === 'Running';
    const renderKey = getInstanceConfigMenuCacheKey(inst, menuId);

    let html = !options.force ? currentConfigMenuRenderCache[renderKey] : '';
    if (!html) {
        html = renderInstanceConfigMenu(inst, isRunning, menuId);
        currentConfigMenuRenderCache[renderKey] = html;
    }

    if (options.force || contentDiv.dataset.renderKey !== renderKey) {
        contentDiv.innerHTML = html;
        contentDiv.dataset.renderKey = renderKey;
    }

    runAfterPagePaint(() => requestInstanceConfigMenuData(menuId, inst.id));
}

// Menu 0: Basic Info
function renderBasicInfo(inst) {
    const stateClass = inst.state === 'Running' ? 'status-running' :
                       inst.state === 'Stopped' ? 'status-stopped' : 'status-error';
    const stateText = inst.state === 'Running' ? '运行中' :
                      inst.state === 'Stopped' ? '已停止' : '错误';

    return `
        <h3>基本信息</h3>
        <div class="info-row">
            <span class="info-label">实例ID:</span>
            <span class="info-value">${inst.id}</span>
        </div>
        <div class="info-row">
            <span class="info-label">实例名称:</span>
            <span class="info-value">${inst.name}</span>
        </div>
        <div class="info-row">
            <span class="info-label">实例类型:</span>
            <span class="info-value">Socks转发 (支持WPE滤镜和二级代理)</span>
        </div>
        <div class="info-row">
            <span class="info-label">监听端口:</span>
            <span class="info-value">${inst.port}</span>
        </div>
        <div class="info-row">
            <span class="info-label">状态:</span>
            <span class="info-value ${stateClass}">${stateText}</span>
        </div>
        <div class="info-row">
            <span class="info-label">当前连接数:</span>
            <span class="info-value">${inst.currentConnections || 0}</span>
        </div>
        <div class="info-row">
            <span class="info-label">总连接数:</span>
            <span class="info-value">${inst.totalPackets || 0}</span>
        </div>
        <div class="info-row">
            <span class="info-label">总流量:</span>
            <span class="info-value">${((inst.totalBytes || 0) / 1024).toFixed(2)} KB</span>
        </div>
        <div class="info-hint">
            提示: WPE滤镜规则请在WPE滤镜页面中配置并指定生效实例
        </div>
    `;
}

// Menu 1: Secondary Proxy
function renderProxyConfig(inst, isRunning) {
    return `
        <h3>二级代理配置</h3>
        <div class="checkbox-group">
            <input type="checkbox" id="enableSecondaryProxy">
            <label for="enableSecondaryProxy">启用二级代理</label>
        </div>
        <div id="proxyFields">
            <div class="form-group">
                <label>代理地址</label>
                <input type="text" id="secondaryProxyHost" class="input" placeholder="127.0.0.1">
            </div>
            <div class="form-group">
                <label>代理端口</label>
                <input type="number" id="secondaryProxyPort" class="input" placeholder="1080">
            </div>
            <div class="form-group">
                <label>用户名</label>
                <input type="text" id="secondaryProxyUsername" class="input" placeholder="留空表示无需认证">
            </div>
            <div class="form-group">
                <label>密码</label>
                <input type="password" id="secondaryProxyPassword" class="input" placeholder="留空表示无需认证">
            </div>
            <button class="btn btn-primary" onclick="saveProxyConfig()">保存配置</button>
        </div>
    `;
}

function loadProxyConfig(instanceId) {
    postAction('config_get_proxy', { instanceId });
}

function handleProxyConfigData(data) {
    if ($('enableSecondaryProxy')) {
        $('enableSecondaryProxy').checked = data.enabled === true;
        $('secondaryProxyHost').value = data.host || '127.0.0.1';
        $('secondaryProxyPort').value = data.port || 1080;
        $('secondaryProxyUsername').value = data.username || '';
        $('secondaryProxyPassword').value = data.password || '';

        // Toggle fields based on checkbox (but keep save button enabled)
        const fields = $('proxyFields');
        const checkbox = $('enableSecondaryProxy');
        const updateFields = () => {
            const inputs = fields.querySelectorAll('input');
            inputs.forEach(input => {
                if (input.id !== 'enableSecondaryProxy') {
                    input.disabled = !checkbox.checked;
                }
            });
            // 保存按钮始终可用，以便用户可以保存"禁用二级代理"的配置
        };
        if (!checkbox._listenerAdded) {
            checkbox._listenerAdded = true;
            checkbox.addEventListener('change', updateFields);
        }
        updateFields();
    }
}

function saveProxyConfig() {
    const config = {
        instanceId: currentConfigInstance.id,
        enabled: $('enableSecondaryProxy').checked,
        host: $('secondaryProxyHost').value,
        port: parseInt($('secondaryProxyPort').value) || 1080,
        username: $('secondaryProxyUsername').value,
        password: $('secondaryProxyPassword').value
    };
    postAction('config_set_proxy', config);
}

// Menu 2: Thread Model
function renderThreadConfig(inst, isRunning) {
    const disabledAttr = isRunning ? 'disabled' : '';
    const warningText = isRunning ? '<span class="warning-text">运行中无法更改</span>' : '';

    return `
        <h3>线程模型配置</h3>
        <div class="form-group">
            <label>线程模型</label>
            <select id="threadPoolMode" class="input" ${disabledAttr}>
                <option value="0">传统模式（每连接一个线程）</option>
                <option value="1">阻塞式线程池</option>
                <option value="2">IOCP高性能模式</option>
            </select>
            ${warningText}
        </div>
        <button class="btn btn-primary" onclick="applyThreadMode()" ${disabledAttr}>应用线程模型</button>

        <div id="threadModeParams"></div>
    `;
}

function loadThreadConfig(instanceId) {
    postAction('config_get_thread', { instanceId });
}

function handleThreadConfigData(data) {
    const isRunning = currentConfigInstance && currentConfigInstance.state === 'Running';
    if ($('threadPoolMode')) {
        $('threadPoolMode').value = data.mode || 0;
        const sel = $('threadPoolMode');
        if (!sel._listenerAdded) {
            sel._listenerAdded = true;
            sel.addEventListener('change', () => {
                updateThreadModeParams(parseInt(sel.value), isRunning);
            });
        }
        updateThreadModeParams(data.mode || 0, isRunning);

        // Fill in existing values
        if (data.whitelistPoolSize && $('whitelistPoolSize'))
            $('whitelistPoolSize').value = data.whitelistPoolSize;
        if (data.normalPoolSize && $('normalPoolSize'))
            $('normalPoolSize').value = data.normalPoolSize;
        if (data.iocpMaxWhitelist && $('iocpMaxWhitelist'))
            $('iocpMaxWhitelist').value = data.iocpMaxWhitelist;
        if (data.iocpMaxNormal && $('iocpMaxNormal'))
            $('iocpMaxNormal').value = data.iocpMaxNormal;
    }
}

function applyThreadMode() {
    const mode = parseInt($('threadPoolMode').value);
    postAction('config_set_thread_mode', {
        instanceId: currentConfigInstance.id,
        mode
    });
}

function updateThreadModeParams(mode, isRunning) {
    const paramsDiv = $('threadModeParams');
    const disabledAttr = isRunning ? 'disabled' : '';

    if (mode === 0) {
        paramsDiv.innerHTML = `
            <div class="mode-description">
                <h4>传统模式特性:</h4>
                <ul>
                    <li>每个连接创建独立线程</li>
                    <li>适合连接数较少的场景（< 100）</li>
                    <li>实现简单，调试方便</li>
                    <li>大量连接时会消耗较多系统资源</li>
                </ul>
            </div>
        `;
    } else if (mode === 1) {
        paramsDiv.innerHTML = `
            <div class="mode-description">
                <h4>阻塞式线程池参数:</h4>
                <div class="form-group">
                    <label>白名单线程数</label>
                    <input type="number" id="whitelistPoolSize" class="input" value="10" ${disabledAttr}>
                </div>
                <div class="form-group">
                    <label>普通线程数</label>
                    <input type="number" id="normalPoolSize" class="input" value="50" ${disabledAttr}>
                </div>
                <button class="btn btn-primary" onclick="applyPoolSize()" ${disabledAttr}>应用线程池大小</button>
                <div id="poolStats"></div>
            </div>
        `;
    } else if (mode === 2) {
        paramsDiv.innerHTML = `
            <div class="mode-description">
                <h4>IOCP高性能模式参数:</h4>
                <div class="form-group">
                    <label>白名单最大连接</label>
                    <input type="number" id="iocpMaxWhitelist" class="input" value="2000" ${disabledAttr}>
                </div>
                <div class="form-group">
                    <label>普通最大连接</label>
                    <input type="number" id="iocpMaxNormal" class="input" value="500" ${disabledAttr}>
                </div>
                <button class="btn btn-primary" onclick="applyIocpMax()" ${disabledAttr}>应用IOCP连接上限</button>
                <div id="iocpStats"></div>
                <ul style="margin-top: 16px;">
                    <li>异步I/O，高并发性能</li>
                    <li>支持数千个并发连接</li>
                    <li>自动处理长连接保活</li>
                    <li>二级代理连接自动超时控制</li>
                </ul>
            </div>
        `;
    }
}

function applyPoolSize() {
    postAction('config_set_pool_size', {
        instanceId: currentConfigInstance.id,
        whitelistPoolSize: parseInt($('whitelistPoolSize').value) || 10,
        normalPoolSize: parseInt($('normalPoolSize').value) || 50
    });
}

function applyIocpMax() {
    postAction('config_set_iocp_max', {
        instanceId: currentConfigInstance.id,
        iocpMaxWhitelist: parseInt($('iocpMaxWhitelist').value) || 2000,
        iocpMaxNormal: parseInt($('iocpMaxNormal').value) || 500
    });
}

// Menu 3: SOCKS5 Auth
function renderAuthConfig(inst, isRunning) {
    return `
        <h3>SOCKS5认证配置</h3>
        <div class="checkbox-group">
            <input type="checkbox" id="enableSocks5Auth">
            <label for="enableSocks5Auth">启用SOCKS5认证</label>
        </div>
        <div class="form-group">
            <label>选择账号实例</label>
            <select id="socks5PoolSelect" class="input">
                <option value="">请选择账号实例</option>
            </select>
        </div>
        <div id="authStatus"></div>
    `;
}

function loadAuthConfig(instanceId) {
    postAction('config_get_auth', { instanceId });
    postAction('get_socks5_pools'); // Load available pools
}

function handleAuthConfigData(data) {
    if ($('enableSocks5Auth')) {
        $('enableSocks5Auth').checked = data.enabled === true;

        // Populate pool select
        const select = $('socks5PoolSelect');
        if (select) {
            select.innerHTML = '<option value="">请选择账号实例</option>';
        }
        if (data.pools && data.pools.length > 0) {
            data.pools.forEach(pool => {
                const opt = document.createElement('option');
                opt.value = pool.id;
                opt.textContent = `${pool.id} (${pool.name})`;
                if (pool.id === data.selectedPoolId) opt.selected = true;
                select.appendChild(opt);
            });
        }

        // Add event listeners (prevent duplicate)
        const authCb = $('enableSocks5Auth');
        if (!authCb._listenerAdded) {
            authCb._listenerAdded = true;
            authCb.addEventListener('change', () => {
                postAction('config_set_auth_enabled', {
                    instanceId: currentConfigInstance.id,
                    enabled: authCb.checked
                });
            });
        }

        if (!select._listenerAdded) {
            select._listenerAdded = true;
            select.addEventListener('change', () => {
                postAction('config_set_auth_pool', {
                    instanceId: currentConfigInstance.id,
                    poolId: select.value
                });
            });
        }

        updateAuthStatus(data.enabled, data.selectedPoolId, data.accountCount);
    }
}

function updateAuthStatus(enabled, poolId, accountCount) {
    const statusDiv = $('authStatus');
    if (!statusDiv) return;
    if (enabled && poolId) {
        statusDiv.innerHTML = `
            <div class="info-hint" style="border-left-color: var(--success);">
                认证已启用<br>
                绑定账号库: ${poolId}<br>
                可用账号数: ${accountCount || 0}<br>
                在线设备策略请到 SOCKS5 账号库页面配置
            </div>
        `;
    } else if (enabled && !poolId) {
        statusDiv.innerHTML = `
            <div class="info-hint" style="border-left-color: var(--warning);">
                请选择账号实例<br>
                在线设备策略请到 SOCKS5 账号库页面配置
            </div>
        `;
    } else {
        statusDiv.innerHTML = `
            <div class="info-hint">
                认证已禁用
            </div>
        `;
    }
}

// Menu 4: Packet Handling
function renderPacketConfig(inst, isRunning) {
    return `
        <h3>分包处理配置</h3>
        <div class="checkbox-group">
            <input type="checkbox" id="enablePacketSplit">
            <label for="enablePacketSplit">启用分包处理</label>
        </div>
        <div class="info-hint">
            说明: 分包处理用于解析游戏协议数据包，启用后WPE滤镜和触发器才能正常工作。
        </div>
        <div class="checkbox-group">
            <input type="checkbox" id="applyWpeOnNonSplit">
            <label for="applyWpeOnNonSplit">对不分包流量也应用WPE滤镜</label>
        </div>
        <div class="info-hint">
            勾选后，即使不进行分包处理的流量（如HTTPS）也会应用WPE滤镜
        </div>

        <h4 class="section-title">生效的目标端口（仅对这些端口启用分包处理）</h4>
        <div id="portList" class="port-list"></div>
        <div class="form-row">
            <div class="form-group">
                <input type="number" id="newPort" class="input" placeholder="输入端口号">
            </div>
            <button class="btn btn-primary" onclick="addPort()">添加端口</button>
        </div>
        <div class="info-hint" style="border-left-color: var(--warning);">
            提示:<br>
            • 如果端口列表为空，则对所有端口都启用分包处理<br>
            • 如果端口列表不为空，则只对列表中的端口启用分包处理<br>
            • 对于HTTPS/HTTP等非游戏流量，建议不添加到列表中，以提高转发性能
        </div>
    `;
}

function loadPacketConfig(instanceId) {
    postAction('config_get_packet', { instanceId });
}

function handlePacketConfigData(data) {
    if ($('enablePacketSplit')) {
        $('enablePacketSplit').checked = data.enabled !== false; // default true
        $('applyWpeOnNonSplit').checked = data.applyWpeOnNonSplit === true;

        // Event listeners (prevent duplicate)
        const splitCb = $('enablePacketSplit');
        if (!splitCb._listenerAdded) {
            splitCb._listenerAdded = true;
            splitCb.addEventListener('change', () => {
                postAction('config_set_packet_split', {
                    instanceId: currentConfigInstance.id,
                    enabled: splitCb.checked
                });
            });
        }
        const wpeCb = $('applyWpeOnNonSplit');
        if (!wpeCb._listenerAdded) {
            wpeCb._listenerAdded = true;
            wpeCb.addEventListener('change', () => {
                postAction('config_set_wpe_non_split', {
                    instanceId: currentConfigInstance.id,
                    enabled: wpeCb.checked
                });
            });
        }

        // Populate port list
        updatePortList(data.ports || []);
    }
}

function addPort() {
    const port = parseInt($('newPort').value);
    if (port > 0 && port <= 65535) {
        postAction('config_add_port', {
            instanceId: currentConfigInstance.id,
            port
        });
        $('newPort').value = '';
    } else {
        showToast('error', '错误', '端口号必须在1-65535之间');
    }
}

function removePort(port) {
    postAction('config_remove_port', {
        instanceId: currentConfigInstance.id,
        port
    });
}

function updatePortList(ports) {
    const listDiv = $('portList');
    if (!ports || ports.length === 0) {
        listDiv.innerHTML = '<p style="color: var(--text-muted);">端口列表为空，对所有端口启用分包处理</p>';
        return;
    }

    listDiv.innerHTML = ports.map(port => `
        <div class="port-item">
            <span class="port-number">${port}</span>
            <button class="btn btn-danger" onclick="removePort(${port})">删除</button>
        </div>
    `).join('');
}

// Menu 5: Traffic Filter
function renderTrafficConfig(inst, isRunning) {
    return `
        <h3>流量过滤配置</h3>
        <div class="checkbox-group">
            <input type="checkbox" id="enableTrafficFilter">
            <label for="enableTrafficFilter">启用流量过滤</label>
        </div>
        <div class="checkbox-group">
            <input type="checkbox" id="enableSniSniffing">
            <label for="enableSniSniffing">启用SNI嗅探</label>
        </div>
        <div class="info-hint" style="border-left-color: var(--warning);">
            说明:<br>
            • 启用流量过滤后，只有匹配规则的流量才能通过<br>
            • SNI嗅探用于识别HTTPS流量的真实域名（需配合'指定端口嗅探指定域名'规则使用）
        </div>

        <div class="toolbar">
            <button class="btn btn-primary" onclick="addTrafficRule()">添加规则</button>
            <button class="btn btn-ghost" onclick="editTrafficRule()">编辑</button>
            <button class="btn btn-danger" onclick="deleteTrafficRule()">删除</button>
            <button class="btn btn-warning" onclick="clearTrafficRules()">清空所有</button>
        </div>

        <h4 class="section-title">过滤规则列表</h4>
        <div id="trafficRuleList"></div>
    `;
}

function loadTrafficConfig(instanceId) {
    postAction('config_get_traffic', { instanceId });
}

function handleTrafficConfigData(data) {
    if ($('enableTrafficFilter')) {
        $('enableTrafficFilter').checked = data.enableTrafficFilter === true;
        $('enableSniSniffing').checked = data.enableSniSniffing === true;

        // Event listeners (prevent duplicate)
        const filterCb = $('enableTrafficFilter');
        if (!filterCb._listenerAdded) {
            filterCb._listenerAdded = true;
            filterCb.addEventListener('change', () => {
                postAction('config_set_traffic_filter', {
                    instanceId: currentConfigInstance.id,
                    enabled: filterCb.checked
                });
            });
        }
        const sniCb = $('enableSniSniffing');
        if (!sniCb._listenerAdded) {
            sniCb._listenerAdded = true;
            sniCb.addEventListener('change', () => {
                postAction('config_set_sni_sniffing', {
                    instanceId: currentConfigInstance.id,
                    enabled: sniCb.checked
                });
            });
        }

        // Render rules table
        renderTrafficRules(data.rules || []);
    }
}

function renderSSLProxyConfig(inst, isRunning) {
    return `
        <h3>SSL代理设置</h3>
        <div class="checkbox-group">
            <input type="checkbox" id="enableSSLMitm">
            <label for="enableSSLMitm">启用 SSL MITM 解密</label>
        </div>
        <div class="info-hint" style="border-left-color: var(--danger);">
            说明:<br>
            • 当前配置仅作用于这个 Socks 转发实例<br>
            • 仅会对命中 MITM 规则的 TLS 流量尝试解密<br>
            • 首次使用前请先导出并安装 CA 证书，否则客户端通常会报证书错误<br>
            • 点击导出后会直接写入程序目录下的 <code>ssl-mitm-ca.cer</code><br>
            • 域名规则支持精确域名或通配域名，例如 <code>*.example.com</code>
        </div>
        <div class="toolbar">
            <button class="btn btn-primary" onclick="exportSslMitmCA()">导出CA证书到程序目录</button>
            <button class="btn btn-primary" onclick="addSslMitmRule()">添加MITM规则</button>
            <button class="btn btn-danger" onclick="clearSslMitmRules()">清空MITM规则</button>
        </div>
        <h4 class="section-title">MITM规则列表</h4>
        <div id="sslMitmRuleList"></div>
    `;
}

function loadSSLProxyConfig(instanceId) {
    postAction('config_get_sslproxy', { instanceId });
}

function handleSSLProxyConfigData(data) {
    const sslMitmCb = $('enableSSLMitm');
    if (!sslMitmCb) return;

    sslMitmCb.checked = data.enableSSLMitm === true;
    currentSslMitmRules = Array.isArray(data.sslMitmRules) ? data.sslMitmRules.slice() : [];

    if (!sslMitmCb._listenerAdded) {
        sslMitmCb._listenerAdded = true;
        sslMitmCb.addEventListener('change', () => {
            postAction('config_set_ssl_mitm', {
                instanceId: currentConfigInstance.id,
                enabled: sslMitmCb.checked
            });
        });
    }

    renderSslMitmRules(currentSslMitmRules);
}

function renderLocalMapConfig(inst, isRunning) {
    return `
        <h3>本地映射设置</h3>
        <div class="checkbox-group">
            <input type="checkbox" id="enableHttpLocalMap">
            <label for="enableHttpLocalMap">启用本地映射</label>
        </div>
        <div class="info-hint" style="border-left-color: var(--accent);">
            说明:<br>
            • 当前配置仅作用于这个 Socks 转发实例<br>
            • 支持 HTTP 明文流量，以及启用 SSL MITM 后的 HTTPS 解密明文流量<br>
            • 命中规则后将直接使用本地文件响应，不再请求上游服务器<br>
            • 第一版建议优先用于 GET / HEAD 的静态资源映射
        </div>
        <div class="toolbar">
            <button class="btn btn-primary" onclick="addLocalMapRule()">添加映射规则</button>
            <button class="btn btn-danger" onclick="clearLocalMapRules()">清空映射规则</button>
        </div>
        <h4 class="section-title">映射规则列表</h4>
        <div id="httpLocalMapRuleList"></div>
    `;
}

function loadLocalMapConfig(instanceId) {
    postAction('config_get_localmap', { instanceId });
}

function handleLocalMapConfigData(data) {
    const enableCb = $('enableHttpLocalMap');
    if (!enableCb) return;

    enableCb.checked = data.enabled === true;
    currentHttpLocalMapRules = Array.isArray(data.rules) ? data.rules.slice() : [];

    if (!enableCb._listenerAdded) {
        enableCb._listenerAdded = true;
        enableCb.addEventListener('change', () => {
            postAction('config_set_localmap', {
                instanceId: currentConfigInstance.id,
                enabled: enableCb.checked
            });
        });
    }

    renderLocalMapRules(currentHttpLocalMapRules);
}

function normalizeLocalMapRule(rule) {
    return {
        id: Math.max(1, parseInt(rule?.id ?? Date.now(), 10) || Date.now()),
        enabled: rule?.enabled !== false,
        scheme: ((rule?.scheme || '*') + '').toLowerCase(),
        hostPattern: (rule?.hostPattern || '*').trim() || '*',
        pathPattern: (rule?.pathPattern || '/').trim() || '/',
        method: ((rule?.method || '*') + '').toUpperCase(),
        localFilePath: (rule?.localFilePath || '').trim(),
        contentType: (rule?.contentType || '').trim()
    };
}

function renderLocalMapRules(rules) {
    const container = $('httpLocalMapRuleList');
    if (!container) return;

    if (!rules || rules.length === 0) {
        container.innerHTML = '<p style="color: var(--text-muted);">暂无本地映射规则</p>';
        return;
    }

    container.innerHTML = `<table class="data-table">
        <thead>
            <tr>
                <th>启用</th>
                <th>协议</th>
                <th>方法</th>
                <th>Host</th>
                <th>Path</th>
                <th>本地文件</th>
                <th>操作</th>
            </tr>
        </thead>
        <tbody>
            ${(rules || []).map((rule, index) => {
                const normalized = normalizeLocalMapRule(rule);
                return `<tr>
                    <td>
                        <input type="checkbox" ${normalized.enabled ? 'checked' : ''}
                            onchange="toggleLocalMapRule(${index}, this.checked)">
                    </td>
                    <td>${escapeHtml(normalized.scheme)}</td>
                    <td>${escapeHtml(normalized.method)}</td>
                    <td>${escapeHtml(normalized.hostPattern)}</td>
                    <td>${escapeHtml(normalized.pathPattern)}</td>
                    <td style="max-width:320px; word-break:break-all;">${escapeHtml(normalized.localFilePath)}</td>
                    <td>
                        <button class="btn btn-danger" onclick="deleteLocalMapRule(${index})">删除</button>
                    </td>
                </tr>`;
            }).join('')}
        </tbody>
    </table>`;
}

function saveLocalMapRules() {
    const normalized = currentHttpLocalMapRules
        .map(normalizeLocalMapRule)
        .filter(rule => !!rule.localFilePath);

    postAction('config_set_localmap_rules', {
        instanceId: currentConfigInstance.id,
        rules: normalized
    });
}

function addLocalMapRule() {
    showModal('添加本地映射规则', `
        <div class="form-group">
            <label>协议</label>
            <select id="newLocalMapScheme" class="input">
                <option value="*">*</option>
                <option value="http">http</option>
                <option value="https">https</option>
            </select>
        </div>
        <div class="form-group">
            <label>请求方法</label>
            <select id="newLocalMapMethod" class="input">
                <option value="*">*</option>
                <option value="GET">GET</option>
                <option value="HEAD">HEAD</option>
            </select>
        </div>
        <div class="form-group">
            <label>Host 匹配</label>
            <input type="text" id="newLocalMapHost" class="input" placeholder="example.com 或 *.example.com">
        </div>
        <div class="form-group">
            <label>Path 匹配</label>
            <input type="text" id="newLocalMapPath" class="input" placeholder="/assets/app.js 或 /assets/*">
        </div>
        <div class="form-group">
            <label>本地文件路径</label>
            <input type="text" id="newLocalMapFile" class="input" placeholder="D:/static/app.js">
        </div>
        <div class="form-group">
            <label>Content-Type（可选）</label>
            <input type="text" id="newLocalMapContentType" class="input" placeholder="留空自动推断">
        </div>
    `, `
        <button class="btn btn-ghost" onclick="closeModal()">取消</button>
        <button class="btn btn-primary" onclick="confirmAddLocalMapRule()">确定</button>
    `);
}

function confirmAddLocalMapRule() {
    const hostPattern = $('newLocalMapHost').value.trim() || '*';
    const pathPattern = $('newLocalMapPath').value.trim() || '/';
    const localFilePath = $('newLocalMapFile').value.trim();
    const scheme = $('newLocalMapScheme').value;
    const method = $('newLocalMapMethod').value;
    const contentType = $('newLocalMapContentType').value.trim();

    if (!localFilePath) {
        showToast('error', '错误', '请输入本地文件路径');
        return;
    }

    const nextId = currentHttpLocalMapRules.reduce((maxId, rule) => {
        const id = parseInt(rule?.id ?? 0, 10) || 0;
        return Math.max(maxId, id);
    }, 0) + 1;

    currentHttpLocalMapRules.push({
        id: nextId,
        enabled: true,
        scheme,
        hostPattern,
        pathPattern,
        method,
        localFilePath,
        contentType
    });

    saveLocalMapRules();
    closeModal();
}

function toggleLocalMapRule(index, enabled) {
    if (index < 0 || index >= currentHttpLocalMapRules.length) return;
    currentHttpLocalMapRules[index].enabled = !!enabled;
    saveLocalMapRules();
}

function deleteLocalMapRule(index) {
    if (index < 0 || index >= currentHttpLocalMapRules.length) return;
    if (!confirm('确定要删除这条本地映射规则吗?')) return;

    currentHttpLocalMapRules.splice(index, 1);
    saveLocalMapRules();
}

function clearLocalMapRules() {
    if (!confirm('确定要清空所有本地映射规则吗?')) return;
    currentHttpLocalMapRules = [];
    saveLocalMapRules();
}

function buildTrafficRuleRowKey(rule) {
    return [
        rule.id,
        rule.enabled ? 1 : 0,
        rule.type,
        rule.value1 || '',
        rule.value2 || ''
    ].join(',');
}

function buildTrafficRuleRowHtml(rule) {
    const ruleTypeNames = ['端口匹配', '域名匹配', 'IP匹配', '端口+SNI域名'];
    return `<tr>
                <td>
                    <input type="checkbox" ${rule.enabled ? 'checked' : ''}
                        onchange="toggleTrafficRule(${rule.id}, this.checked)">
                </td>
                <td>${ruleTypeNames[rule.type] || '未知'}</td>
                <td>${rule.value1 || ''}</td>
                <td>${rule.type === 3 ? (rule.value2 || '') : '-'}</td>
                <td>${rule.id}</td>
                <td>
                    <button class="btn btn-ghost" onclick="editTrafficRuleById(${rule.id})">编辑</button>
                    <button class="btn btn-danger" onclick="deleteTrafficRuleById(${rule.id})">删除</button>
                </td>
            </tr>`;
}

function syncTrafficRuleRows(rules) {
    const tbody = $('trafficRuleBody');
    if (!tbody) return;

    const nextRowKeys = (rules || []).map(buildTrafficRuleRowKey);
    const rows = tbody.rows;

    if (rows.length === nextRowKeys.length && lastTrafficRuleRowKeys.length === nextRowKeys.length) {
        for (let i = 0; i < nextRowKeys.length; i++) {
            if (lastTrafficRuleRowKeys[i] !== nextRowKeys[i]) {
                rows[i].outerHTML = buildTrafficRuleRowHtml(rules[i]);
            }
        }
    } else {
        tbody.innerHTML = (rules || []).map(buildTrafficRuleRowHtml).join('');
    }

    lastTrafficRuleRowKeys = nextRowKeys;
}

function renderTrafficRules(rules) {
    const container = $('trafficRuleList');
    if (!container) return;

    if (!rules || rules.length === 0) {
        lastTrafficRuleRowKeys = [];
        container.innerHTML = '<p style="color: var(--text-muted);">暂无过滤规则</p>';
        return;
    }

    if (!$('trafficRuleBody')) {
        container.innerHTML = `<table class="data-table">
        <thead>
            <tr>
                <th>启用</th>
                <th>类型</th>
                <th>值</th>
                <th>SNI域名</th>
                <th>ID</th>
                <th>操作</th>
            </tr>
        </thead>
        <tbody id="trafficRuleBody"></tbody></table>`;
    }

    syncTrafficRuleRows(rules);
}

function toggleTrafficRule(ruleId, enabled) {
    postAction('config_toggle_traffic_rule', {
        instanceId: currentConfigInstance.id,
        ruleId, enabled
    });
}

function editTrafficRuleById(ruleId) {
    postAction('config_get_traffic_rule', {
        instanceId: currentConfigInstance.id,
        ruleId
    });
}

function deleteTrafficRuleById(ruleId) {
    if (confirm('确定要删除选中的规则吗?')) {
        postAction('config_delete_traffic_rule', {
            instanceId: currentConfigInstance.id,
            ruleId
        });
    }
}

function addTrafficRule() {
    showModal('添加流量过滤规则', `
        <div class="form-group">
            <label>规则类型</label>
            <select id="newRuleType" class="input">
                <option value="0">端口匹配</option>
                <option value="1">域名匹配</option>
                <option value="2">IP匹配</option>
                <option value="3">端口+SNI域名</option>
            </select>
        </div>
        <div class="form-group">
            <label>规则值</label>
            <input type="text" id="newRuleValue" class="input" placeholder="端口号/域名/IP">
        </div>
        <div class="form-group" id="sniDomainGroup" style="display:none;">
            <label>SNI域名</label>
            <input type="text" id="newRuleSniDomain" class="input" placeholder="example.com">
        </div>
    `, `
        <button class="btn btn-ghost" onclick="closeModal()">取消</button>
        <button class="btn btn-primary" onclick="confirmAddTrafficRule()">确定</button>
    `);

    $('newRuleType').addEventListener('change', (e) => {
        $('sniDomainGroup').style.display = e.target.value === '3' ? 'block' : 'none';
    });
}

function confirmAddTrafficRule() {
    const type = parseInt($('newRuleType').value);
    const value = $('newRuleValue').value.trim();
    const sniDomain = $('newRuleSniDomain')?.value.trim() || '';

    if (!value) {
        showToast('error', '错误', '请输入规则值');
        return;
    }

    postAction('config_add_traffic_rule', {
        instanceId: currentConfigInstance.id,
        type, value, sniDomain
    });
    closeModal();
}

function editTrafficRule() {
    showToast('info', '提示', '请先在列表中选择要编辑的规则');
}

function deleteTrafficRule() {
    showToast('info', '提示', '请先在列表中选择要删除的规则');
}

function clearTrafficRules() {
    if (confirm('确定要清空所有规则吗?')) {
        postAction('config_clear_traffic_rules', {
            instanceId: currentConfigInstance.id
        });
    }
}

function normalizeSslMitmRule(rule) {
    if (rule && rule.matchByPort === false) {
        return {
            matchByPort: false,
            port: 0,
            domain: (rule.domain || '').trim()
        };
    }

    return {
        matchByPort: true,
        port: Math.max(1, Math.min(65535, parseInt(rule?.port ?? 443, 10) || 443)),
        domain: ''
    };
}

function renderSslMitmRules(rules) {
    const container = $('sslMitmRuleList');
    if (!container) return;

    if (!rules || rules.length === 0) {
        container.innerHTML = '<p style="color: var(--text-muted);">暂无 SSL MITM 规则</p>';
        return;
    }

    container.innerHTML = `<table class="data-table">
        <thead>
            <tr>
                <th>类型</th>
                <th>值</th>
                <th>操作</th>
            </tr>
        </thead>
        <tbody>
            ${(rules || []).map((rule, index) => {
                const normalized = normalizeSslMitmRule(rule);
                const label = normalized.matchByPort ? '端口' : '域名';
                const value = normalized.matchByPort ? normalized.port : escapeHtml(normalized.domain || '');
                return `<tr>
                    <td>${label}</td>
                    <td>${value}</td>
                    <td>
                        <button class="btn btn-danger" onclick="deleteSslMitmRule(${index})">删除</button>
                    </td>
                </tr>`;
            }).join('')}
        </tbody>
    </table>`;
}

function saveSslMitmRules() {
    const normalized = currentSslMitmRules
        .map(normalizeSslMitmRule)
        .filter(rule => (rule.matchByPort && rule.port > 0) || (!rule.matchByPort && rule.domain));

    postAction('config_set_ssl_mitm_rules', {
        instanceId: currentConfigInstance.id,
        rules: normalized
    });
}

function exportSslMitmCA() {
    const inst = currentConfigInstance;
    if (!inst || inst.type !== 'SocksForward' || !inst.id) {
        showToast('error', '导出失败', '当前实例未加载，请重新打开该 Socks 转发实例配置页后再试');
        console.error('exportSslMitmCA aborted: invalid currentConfigInstance', inst);
        return;
    }

    showToast('info', '正在导出', 'CA 证书将直接导出到程序目录下的 ssl-mitm-ca.cer');
    setTimeout(() => {
        postAction('config_export_ssl_mitm_ca', {
            instanceId: inst.id
        });
    }, 0);
}

function addSslMitmRule() {
    showModal('添加 SSL MITM 规则', `
        <div class="form-group">
            <label>规则类型</label>
            <select id="newSslMitmRuleType" class="input">
                <option value="port">端口</option>
                <option value="domain">域名</option>
            </select>
        </div>
        <div class="form-group">
            <label id="newSslMitmRuleLabel">端口</label>
            <input type="text" id="newSslMitmRuleValue" class="input" placeholder="443">
        </div>
    `, `
        <button class="btn btn-ghost" onclick="closeModal()">取消</button>
        <button class="btn btn-primary" onclick="confirmAddSslMitmRule()">确定</button>
    `);

    $('newSslMitmRuleType').addEventListener('change', (e) => {
        const isPort = e.target.value === 'port';
        $('newSslMitmRuleLabel').textContent = isPort ? '端口' : '域名';
        $('newSslMitmRuleValue').placeholder = isPort ? '443' : '*.example.com';
    });
}

function confirmAddSslMitmRule() {
    const ruleType = $('newSslMitmRuleType').value;
    const rawValue = $('newSslMitmRuleValue').value.trim();

    if (!rawValue) {
        showToast('error', '错误', '请输入 MITM 规则值');
        return;
    }

    if (ruleType === 'port') {
        const port = Math.max(1, Math.min(65535, parseInt(rawValue, 10) || 0));
        if (!port) {
            showToast('error', '错误', '请输入有效端口');
            return;
        }
        currentSslMitmRules.push({ matchByPort: true, port, domain: '' });
    } else {
        currentSslMitmRules.push({ matchByPort: false, port: 0, domain: rawValue });
    }

    saveSslMitmRules();
    closeModal();
}

function deleteSslMitmRule(index) {
    if (index < 0 || index >= currentSslMitmRules.length) return;
    if (!confirm('确定要删除这条 SSL MITM 规则吗?')) return;

    currentSslMitmRules.splice(index, 1);
    saveSslMitmRules();
}

function clearSslMitmRules() {
    if (!confirm('确定要清空所有 SSL MITM 规则吗?')) return;
    currentSslMitmRules = [];
    saveSslMitmRules();
}

// Menu 6: Disconnect Rules
function normalizeDisconnectRule(rule) {
    return {
        targetPort: Math.max(1, Math.min(65535, parseInt(rule?.targetPort ?? 1, 10) || 1)),
        mode: parseInt(rule?.mode ?? 0, 10) === 1 ? 1 : 0,
        disconnectIntervalSec: Math.max(0, parseInt(rule?.disconnectIntervalSec ?? 0, 10) || 0),
        enabled: rule?.enabled !== false,
        hexPattern: (rule?.hexPattern || '').trim(),
        hexDelayEnabled: rule?.hexDelayEnabled === true,
        hexDelaySeconds: Math.max(0, parseInt(rule?.hexDelaySeconds ?? 0, 10) || 0)
    };
}

function sortDisconnectRules(rules) {
    rules.sort((left, right) => {
        const leftWeight = left.targetPort === 0 ? 1 : 0;
        const rightWeight = right.targetPort === 0 ? 1 : 0;
        if (leftWeight !== rightWeight) {
            return leftWeight - rightWeight;
        }
        return left.targetPort - right.targetPort;
    });
}

function renderDisconnectConfig(inst, isRunning) {
    return `
        <h3>断网规则</h3>
        <div class="info-hint" style="border-left-color: var(--danger);">
            说明:<br>
            • 先按目标端口精确匹配连接，再执行断网逻辑<br>
            • 每条规则只能选择一种模式：定时断开 或 Hex 断开<br>
            • Hex 模式只会处理该目标端口的连接流量<br>
            • 同一 IP:端口 的并发多连接会逐条独立判定，不会串切
        </div>
        <div class="toolbar disconnect-toolbar">
            <button class="btn btn-primary" onclick="addDisconnectRule()">添加规则</button>
            <button class="btn btn-warning" onclick="clearDisconnectRules()">清空所有</button>
        </div>
        <div id="disconnectRuleList"></div>
    `;
}

function loadDisconnectConfig(instanceId) {
    postAction('config_get_disconnect', { instanceId });
}

function handleDisconnectConfigData(data) {
    currentDisconnectRules = Array.isArray(data?.rules)
        ? data.rules.map(normalizeDisconnectRule)
        : [];
    sortDisconnectRules(currentDisconnectRules);
    renderDisconnectRules(currentDisconnectRules);
}

function renderDisconnectRules(rules) {
    const container = $('disconnectRuleList');
    if (!container) return;

    if (!rules || rules.length === 0) {
        container.innerHTML = '<p style="color: var(--text-muted);">暂无断网规则</p>';
        return;
    }

    container.innerHTML = `
        <div class="rule-list disconnect-rule-list">
            ${rules.map((rule, index) => {
                const portText = `端口 ${rule.targetPort}`;
                const modeText = rule.mode === 1 ? 'Hex断网' : '定时断网';
                const intervalText = rule.mode === 0 && rule.disconnectIntervalSec > 0 ? `${rule.disconnectIntervalSec} 秒` : '-';
                const hexText = rule.mode === 1 && rule.hexPattern ? escapeHtml(rule.hexPattern) : '-';
                const hexDelayText = rule.mode === 1 ? (rule.hexDelayEnabled ? `${rule.hexDelaySeconds || 0} 秒` : '命中即断开') : '-';
                return `
                    <div class="rule-item disconnect-rule-card ${rule.enabled ? 'is-enabled' : 'is-disabled'}">
                        <div class="rule-info">
                            <div class="disconnect-rule-head">
                                <span class="disconnect-rule-title">${portText}</span>
                                <span class="mobile-badge ${rule.enabled ? 'is-on' : 'is-off'}">${rule.enabled ? '启用' : '停用'}</span>
                            </div>
                            <div class="disconnect-rule-meta">
                                <span><strong>模式:</strong> ${modeText}</span>
                                <span><strong>定时断开:</strong> ${intervalText}</span>
                                <span><strong>Hex延迟:</strong> ${hexDelayText}</span>
                            </div>
                            <div class="disconnect-rule-pattern"><strong>Hex规则:</strong> ${hexText}</div>
                        </div>
                        <div class="rule-actions disconnect-rule-actions">
                            <label class="mobile-inline-check">
                                <input type="checkbox" ${rule.enabled ? 'checked' : ''} onchange="toggleDisconnectRule(${index}, this.checked)">
                                启用
                            </label>
                            <button class="btn btn-ghost" onclick="editDisconnectRule(${index})">编辑</button>
                            <button class="btn btn-danger" onclick="deleteDisconnectRule(${index})">删除</button>
                        </div>
                    </div>
                `;
            }).join('')}
        </div>
    `;
}

function addDisconnectRule() {
    openDisconnectRuleModal();
}

function editDisconnectRule(index) {
    const rule = currentDisconnectRules[index];
    if (!rule) return;
    openDisconnectRuleModal(rule, index);
}

function deleteDisconnectRule(index) {
    const rule = currentDisconnectRules[index];
    if (!rule) return;
    if (!confirm(`确定要删除端口 ${rule.targetPort} 的断网规则吗?`)) {
        return;
    }
    currentDisconnectRules.splice(index, 1);
    saveDisconnectRules();
}

function toggleDisconnectRule(index, enabled) {
    const rule = currentDisconnectRules[index];
    if (!rule) return;
    rule.enabled = enabled === true;
    saveDisconnectRules(false);
}

function clearDisconnectRules() {
    if (!confirm('确定要清空当前实例的所有断网规则吗?')) {
        return;
    }
    currentDisconnectRules = [];
    saveDisconnectRules();
}

function openDisconnectRuleModal(rule = null, editIndex = -1) {
    const currentRule = normalizeDisconnectRule(rule || {});
    disconnectHexGridOffset = 500;
    showModal(editIndex >= 0 ? '编辑断网规则' : '添加断网规则', `
        <div class="form-group">
            <label>目标端口</label>
            <input type="number" id="disconnectRulePort" class="input" value="${currentRule.targetPort}" min="1" max="65535" placeholder="请输入目标端口">
            <div class="info-hint" style="margin-top:8px;">断网判断只会作用于该目标端口的连接。</div>
        </div>
        <div class="form-group">
            <label>断网模式</label>
            <select id="disconnectRuleMode" class="input">
                <option value="0" ${currentRule.mode === 0 ? 'selected' : ''}>定时断开</option>
                <option value="1" ${currentRule.mode === 1 ? 'selected' : ''}>Hex 断开</option>
            </select>
        </div>
        <div class="form-group" id="disconnectTimerGroup" style="display:${currentRule.mode === 0 ? 'block' : 'none'};">
            <label>定时断开秒数</label>
            <input type="number" id="disconnectRuleInterval" class="input" value="${currentRule.disconnectIntervalSec}" min="1" placeholder="多少秒后断开该端口连接">
        </div>
        <div class="form-group" id="disconnectHexGroup" style="display:${currentRule.mode === 1 ? 'block' : 'none'};">
            <label>Hex 匹配规则</label>
            <textarea id="disconnectRuleHexPattern" class="input disconnect-hex-textarea" rows="4" placeholder="示例: 0|16,1|03,2|??" onchange="syncGridFromText('disconnect')">${escapeHtml(currentRule.hexPattern)}</textarea>
            <div class="info-hint" style="margin-top:8px;">支持 <code>??</code> / <code>**</code> 通配。位置从 0 开始。可直接编辑文本，也可使用下方表格输入。</div>
            <div class="hex-grid-container disconnect-hex-grid-container" style="margin-top:10px;">
                <div class="toolbar wpe-grid-toolbar disconnect-grid-toolbar">
                    <input type="number" id="disconnectGridJump" class="input" style="width:110px;" placeholder="跳转位置" onchange="jumpHexGrid('disconnect')">
                    <span id="disconnectGridRange" class="text-muted">[0 ~ 0]</span>
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('disconnect', -${WPE_GRID_COLS})">&lt;&lt;</button>
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('disconnect', -1)">&lt;</button>
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('disconnect', 1)">&gt;</button>
                    <button class="btn btn-ghost btn-sm" onclick="scrollHexGrid('disconnect', ${WPE_GRID_COLS})">&gt;&gt;</button>
                    <button class="btn btn-ghost btn-sm" onclick="centerHexGrid('disconnect')">0</button>
                    <button class="btn btn-ghost btn-sm" style="color:#6f6;" onclick="pasteHexGrid('disconnect')">粘贴</button>
                    <button class="btn btn-ghost btn-sm" style="color:#f66;" onclick="clearWPEHexGrid('disconnect')">清空</button>
                </div>
                <div id="disconnectHexGridView"></div>
            </div>
        </div>
        <div class="checkbox-group">
            <input type="checkbox" id="disconnectRuleEnabled" ${currentRule.enabled ? 'checked' : ''}>
            <label for="disconnectRuleEnabled">启用该规则</label>
        </div>
        <div class="checkbox-group">
            <input type="checkbox" id="disconnectRuleHexDelayEnabled" ${currentRule.hexDelayEnabled ? 'checked' : ''}>
            <label for="disconnectRuleHexDelayEnabled">Hex 命中后延迟断开</label>
        </div>
        <div class="form-group" id="disconnectRuleHexDelayGroup" style="display:${currentRule.mode === 1 && currentRule.hexDelayEnabled ? 'block' : 'none'};">
            <label>Hex 延迟秒数</label>
            <input type="number" id="disconnectRuleHexDelaySeconds" class="input" value="${currentRule.hexDelaySeconds}" min="0" placeholder="0 表示命中即断开">
        </div>
    `, `
        <button class="btn btn-ghost" onclick="closeModal()">取消</button>
        <button class="btn btn-primary" onclick="confirmDisconnectRule(${editIndex})">保存</button>
    `);

    const hexDelayCheckbox = $('disconnectRuleHexDelayEnabled');
    if (hexDelayCheckbox) {
        hexDelayCheckbox.addEventListener('change', () => {
            const group = $('disconnectRuleHexDelayGroup');
            const modeSelect = $('disconnectRuleMode');
            if (group) {
                group.style.display = (modeSelect && modeSelect.value === '1' && hexDelayCheckbox.checked) ? 'block' : 'none';
            }
        });
    }

    const modeSelect = $('disconnectRuleMode');
    if (modeSelect) {
        modeSelect.addEventListener('change', () => {
            const timerGroup = $('disconnectTimerGroup');
            const hexGroup = $('disconnectHexGroup');
            const delayGroup = $('disconnectRuleHexDelayGroup');
            const isHexMode = modeSelect.value === '1';
            if (timerGroup) timerGroup.style.display = isHexMode ? 'none' : 'block';
            if (hexGroup) hexGroup.style.display = isHexMode ? 'block' : 'none';
            if (delayGroup) delayGroup.style.display = isHexMode && $('disconnectRuleHexDelayEnabled')?.checked ? 'block' : 'none';
        });
    }

    renderWPEHexGrid('disconnect', disconnectHexGridOffset, parsePatternString(currentRule.hexPattern || ''));
}

function confirmDisconnectRule(editIndex) {
    syncTextFromGrid('disconnect');

    const targetPort = parseInt($('disconnectRulePort').value, 10);
    const mode = parseInt($('disconnectRuleMode').value, 10) === 1 ? 1 : 0;
    const disconnectIntervalSec = parseInt($('disconnectRuleInterval').value, 10) || 0;
    const enabled = $('disconnectRuleEnabled').checked;
    const hexPattern = $('disconnectRuleHexPattern').value.trim();
    const hexDelayEnabled = $('disconnectRuleHexDelayEnabled').checked;
    const hexDelaySeconds = parseInt($('disconnectRuleHexDelaySeconds')?.value || '0', 10) || 0;

    if (Number.isNaN(targetPort) || targetPort < 1 || targetPort > 65535) {
        showToast('error', '错误', '目标端口必须在 1-65535 之间');
        return;
    }

    const duplicated = currentDisconnectRules.some((item, index) => index !== editIndex && item.targetPort === targetPort);
    if (duplicated) {
        showToast('error', '错误', `端口 ${targetPort} 的规则已存在`);
        return;
    }

    if (mode === 0 && disconnectIntervalSec <= 0) {
        showToast('error', '错误', '定时模式下断开秒数必须大于 0');
        return;
    }

    if (mode === 1 && !hexPattern) {
        showToast('error', '错误', 'Hex 模式下必须填写匹配规则');
        return;
    }

    const nextRule = normalizeDisconnectRule({
        targetPort,
        mode,
        disconnectIntervalSec: mode === 0 ? disconnectIntervalSec : 0,
        enabled,
        hexPattern: mode === 1 ? hexPattern : '',
        hexDelayEnabled: mode === 1 ? hexDelayEnabled : false,
        hexDelaySeconds: mode === 1 ? hexDelaySeconds : 0
    });

    if (editIndex >= 0 && editIndex < currentDisconnectRules.length) {
        currentDisconnectRules[editIndex] = nextRule;
    } else {
        currentDisconnectRules.push(nextRule);
    }

    sortDisconnectRules(currentDisconnectRules);
    closeModal();
    saveDisconnectRules();
}

function saveDisconnectRules(showSuccessToast = true) {
    if (!currentConfigInstance) {
        return;
    }

    const payloadRules = currentDisconnectRules.map(rule => ({
        targetPort: rule.targetPort,
        mode: rule.mode,
        disconnectIntervalSec: rule.disconnectIntervalSec,
        enabled: rule.enabled,
        hexPattern: rule.hexPattern,
        hexDelayEnabled: rule.hexDelayEnabled,
        hexDelaySeconds: rule.hexDelaySeconds
    }));

    postAction('config_set_disconnect', {
        instanceId: currentConfigInstance.id,
        rules: payloadRules,
        silent: showSuccessToast ? 0 : 1
    });
}

// Menu 7: User Filter
function renderUserFilterConfig(inst, isRunning) {
    return `
        <h3>用户滤镜配置</h3>
        <div class="sub-tabs">
            <button class="sub-tab ${currentUserFilterSubTab === 'webUserState' ? 'active' : ''}"
                onclick="switchUserFilterSubTab('webUserState')">Web用户态设置</button>
            <button class="sub-tab ${currentUserFilterSubTab === 'apiAddress' ? 'active' : ''}"
                onclick="switchUserFilterSubTab('apiAddress')">API地址</button>
        </div>
        <div id="userFilterSubTabContent">
            ${currentUserFilterSubTab === 'apiAddress'
                ? renderApiAddressSettings()
                : renderWebUserStateSettings()}
        </div>
    `;
}

function switchUserFilterSubTab(tab) {
    currentUserFilterSubTab = tab === 'apiAddress' ? 'apiAddress' : 'webUserState';
    const content = $('userFilterSubTabContent');
    if (content) {
        content.innerHTML = currentUserFilterSubTab === 'apiAddress'
            ? renderApiAddressSettings()
            : renderWebUserStateSettings();
    }
    if (lastUserFilterConfigData) {
        handleUserFilterConfigData(lastUserFilterConfigData);
    }
}

function renderApiAddressSettings() {
    return `
        <h4 class="section-title">HTTP服务器配置</h4>
        <div class="form-row">
            <div class="form-group">
                <label>监听端口</label>
                <input type="number" id="httpPort" class="input" value="8080">
            </div>
            <button class="btn btn-primary" onclick="saveHttpPort()">保存端口</button>
        </div>
        <div id="httpServerStatus"></div>

        <h4 class="section-title">用户访问地址</h4>
        <div id="accessUrls"></div>
    `;
}

function renderWebUserStateSettings() {
    return `
        <div class="info-hint">
            说明：启用用户滤镜模式后，每个SOCKS用户只能管理自己有权限的WPE滤镜。
        </div>
        <div class="checkbox-group">
            <input type="checkbox" id="enableUserFilterMode">
            <label for="enableUserFilterMode">启用用户滤镜模式</label>
        </div>
        <div class="checkbox-group">
            <input type="checkbox" id="showUserFilterCounts">
            <label for="showUserFilterCounts">远程控制台显示执行次数</label>
        </div>

        <h4 class="section-title">默认滤镜配置（对所有账号生效）</h4>
        <div class="info-hint">
            说明：未配置的用户将自动使用这些默认滤镜
        </div>
        <div id="defaultFilterList"></div>
        <div class="checkbox-group">
            <input type="checkbox" id="applyToAllUsers">
            <label for="applyToAllUsers">应用到所有现有用户</label>
        </div>
        <div class="toolbar">
            <button class="btn btn-primary" onclick="saveDefaultFilters()">保存默认配置</button>
            <button class="btn btn-ghost" onclick="selectAllFilters()">全选</button>
            <button class="btn btn-ghost" onclick="clearAllFilters()">清空</button>
        </div>

        <h4 class="section-title">用户Web显示名称</h4>
        <div id="wpeWebDisplayNameList"></div>

        <h4 class="section-title">WPE滤镜组</h4>
        <div class="toolbar">
            <button class="btn btn-primary" onclick="editWpeFilterGroup(0)">新增滤镜组</button>
            <button class="btn btn-ghost" onclick="postAction('get_wpe_filter_groups')">刷新滤镜组</button>
        </div>
        <div id="wpeFilterGroupList"></div>
        <div id="wpeFilterGroupEditor"></div>
    `;
}

function loadUserFilterConfig(instanceId) {
    postAction('config_get_userfilter', { instanceId });
}

function handleUserFilterConfigData(data) {
    lastUserFilterConfigData = data;
    window.__lastUserFilterConfigData = data;

    if ($('enableUserFilterMode')) {
        $('enableUserFilterMode').checked = data.enabled === true;
    }
    if ($('httpPort')) {
        $('httpPort').value = data.httpPort || 8080;
    }
    if ($('showUserFilterCounts')) {
        $('showUserFilterCounts').checked = data.showCounts === true;
    }

    // Event listener (prevent duplicate)
    const cb = $('enableUserFilterMode');
    if (cb && !cb._listenerAdded) {
        cb._listenerAdded = true;
        cb.addEventListener('change', () => {
            postAction('config_set_userfilter_mode', {
                instanceId: currentConfigInstance.id,
                enabled: cb.checked
            });
        });
    }
    const countsCb = $('showUserFilterCounts');
    if (countsCb && !countsCb._listenerAdded) {
        countsCb._listenerAdded = true;
        countsCb.addEventListener('change', () => {
            postAction('config_set_userfilter_showcounts', {
                instanceId: currentConfigInstance.id,
                enabled: countsCb.checked
            });
        });
    }

    // HTTP server status
    const statusDiv = $('httpServerStatus');
    const isRunning = data.httpServerRunning;
    if (statusDiv) {
        statusDiv.innerHTML = `
            <div class="info-row">
                <span class="info-label">服务器状态:</span>
                <span class="info-value ${isRunning ? 'status-running' : 'status-stopped'}">
                    ${isRunning ? '运行中' : '已停止'}
                </span>
                <button class="btn ${isRunning ? 'btn-danger' : 'btn-success'}" style="margin-left: 12px;"
                    onclick="${isRunning ? 'stopHttpServer()' : 'startHttpServer()'}">
                    ${isRunning ? '停止服务器' : '启动服务器'}
                </button>
            </div>
        `;
    }

    // Access URLs
    const urlsDiv = $("accessUrls");
    const localUrl = `http://127.0.0.1:${data.httpPort || 8080}`;
    const remoteUrl = data.remoteUrl || "";
    const remoteScope = data.remoteHostScope || "unknown";
    const accessHint = data.remoteAccessHint || "如需跨公网访问，请检查系统防火墙、路由器端口映射或云安全组。";

    if (urlsDiv) {
        urlsDiv.innerHTML = `
            <div class="info-row">
                <span class="info-label">本地访问:</span>
                <span class="info-value">${localUrl}</span>
                <button class="btn btn-ghost" style="margin-left: 8px;"
                    onclick='copyToClipboard("${localUrl}")'>复制</button>
            </div>
            ${remoteUrl ? `
            <div class="info-row">
                <span class="info-label">远程访问:</span>
                <span class="info-value">${remoteUrl}</span>
                <button class="btn btn-ghost" style="margin-left: 8px;"
                    onclick='copyToClipboard("${remoteUrl}")'>复制</button>
            </div>
            <div class="info-hint" style="margin-top:8px; border-color:${remoteScope === "public" ? "rgba(16,185,129,0.25)" : "rgba(245,158,11,0.25)"}; color:${remoteScope === "public" ? "#a7f3d0" : "#fde68a"};">
                ${accessHint}
            </div>
            ` : `
            <div class="info-hint" style="margin-top:8px;">
                ${accessHint}
            </div>
            `}
        `;
    }

    // Default filters
    renderDefaultFilters(data.availableFilters || [], data.defaultFilters || []);
    renderWpeWebDisplayNameSettings(data.availableFilters || []);
    renderWpeFilterGroups(data.wpeFilterGroups || wpeFilterGroups || []);
}

function buildDefaultFilterItemKey(filter, selectedIds) {
    return [
        filter.id,
        filter.name,
        selectedIds.has(filter.id) ? 1 : 0
    ].join(',');
}

function buildDefaultFilterItemHtml(filter, selectedIds) {
    return `<div class="checkbox-group">
            <input type="checkbox" id="defaultFilter_${filter.id}" value="${filter.id}"
                ${selectedIds.has(filter.id) ? 'checked' : ''}>
            <label for="defaultFilter_${filter.id}">[${filter.id}] ${filter.name}</label>
        </div>`;
}

function syncDefaultFilterItems(available, defaults) {
    const container = $('defaultFilterList');
    if (!container) return;

    const selectedIds = new Set(defaults || []);
    const nextItemKeys = (available || []).map(filter => buildDefaultFilterItemKey(filter, selectedIds));
    const items = container.querySelectorAll(':scope > .checkbox-group');

    if (items.length === nextItemKeys.length && lastDefaultFilterItemKeys.length === nextItemKeys.length) {
        for (let i = 0; i < nextItemKeys.length; i++) {
            if (lastDefaultFilterItemKeys[i] !== nextItemKeys[i]) {
                items[i].outerHTML = buildDefaultFilterItemHtml(available[i], selectedIds);
            }
        }
    } else {
        container.innerHTML = (available || []).map(filter => buildDefaultFilterItemHtml(filter, selectedIds)).join('');
    }

    lastDefaultFilterItemKeys = nextItemKeys;
}

function renderDefaultFilters(available, defaults) {
    const container = $('defaultFilterList');
    if (!container) return;

    if (!available || available.length === 0) {
        lastDefaultFilterItemKeys = [];
        container.innerHTML = '<p style="color: var(--text-muted);">暂无可用滤镜</p>';
        return;
    }

    syncDefaultFilterItems(available, defaults || []);
}

function renderWpeWebDisplayNameSettings(filters) {
    const container = $('wpeWebDisplayNameList');
    if (!container) return;

    const list = Array.isArray(filters) ? filters : [];
    if (list.length === 0) {
        container.innerHTML = '<p style="color: var(--text-muted);">暂无WPE滤镜</p>';
        return;
    }

    container.innerHTML = list.map(filter => `
        <div class="form-row wpe-web-name-row" data-filter-id="${filter.id}">
            <div class="form-group">
                <label>[${filter.id}] ${escapeHtml(filter.name || '')}</label>
                <input class="input wpe-web-display-name-input"
                    value="${escapeHtml(filter.webDisplayName || '')}"
                    placeholder="用户Web显示名称，留空则显示原名称">
            </div>
        </div>
    `).join('') + `
        <div class="toolbar">
            <button class="btn btn-primary" onclick="saveWpeWebDisplayNames()">保存显示名称</button>
        </div>
    `;
}

function saveWpeWebDisplayNames() {
    const rows = document.querySelectorAll('.wpe-web-name-row');
    const items = [];
    rows.forEach(row => {
        const input = row.querySelector('.wpe-web-display-name-input');
        const filterId = parseInt(row.dataset.filterId, 10);
        if (filterId > 0) {
            items.push({
                filterId,
                webDisplayName: input ? input.value.trim() : ''
            });
        }
    });
    postAction('save_wpe_web_display_names', { items });
}

function getAvailableWpeFiltersForUserSettings() {
    if (lastUserFilterConfigData && Array.isArray(lastUserFilterConfigData.availableFilters)) {
        return lastUserFilterConfigData.availableFilters;
    }
    return Array.isArray(wpeFilters) ? wpeFilters.map(f => ({ id: f.id, name: f.name })) : [];
}

function findWpeFilterGroup(groupId) {
    const id = parseInt(groupId, 10) || 0;
    return (wpeFilterGroups || []).find(g => parseInt(g.id, 10) === id) || null;
}

function renderWpeFilterGroups(groups) {
    wpeFilterGroups = Array.isArray(groups) ? groups : [];
    const container = $('wpeFilterGroupList');
    if (!container) return;

    if (wpeFilterGroups.length === 0) {
        container.innerHTML = '<div class="empty-state">暂无滤镜组</div>';
        return;
    }

    container.innerHTML = wpeFilterGroups.map(group => {
        const itemCount = Array.isArray(group.items) ? group.items.length : 0;
        const disabledClass = group.enabled === false ? ' is-disabled' : '';
        return `
            <div class="filter-group-card${disabledClass}">
                <div class="filter-group-head">
                    <div>
                        <strong>${escapeHtml(group.name || `滤镜组 ${group.id}`)}</strong>
                        <div class="filter-group-desc">${escapeHtml(group.description || '')}</div>
                    </div>
                    <span>${itemCount} 个滤镜</span>
                </div>
                <div class="filter-group-actions">
                    <button class="btn btn-sm" onclick="editWpeFilterGroup(${group.id || 0})">编辑</button>
                    <button class="btn btn-sm btn-danger" onclick="deleteWpeFilterGroup(${group.id || 0})">删除</button>
                </div>
            </div>
        `;
    }).join('');
}

function renderWpeFilterGroupEditor(group) {
    const availableFilters = getAvailableWpeFiltersForUserSettings();
    const itemMap = new Map();
    (group.items || []).forEach(item => {
        itemMap.set(parseInt(item.filterId, 10), item.defaultEnabled === true);
    });

    const rowsHtml = availableFilters.length === 0
        ? '<div class="empty-state">暂无可用WPE滤镜</div>'
        : availableFilters.map(filter => {
            const filterId = parseInt(filter.id, 10);
            const allowed = itemMap.has(filterId);
            const defaultEnabled = itemMap.get(filterId) === true;
            return `
                <div class="wpe-group-filter-row" data-filter-id="${filterId}">
                    <div class="wpe-group-filter-name">[${filterId}] ${escapeHtml(filter.name || '')}</div>
                    <label>
                        <input type="checkbox" class="wpe-group-filter-allowed" ${allowed ? 'checked' : ''}>
                        授权
                    </label>
                    <label>
                        <input type="checkbox" class="wpe-group-filter-default" ${defaultEnabled ? 'checked' : ''}>
                        默认启用
                    </label>
                </div>
            `;
        }).join('');

    const editor = $('wpeFilterGroupEditor');
    if (!editor) return;

    editor.innerHTML = `
        <div class="filter-group-editor">
            <h4 class="section-title">${group.id ? '编辑滤镜组' : '新增滤镜组'}</h4>
            <div class="form-row">
                <div class="form-group">
                    <label>滤镜组名称</label>
                    <input type="text" id="wpeGroupName" class="input" value="${escapeHtml(group.name || '')}">
                </div>
                <div class="form-group">
                    <label>说明</label>
                    <input type="text" id="wpeGroupDescription" class="input" value="${escapeHtml(group.description || '')}">
                </div>
            </div>
            <div class="checkbox-group">
                <input type="checkbox" id="wpeGroupEnabled" ${group.enabled === false ? '' : 'checked'}>
                <label for="wpeGroupEnabled">启用滤镜组</label>
            </div>
            <div id="wpeGroupFilterList" class="wpe-group-filter-list">
                ${rowsHtml}
            </div>
            <div class="toolbar">
                <button class="btn btn-primary" onclick="saveWpeFilterGroup()">保存滤镜组</button>
                <button class="btn btn-ghost" onclick="cancelWpeFilterGroupEdit()">取消</button>
            </div>
        </div>
    `;
}

function editWpeFilterGroup(groupId) {
    currentWpeFilterGroupId = parseInt(groupId, 10) || 0;
    const group = findWpeFilterGroup(currentWpeFilterGroupId) || {
        id: 0,
        name: '',
        description: '',
        enabled: true,
        items: []
    };
    renderWpeFilterGroupEditor(group);
}

function cancelWpeFilterGroupEdit() {
    currentWpeFilterGroupId = 0;
    const editor = $('wpeFilterGroupEditor');
    if (editor) editor.innerHTML = '';
}

function saveWpeFilterGroup() {
    const name = ($('wpeGroupName')?.value || '').trim();
    if (!name) {
        showToast('warning', '保存失败', '滤镜组名称不能为空');
        return;
    }

    const rows = document.querySelectorAll('#wpeGroupFilterList .wpe-group-filter-row');
    const items = [];
    rows.forEach(row => {
        const filterId = parseInt(row.dataset.filterId, 10);
        const allowed = row.querySelector('.wpe-group-filter-allowed');
        const def = row.querySelector('.wpe-group-filter-default');
        if (filterId > 0 && allowed && allowed.checked) {
            items.push({
                filterId,
                defaultEnabled: !!(def && def.checked)
            });
        }
    });

    postAction('save_wpe_filter_group', {
        groupId: currentWpeFilterGroupId || 0,
        name,
        description: ($('wpeGroupDescription')?.value || '').trim(),
        enabled: $('wpeGroupEnabled') ? $('wpeGroupEnabled').checked : true,
        items
    });
}

function deleteWpeFilterGroup(groupId) {
    const id = parseInt(groupId, 10) || 0;
    if (!id) return;
    if (!confirm('确定删除这个WPE滤镜组吗？')) return;
    postAction('delete_wpe_filter_group', { groupId: id });
}

function startHttpServer() {
    postAction('config_start_http_server', {
        instanceId: currentConfigInstance.id
    });
}

function stopHttpServer() {
    postAction('config_stop_http_server', {
        instanceId: currentConfigInstance.id
    });
}

function copyToClipboard(text) {
    // 方案1：尝试使用现代 Clipboard API
    if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(() => {
            showToast('success', '已复制', text);
        }).catch((err) => {
            console.error('Clipboard API 失败:', err);
            // 如果失败，尝试回退方案
            fallbackCopyToClipboard(text);
        });
    } else {
        // 方案2：使用传统的 execCommand 方法（兼容性更好）
        fallbackCopyToClipboard(text);
    }
}

function fallbackCopyToClipboard(text) {
    // 创建临时 textarea 元素
    const textarea = document.createElement('textarea');
    textarea.value = text;
    textarea.style.position = 'fixed';
    textarea.style.top = '0';
    textarea.style.left = '0';
    textarea.style.width = '2em';
    textarea.style.height = '2em';
    textarea.style.padding = '0';
    textarea.style.border = 'none';
    textarea.style.outline = 'none';
    textarea.style.boxShadow = 'none';
    textarea.style.background = 'transparent';
    textarea.style.opacity = '0';

    document.body.appendChild(textarea);
    textarea.focus();
    textarea.select();

    try {
        const successful = document.execCommand('copy');
        if (successful) {
            showToast('success', '已复制', text);
        } else {
            showToast('error', '复制失败', '无法复制到剪贴板');
        }
    } catch (err) {
        console.error('execCommand 复制失败:', err);
        showToast('error', '复制失败', '浏览器不支持复制功能');
    }

    document.body.removeChild(textarea);
}

function saveHttpPort() {
    const port = parseInt($('httpPort').value);
    postAction('config_set_http_port', {
        instanceId: currentConfigInstance.id,
        port
    });
}

function saveDefaultFilters() {
    const checkboxes = document.querySelectorAll('#defaultFilterList input[type="checkbox"]');
    const selectedFilters = [];

    checkboxes.forEach(cb => {
        if (cb.checked) {
            selectedFilters.push(cb.value);
        }
    });

    const applyToAll = $('applyToAllUsers') && $('applyToAllUsers').checked;

    postAction('config_set_defaultfilter', {
        instanceId: currentConfigInstance.id,
        filterIds: selectedFilters,
        applyToAllUsers: applyToAll
    });
}

function selectAllFilters() {
    document.querySelectorAll('#defaultFilterList input[type="checkbox"]').forEach(cb => {
        cb.checked = true;
    });
}

function clearAllFilters() {
    document.querySelectorAll('#defaultFilterList input[type="checkbox"]').forEach(cb => {
        cb.checked = false;
    });
}

function buildWpeGroupCheckboxes(selectedGroupIds) {
    const selected = new Set((selectedGroupIds || []).map(Number));
    if (!wpeFilterGroups || wpeFilterGroups.length === 0) {
        return '<div class="empty-state">暂无WPE滤镜组</div>';
    }

    return wpeFilterGroups.map(group => `
        <label class="checkbox-group">
            <input type="checkbox" class="wpe-group-bind-cb" value="${group.id}" ${selected.has(Number(group.id)) ? 'checked' : ''}>
            <span>${escapeHtml(group.name || `滤镜组 ${group.id}`)}</span>
        </label>
    `).join('');
}

function collectSelectedWpeGroupIds(rootSelector) {
    return Array.from(document.querySelectorAll(`${rootSelector} .wpe-group-bind-cb:checked`))
        .map(cb => parseInt(cb.value, 10))
        .filter(Number.isFinite);
}

// Menu 7: Account Filter
function renderAccountFilterConfig(inst, isRunning) {
    return `
        <h3>账号滤镜配置查看</h3>
        <div class="info-hint">
            此页面显示每个SOCKS账号当前生效的滤镜配置
        </div>
        <div id="accountFilterList"></div>
    `;
}

function loadAccountFilterConfig(instanceId) {
    postAction('config_get_accountfilter', { instanceId });
}

function buildAccountFilterItemKey(account) {
    const filtersKey = Array.isArray(account.filters)
        ? account.filters.map(f => [f.id, f.name].join(':')).join('|')
        : '';
    const groupsKey = Array.isArray(account.groupIds) ? account.groupIds.join(':') : '';
    return [
        account.username || '',
        account.hasCustomConfig ? 1 : 0,
        filtersKey,
        groupsKey
    ].join(',');
}

function buildAccountFilterItemHtml(account) {
    const configType = account.hasCustomConfig ? '自定义' : '默认配置';
    const configColor = account.hasCustomConfig ? 'var(--success)' : 'var(--warning)';

    let filtersHtml = '';
    if (account.filters && account.filters.length > 0) {
        filtersHtml = `<div style="padding-left: 20px; margin-top: 4px;">
                <span style="color: var(--text-secondary); font-size: 13px;">
                    生效滤镜 (${account.filters.length} 个):
                </span>
                ${account.filters.map(f => `
                    <div style="padding-left: 20px; font-size: 13px; color: var(--text-secondary);">
                        &bull; [${f.id}] ${f.name}
                    </div>
                `).join('')}
            </div>`;
    } else {
        filtersHtml = '<div style="padding-left: 20px; margin-top: 4px; color: var(--text-muted); font-size: 13px;">无生效滤镜</div>';
    }

    return `<div class="list-item account-filter-item" data-username="${escapeHtml(account.username || '')}" style="flex-direction: column; align-items: flex-start;">
                <div>
                    <span style="color: ${configColor}; font-weight: 600;">[${configType}]</span>
                    <span style="font-weight: 600;">${escapeHtml(account.username || '')}</span>
                </div>
                ${filtersHtml}
                <div class="account-filter-group-bind">
                    <div style="color: var(--text-secondary); font-size: 13px; margin-top: 8px;">滤镜组权限</div>
                    ${buildWpeGroupCheckboxes(account.groupIds || [])}
                    <button class="btn btn-primary btn-sm" onclick="saveAccountWpeFilterGroups('${escapeHtml(account.username || '')}')">保存组权限</button>
                </div>
            </div>`;
}

function syncAccountFilterItems(accounts) {
    const container = $('accountFilterList');
    if (!container) return;

    const nextItemKeys = (accounts || []).map(buildAccountFilterItemKey);
    const items = container.querySelectorAll(':scope > .list-item');

    if (items.length === nextItemKeys.length && lastAccountFilterItemKeys.length === nextItemKeys.length) {
        for (let i = 0; i < nextItemKeys.length; i++) {
            if (lastAccountFilterItemKeys[i] !== nextItemKeys[i]) {
                items[i].outerHTML = buildAccountFilterItemHtml(accounts[i]);
            }
        }
    } else {
        container.innerHTML = (accounts || []).map(buildAccountFilterItemHtml).join('');
    }

    lastAccountFilterItemKeys = nextItemKeys;
}

function handleAccountFilterData(data) {
    const container = $('accountFilterList');
    if (!container) return;

    if (Array.isArray(data.wpeFilterGroups)) {
        wpeFilterGroups = data.wpeFilterGroups;
    }

    if (!data || !data.accounts || data.accounts.length === 0) {
        lastAccountFilterItemKeys = [];
        container.innerHTML = `
            <p style="color: var(--text-muted);">暂无账号滤镜配置数据</p>
            <p style="color: var(--text-secondary); font-size: 13px;">
                请确保实例已启动、已启用用户滤镜模式、且已绑定账号库
            </p>
        `;
        return;
    }

    syncAccountFilterItems(data.accounts);
}

function saveAccountWpeFilterGroups(username) {
    const row = Array.from(document.querySelectorAll('.account-filter-item'))
        .find(item => item.dataset.username === username);
    if (!row || !currentConfigInstance) return;

    const groupIds = Array.from(row.querySelectorAll('.wpe-group-bind-cb:checked'))
        .map(cb => parseInt(cb.value, 10))
        .filter(Number.isFinite);

    postAction('save_user_wpe_filter_groups', {
        instanceId: currentConfigInstance.id,
        username,
        groupIds
    });
}

// Initialize titlebar drag
initTitlebarDrag();

setTimeout(() => {
    initRemoteBrowserClientUi();
    pollRemoteBrowserMessages();

    // 延后到当前脚本整体执行完成后再向后端发起初始化请求，
    // 避免 proxydata 等消息在相关 let 状态变量初始化前就进入处理链，
    // 触发 TDZ 报错（例如 cachedCharlesProjectionKey）。
    postAction('get_status');
    postAction('get_notice');
    loadAppConfig();
}, 0);

// ========== Proxy Data Viewer Page ==========
const PROXY_INSTANCE_PANEL_WIDTH_STORAGE_KEY = 'proxyInstancePanelWidthPx';
const PROXY_INSTANCE_PANEL_COLLAPSED_STORAGE_KEY = 'proxyInstancePanelCollapsed';
let selectedProxyInstanceId = null;
let proxyDataRefreshInterval = null;
let lastProxyRealtimeUpdateAt = 0;
let proxyRealtimeEnabled = true;
let currentProxyHexSearch = '';
let currentProxySubview = 'raw';
let currentProxyRecordEnabled = false;
let currentProxyBufferSize = 200;
let currentProxyTotalCount = 0;
let currentProxyReturnedCount = 0;
let currentProxyShouldStickToBottom = false;
let currentProxyProjectionKey = '';
let currentProxyLastBackendInstanceId = '';
let currentProxyLastBackendReturnedCount = 0;
let currentProxyLastBackendTotalCount = 0;
let currentProxyLastDropReason = '';
let currentProxyFocusedSequence = '';
let currentProxyNeedsFocusScroll = false;
let proxyFocusClearTimer = null;
let activeProxyRealtimeSubscriptionInstanceId = '';
let activeProxyRealtimeSubscriptionEnabled = false;
let currentProxyInstancePanelWidthPx = (() => {
    try {
        const raw = Number(window.localStorage.getItem(PROXY_INSTANCE_PANEL_WIDTH_STORAGE_KEY));
        return Number.isFinite(raw) ? Math.min(520, Math.max(220, raw)) : 280;
    } catch (error) {
        return 280;
    }
})();
let currentProxyInstancePanelCollapsed = (() => {
    try {
        return window.localStorage.getItem(PROXY_INSTANCE_PANEL_COLLAPSED_STORAGE_KEY) === '1';
    } catch (error) {
        return false;
    }
})();
const PROXY_VIRTUAL_ROW_HEIGHT = 36;
const PROXY_VIRTUAL_OVERSCAN = 12;
const PROXY_POLL_FALLBACK_MS = 2000;
const CHARLES_SESSION_IDLE_GAP_MS = 2500;
const CHARLES_UNPARSED_GROUP_LABEL = 'Generic TCP / 未解析';
const CHARLES_LIST_WIDTH_STORAGE_KEY = 'charlesListWidthPercent';
let currentCharlesViewMode = 'sequence';
let currentCharlesSearchText = '';
let currentCharlesProtocolFilter = 'all';
let currentCharlesFocusHost = 'all';
let currentCharlesMethodFilter = 'all';
let currentCharlesStatusFilter = 'all';
let currentCharlesContentTypeFilter = 'all';
let currentCharlesShowUnparsed = true;
let currentCharlesSelectedSessionId = '';
let currentCharlesDetailTab = 'overview';
let currentCharlesContentViewRequest = 'text';
let currentCharlesContentViewResponse = 'text';
let lastCharlesRenderKey = '';
let lastCharlesListRenderKey = '';
let lastCharlesDetailRenderKey = '';
let cachedCharlesProjectionKey = '';
let cachedCharlesSessions = [];
let currentCharlesCollapsedEndpoints = new Set();
let charlesFilterDebounceTimer = null;

function shouldUseProxyDesktopLayout() {
    return window.innerWidth > 1024;
}

function applyProxyInstancePanelWidth() {
    const card = $('proxyInstanceCard');
    const collapseBtn = $('proxyInstanceCollapseBtn');
    if (!card) return;

    if (!shouldUseProxyDesktopLayout()) {
        card.style.width = '';
        card.classList.remove('is-collapsed');
        if (collapseBtn) {
            collapseBtn.textContent = '«';
            collapseBtn.title = '折叠实例菜单';
            collapseBtn.setAttribute('aria-label', '折叠实例菜单');
        }
        return;
    }

    card.classList.toggle('is-collapsed', currentProxyInstancePanelCollapsed);
    if (collapseBtn) {
        collapseBtn.textContent = currentProxyInstancePanelCollapsed ? '»' : '«';
        collapseBtn.title = currentProxyInstancePanelCollapsed ? '展开实例菜单' : '折叠实例菜单';
        collapseBtn.setAttribute('aria-label', currentProxyInstancePanelCollapsed ? '展开实例菜单' : '折叠实例菜单');
    }

    if (currentProxyInstancePanelCollapsed) {
        card.style.width = '44px';
        return;
    }

    currentProxyInstancePanelWidthPx = Math.min(520, Math.max(220, currentProxyInstancePanelWidthPx));
    card.style.width = `${currentProxyInstancePanelWidthPx}px`;
}

function toggleProxyInstancePanelCollapsed(forceCollapsed = null) {
    const nextCollapsed = typeof forceCollapsed === 'boolean'
        ? forceCollapsed
        : !currentProxyInstancePanelCollapsed;
    currentProxyInstancePanelCollapsed = nextCollapsed;
    applyProxyInstancePanelWidth();
    try {
        window.localStorage.setItem(PROXY_INSTANCE_PANEL_COLLAPSED_STORAGE_KEY, nextCollapsed ? '1' : '0');
    } catch (error) {}
}

function bindProxyLayoutSplitter() {
    const splitter = $('proxyLayoutSplitter');
    const layout = document.querySelector('.proxydata-layout');
    const card = $('proxyInstanceCard');
    const collapseBtn = $('proxyInstanceCollapseBtn');
    if (!splitter || !layout || !card || splitter.dataset.bound === '1') {
        applyProxyInstancePanelWidth();
        return;
    }

    splitter.dataset.bound = '1';
    applyProxyInstancePanelWidth();

    if (collapseBtn && collapseBtn.dataset.bound !== '1') {
        collapseBtn.dataset.bound = '1';
        collapseBtn.addEventListener('click', () => toggleProxyInstancePanelCollapsed());
    }

    splitter.addEventListener('pointerdown', event => {
        if (!shouldUseProxyDesktopLayout()) {
            return;
        }

        event.preventDefault();
        const rect = layout.getBoundingClientRect();
        splitter.setPointerCapture?.(event.pointerId);

        const handleMove = moveEvent => {
            const nextWidth = moveEvent.clientX - rect.left;
            if (currentProxyInstancePanelCollapsed) {
                currentProxyInstancePanelCollapsed = false;
            }
            currentProxyInstancePanelWidthPx = Math.min(rect.width - 320, Math.max(220, nextWidth));
            applyProxyInstancePanelWidth();
        };

        const handleUp = upEvent => {
            splitter.releasePointerCapture?.(upEvent.pointerId);
            window.removeEventListener('pointermove', handleMove);
            window.removeEventListener('pointerup', handleUp);
            try {
                window.localStorage.setItem(PROXY_INSTANCE_PANEL_WIDTH_STORAGE_KEY, String(Math.round(currentProxyInstancePanelWidthPx)));
                window.localStorage.setItem(PROXY_INSTANCE_PANEL_COLLAPSED_STORAGE_KEY, currentProxyInstancePanelCollapsed ? '1' : '0');
            } catch (error) {}
        };

        window.addEventListener('pointermove', handleMove);
        window.addEventListener('pointerup', handleUp);
    });

    splitter.addEventListener('dblclick', () => {
        if (!shouldUseProxyDesktopLayout()) {
            return;
        }
        toggleProxyInstancePanelCollapsed();
    });

    window.addEventListener('resize', applyProxyInstancePanelWidth);
}
let currentCharlesListWidthPercent = (() => {
    try {
        const raw = Number(window.localStorage.getItem(CHARLES_LIST_WIDTH_STORAGE_KEY));
        return Number.isFinite(raw) ? Math.min(55, Math.max(20, raw)) : 30;
    } catch (error) {
        return 30;
    }
})();

function applyCharlesLayoutWidth() {
    const workspace = $('charlesWorkspace');
    if (!workspace) return;
    workspace.style.setProperty('--charles-list-width', `${currentCharlesListWidthPercent}%`);
}

function bindCharlesSplitter() {
    const splitter = $('charlesSplitter');
    const workspace = $('charlesWorkspace');
    if (!splitter || !workspace || splitter.dataset.bound === '1') {
        applyCharlesLayoutWidth();
        return;
    }

    splitter.dataset.bound = '1';
    applyCharlesLayoutWidth();

    splitter.addEventListener('pointerdown', event => {
        event.preventDefault();
        const rect = workspace.getBoundingClientRect();
        splitter.setPointerCapture?.(event.pointerId);

        const handleMove = moveEvent => {
            const nextPercent = ((moveEvent.clientX - rect.left) / rect.width) * 100;
            currentCharlesListWidthPercent = Math.min(55, Math.max(20, nextPercent));
            applyCharlesLayoutWidth();
        };

        const handleUp = upEvent => {
            splitter.releasePointerCapture?.(upEvent.pointerId);
            window.removeEventListener('pointermove', handleMove);
            window.removeEventListener('pointerup', handleUp);
            try {
                window.localStorage.setItem(CHARLES_LIST_WIDTH_STORAGE_KEY, String(Math.round(currentCharlesListWidthPercent)));
            } catch (error) {}
        };

        window.addEventListener('pointermove', handleMove);
        window.addEventListener('pointerup', handleUp);
    });
}

function stopProxyDataRefresh() {
    if (proxyDataRefreshInterval) {
        clearInterval(proxyDataRefreshInterval);
        proxyDataRefreshInterval = null;
    }
}

function shouldKeepProxyRealtimeActive(instanceId) {
    return !!instanceId
        && proxyRealtimeEnabled
        && PageVisibility.isVisible
        && isMainPageActive('proxydata')
        && selectedProxyInstanceId === instanceId;
}

function startProxyDataRefresh(instanceId) {
    stopProxyDataRefresh();

    if (!shouldKeepProxyRealtimeActive(instanceId)) {
        return;
    }

    proxyDataRefreshInterval = setInterval(() => {
        if (!shouldKeepProxyRealtimeActive(instanceId)) {
            stopProxyDataRefresh();
            return;
        }

        // 实时推送偶发丢失或未建立订阅时，保持一个轻量轮询兜底，
        // 避免“后端已记录首包，但页面长期不刷新”的情况。
        const hasVisiblePackets = Array.isArray(window._proxyPackets) && window._proxyPackets.length > 0;
        if (!hasVisiblePackets || Date.now() - lastProxyRealtimeUpdateAt > 1500) {
            requestProxyData(instanceId);
        }
    }, PROXY_POLL_FALLBACK_MS);
}

function getProxyInstanceNameById(instanceId) {
    const instances = window.cachedInstances || [];
    const instance = instances.find(inst => inst.id === instanceId);
    return instance ? instance.name : instanceId;
}

function updateProxySubviewTabs() {
    const rawBtn = $('proxySubviewRawBtn');
    const charlesBtn = $('proxySubviewCharlesBtn');
    if (rawBtn) rawBtn.classList.toggle('active', currentProxySubview === 'raw');
    if (charlesBtn) charlesBtn.classList.toggle('active', currentProxySubview === 'charles');
}

function switchProxySubview(subview, options = {}) {
    const nextSubview = subview === 'charles' ? 'charles' : 'raw';
    if (currentProxySubview === nextSubview && !options.force) {
        return;
    }

    currentProxySubview = nextSubview;
    updateProxySubviewTabs();
    lastProxyDataRenderKey = '';
    lastCharlesRenderKey = ''; lastCharlesListRenderKey = ''; lastCharlesDetailRenderKey = '';

    if (!selectedProxyInstanceId) {
        return;
    }

    const instanceName = getProxyInstanceNameById(selectedProxyInstanceId);
    updateProxyDataPanelShell(selectedProxyInstanceId, instanceName);
    renderCurrentProxySubview();
}

function scheduleCharlesRender() {
    if (charlesFilterDebounceTimer) {
        clearTimeout(charlesFilterDebounceTimer);
    }
    charlesFilterDebounceTimer = setTimeout(() => {
        charlesFilterDebounceTimer = null;
        lastCharlesRenderKey = ''; lastCharlesListRenderKey = ''; lastCharlesDetailRenderKey = '';
        renderCharlesCurrentState();
    }, 120);
}

function updateCharlesFiltersFromUi() {
    const searchInput = $('charlesSearchInput');
    const protocolSelect = $('charlesProtocolFilter');
    const focusHostSelect = $('charlesFocusHostFilter');
    const methodSelect = $('charlesMethodFilter');
    const statusSelect = $('charlesStatusFilter');
    const contentTypeSelect = $('charlesContentTypeFilter');
    const showUnparsedCheckbox = $('charlesShowUnparsed');

    currentCharlesSearchText = searchInput ? searchInput.value.trim() : '';
    currentCharlesProtocolFilter = protocolSelect ? protocolSelect.value : 'all';
    currentCharlesFocusHost = focusHostSelect ? focusHostSelect.value : 'all';
    currentCharlesMethodFilter = methodSelect ? methodSelect.value : 'all';
    currentCharlesStatusFilter = statusSelect ? statusSelect.value : 'all';
    currentCharlesContentTypeFilter = contentTypeSelect ? contentTypeSelect.value : 'all';
    currentCharlesShowUnparsed = showUnparsedCheckbox ? !!showUnparsedCheckbox.checked : true;
    scheduleCharlesRender();
}

function resetCharlesFilters() {
    currentCharlesSearchText = '';
    currentCharlesProtocolFilter = 'all';
    currentCharlesFocusHost = 'all';
    currentCharlesMethodFilter = 'all';
    currentCharlesStatusFilter = 'all';
    currentCharlesContentTypeFilter = 'all';
    currentCharlesShowUnparsed = true;

    const searchInput = $('charlesSearchInput');
    const protocolSelect = $('charlesProtocolFilter');
    const focusHostSelect = $('charlesFocusHostFilter');
    const methodSelect = $('charlesMethodFilter');
    const statusSelect = $('charlesStatusFilter');
    const contentTypeSelect = $('charlesContentTypeFilter');
    const showUnparsedCheckbox = $('charlesShowUnparsed');

    if (searchInput) searchInput.value = '';
    if (protocolSelect) protocolSelect.value = 'all';
    if (focusHostSelect) focusHostSelect.value = 'all';
    if (methodSelect) methodSelect.value = 'all';
    if (statusSelect) statusSelect.value = 'all';
    if (contentTypeSelect) contentTypeSelect.value = 'all';
    if (showUnparsedCheckbox) showUnparsedCheckbox.checked = true;

    lastCharlesRenderKey = ''; lastCharlesListRenderKey = ''; lastCharlesDetailRenderKey = '';
    renderCharlesCurrentState();
}

function clearProxyFocusHighlight() {
    currentProxyFocusedSequence = '';
    currentProxyNeedsFocusScroll = false;
    if (proxyFocusClearTimer) {
        clearTimeout(proxyFocusClearTimer);
        proxyFocusClearTimer = null;
    }
    lastProxyDataRenderKey = '';
    if (currentProxySubview === 'raw') {
        renderProxyRawCurrentState();
    }
}

function scheduleProxyFocusClear() {
    if (proxyFocusClearTimer) {
        clearTimeout(proxyFocusClearTimer);
    }
    proxyFocusClearTimer = setTimeout(() => {
        proxyFocusClearTimer = null;
        clearProxyFocusHighlight();
    }, 12000);
}

function setCharlesViewMode(mode) {
    currentCharlesViewMode = mode === 'structure' ? 'structure' : 'sequence';
    lastCharlesRenderKey = ''; lastCharlesListRenderKey = ''; lastCharlesDetailRenderKey = '';
    renderCharlesCurrentState();
}

function setCharlesDetailTab(tab) {
    currentCharlesDetailTab = tab || 'overview';
    lastCharlesRenderKey = ''; lastCharlesListRenderKey = ''; lastCharlesDetailRenderKey = '';
    renderCharlesCurrentState();
}

function setCharlesContentTab(direction, mode) {
    const normalizedDirection = direction === 'response' ? 'response' : 'request';
    const normalizedMode = (mode === 'hex' || mode === 'raw') ? mode : 'text';
    if (normalizedDirection === 'response') {
        currentCharlesContentViewResponse = normalizedMode;
    } else {
        currentCharlesContentViewRequest = normalizedMode;
    }
    lastCharlesRenderKey = ''; lastCharlesListRenderKey = ''; lastCharlesDetailRenderKey = '';
    renderCharlesCurrentState();
}

function selectCharlesSession(sessionId) {
    currentCharlesSelectedSessionId = sessionId || '';
    currentCharlesDetailTab = currentCharlesDetailTab || 'overview';
    lastCharlesRenderKey = ''; lastCharlesListRenderKey = ''; lastCharlesDetailRenderKey = '';
    renderCharlesCurrentState();
}

function toggleCharlesEndpoint(endpointLabel) {
    if (!endpointLabel) return;
    if (currentCharlesCollapsedEndpoints.has(endpointLabel)) {
        currentCharlesCollapsedEndpoints.delete(endpointLabel);
    } else {
        currentCharlesCollapsedEndpoints.add(endpointLabel);
    }
    lastCharlesRenderKey = ''; lastCharlesListRenderKey = ''; lastCharlesDetailRenderKey = '';
    renderCharlesCurrentState();
}

function encodeCharlesEndpointKey(value) {
    return encodeURIComponent(String(value || ''));
}

function decodeCharlesEndpointKey(value) {
    try {
        return decodeURIComponent(String(value || ''));
    } catch (error) {
        return String(value || '');
    }
}

function toggleCharlesEndpointByKey(encodedKey) {
    toggleCharlesEndpoint(decodeCharlesEndpointKey(encodedKey));
}

function initProxyDataPage() {
    lastProxyListRenderKey = '';
    updateProxySubviewTabs();
    bindProxyLayoutSplitter();
    // 加载 SOCKS 转发实例列表
    postAction('get_instances');
}

function buildProxyInstanceCardKey(inst) {
    return [
        inst.id,
        inst.name,
        inst.port,
        inst.state,
        selectedProxyInstanceId === inst.id ? 1 : 0
    ].join(',');
}

function buildProxyInstanceCardHtml(inst) {
    const isSelected = selectedProxyInstanceId === inst.id;
    return `<div class="pool-item ${isSelected ? 'selected' : ''}"
             onclick="selectProxyInstance('${inst.id}', '${inst.name}')"
             style="cursor:pointer; padding:12px; margin-bottom:8px; border-radius:6px; background:${isSelected ? 'rgba(110, 231, 231, 0.1)' : 'transparent'}; border:1px solid ${isSelected ? '#6ee7e7' : 'var(--border)'};">
            <div style="font-size:13px; color:var(--text); font-weight:500;">${inst.name}</div>
            <div style="font-size:11px; color:var(--text-muted); margin-top:4px;">端口: ${inst.port} | ${inst.state === 'Running' ? '<span style="color:#0f0;">运行中</span>' : '<span style="color:#888;">已停止</span>'}</div>
        </div>`;
}

function syncProxyInstanceCards(instances) {
    const container = $('proxyInstanceListPanel');
    if (!container) return;

    const nextCardKeys = (instances || []).map(buildProxyInstanceCardKey);
    const cards = container.querySelectorAll(':scope > .pool-item');

    if (cards.length === nextCardKeys.length && lastProxyInstanceCardKeys.length === nextCardKeys.length) {
        for (let i = 0; i < nextCardKeys.length; i++) {
            if (lastProxyInstanceCardKeys[i] !== nextCardKeys[i]) {
                cards[i].outerHTML = buildProxyInstanceCardHtml(instances[i]);
            }
        }
    } else {
        container.innerHTML = (instances || []).map(buildProxyInstanceCardHtml).join('');
    }

    lastProxyInstanceCardKeys = nextCardKeys;
}

function renderProxyInstanceList(instances) {
    const socksForwardInstances = (instances || []).filter(inst => inst.type === 'SocksForward');
    const renderKey = (selectedProxyInstanceId || '') + '|' + socksForwardInstances.map(inst => [
        inst.id,
        inst.name,
        inst.port,
        inst.state
    ].join(',')).join('|');

    if (!isMainPageActive('proxydata')) {
        return;
    }
    if (renderKey === lastProxyListRenderKey) {
        return;
    }
    lastProxyListRenderKey = renderKey;

    const container = $('proxyInstanceListPanel');
    if (!container) return;

    if (socksForwardInstances.length === 0) {
        lastProxyInstanceCardKeys = [];
        container.innerHTML = '<div style="text-align:center; padding:20px; color:var(--text-muted); font-size:13px;">暂无SOCKS转发实例</div>';
        return;
    }

    syncProxyInstanceCards(socksForwardInstances);
}

function selectProxyInstance(instanceId, instanceName) {
    selectedProxyInstanceId = instanceId;
    currentProxyHexSearch = '';
    stopProxyDataRefresh();

    if (Array.isArray(window.cachedInstances) && window.cachedInstances.length > 0) {
        lastProxyListRenderKey = '';
        renderProxyInstanceList(window.cachedInstances);
    }

    loadProxyDataForInstance(instanceId, instanceName);
    syncProxyRealtimeState();
    postAction('config_search_proxydata', { instanceId, hexQuery: '' });
}

function setProxyRealtimeSubscription(instanceId, enabled) {
    const normalizedEnabled = !!enabled;
    const targetInstanceId = normalizedEnabled
        ? instanceId
        : (activeProxyRealtimeSubscriptionInstanceId || instanceId);

    if (!targetInstanceId) {
        return;
    }

    if (normalizedEnabled) {
        if (activeProxyRealtimeSubscriptionEnabled && activeProxyRealtimeSubscriptionInstanceId === targetInstanceId) {
            return;
        }
    } else if (!activeProxyRealtimeSubscriptionEnabled && !activeProxyRealtimeSubscriptionInstanceId) {
        return;
    }

    postAction('config_set_proxydata_realtime', {
        instanceId: targetInstanceId,
        enabled: normalizedEnabled
    });
    activeProxyRealtimeSubscriptionEnabled = normalizedEnabled;
    activeProxyRealtimeSubscriptionInstanceId = normalizedEnabled ? targetInstanceId : '';
}

function syncProxyRealtimeState(options = {}) {
    const instanceId = selectedProxyInstanceId;
    const shouldEnableRealtime = shouldKeepProxyRealtimeActive(instanceId);

    if (!shouldEnableRealtime) {
        stopProxyDataRefresh();
        setProxyRealtimeSubscription(instanceId, false);
        return false;
    }

    setProxyRealtimeSubscription(instanceId, true);
    startProxyDataRefresh(instanceId);

    if (options.forceFullRefresh) {
        lastProxyRealtimeUpdateAt = Date.now();
        requestProxyData(instanceId, true);
    }

    return true;
}

PageVisibility.onPause(() => {
    stopProxyDataRefresh();
    setProxyRealtimeSubscription(selectedProxyInstanceId, false);

    if (remoteBridgeState.pollAbortController) {
        remoteBridgeState.pollAbortController.abort();
        remoteBridgeState.pollAbortController = null;
    }
});

PageVisibility.onResume(() => {
    syncProxyRealtimeState({ forceFullRefresh: true });
});

function searchProxyData(instanceId) {
    const input = $('proxyHexSearchInput');
    currentProxyHexSearch = input ? input.value.trim() : '';
    postAction('config_search_proxydata', { instanceId, hexQuery: currentProxyHexSearch });
}

function clearProxyDataSearch(instanceId) {
    currentProxyHexSearch = '';
    const input = $('proxyHexSearchInput');
    if (input) input.value = '';
    postAction('config_search_proxydata', { instanceId, hexQuery: '' });
}

function toggleProxyRealtime(instanceId) {
    const toggle = $('proxyRealtimeToggle');
    proxyRealtimeEnabled = toggle ? toggle.checked : true;
    updateProxyDataPerfStatus();
    if (proxyRealtimeEnabled) {
        syncProxyRealtimeState({ forceFullRefresh: true });
        return;
    }

    if (instanceId && selectedProxyInstanceId !== instanceId) {
        selectedProxyInstanceId = instanceId;
    }
    syncProxyRealtimeState();
}

function ensureProxyDataPanelShell(content) {
    if (content.dataset.shellReady === '1' && $('proxyDataInstanceName') && $('proxySubviewContent')) {
        return;
    }

    content.innerHTML = `
        <div class="proxy-shared-header">
            <div>
                <span id="proxyDataInstanceName" style="color:#6ee7e7; font-size:16px; font-weight:600;"></span>
                <span id="proxyDataInstanceId" style="color:var(--text-muted); font-size:13px; margin-left:8px;"></span>
            </div>
            <div class="proxy-shared-subtitle" id="proxySubviewSummary">同一实例下共用原始包记录源</div>
        </div>
        <div id="proxyDataStatsHint" style="margin:8px 0 14px; color:var(--text-muted); font-size:12px;"></div>
        <div id="proxySubviewContent" style="min-height:0; flex:1; display:flex; flex-direction:column;"></div>
    `;
    content.dataset.shellReady = '1';
}

function ensureProxyRawShell(container) {
    if (container.dataset.shellMode === 'raw' && $('proxyDataTableContainer')) {
        return;
    }

    container.innerHTML = `
        <div style="display:flex; align-items:center; gap:8px; margin-bottom:16px; padding:12px; background:rgba(255,255,255,0.03); border:1px solid var(--border); border-radius:var(--radius-sm);">
            <span style="font-size:13px; color:var(--text); white-space:nowrap;">Hex搜索</span>
            <input type="text" id="proxyHexSearchInput" class="input" placeholder="输入Hex，例如 7E 00 12 或 7E0012" style="flex:1; min-width:0;">
            <button id="proxyHexSearchButton" class="btn btn-primary">搜索</button>
            <button id="proxyHexSearchClearButton" class="btn btn-ghost">清空</button>
        </div>
        <div class="toolbar proxy-data-toolbar" style="margin-bottom:16px;">
            <label style="display:flex; align-items:center; gap:8px; margin-left:12px; color:var(--text); font-size:13px; cursor:pointer;">
                <input type="checkbox" id="proxyRealtimeToggle" checked style="width:16px; height:16px; cursor:pointer;">
                <span>实时更新/自动滚动</span>
            </label>
            <span class="proxy-direction-legend" style="color:var(--text-secondary); font-size:12px; margin-left:auto;">
                <span style="color:#4a9eff;">■</span> 请求 (客户端→服务器) &nbsp;&nbsp;
                <span style="color:#ffd700;">■</span> 响应 (服务器→客户端)
            </span>
        </div>
        <div id="proxyRawStatusBar" class="proxy-status-bar" style="display:none;"></div>
        <div id="proxyPerfStatus" class="info-hint" style="margin-bottom:16px;"></div>
        <div id="proxyDataTableContainer"></div>
    `;
    container.dataset.shellMode = 'raw';
}

function ensureCharlesShell(container) {
    if (container.dataset.shellMode === 'charles' && $('charlesSessionList') && $('charlesDetailBody')) {
        applyCharlesLayoutWidth();
        bindCharlesSplitter();
        return;
    }

    container.innerHTML = `
        <div class="charles-shell">
            <div class="charles-workspace" id="charlesWorkspace">
                <div class="charles-pane charles-pane-list">
                    <div class="charles-pane-header">
                        <div class="charles-pane-title">Charles 会话列表</div>
                        <div class="charles-pane-meta" id="charlesSessionMeta">0 个会话</div>
                    </div>
                    <div class="charles-session-list" id="charlesSessionList"></div>
                </div>
                <div class="charles-splitter" id="charlesSplitter" title="拖拽调整列表/详情宽度"></div>
                <div class="charles-pane charles-pane-detail">
                    <div class="charles-pane-header">
                        <div class="charles-pane-title">会话详情</div>
                        <div class="charles-pane-meta" id="charlesDetailMeta">请选择一个会话</div>
                    </div>
                    <div class="charles-detail-tabs">
                        <button id="charlesDetailTabOverview" class="charles-detail-tab" onclick="setCharlesDetailTab('overview')">Overview</button>
                        <button id="charlesDetailTabContents" class="charles-detail-tab" onclick="setCharlesDetailTab('contents')">Contents</button>
                        <button id="charlesDetailTabSummary" class="charles-detail-tab" onclick="setCharlesDetailTab('summary')">Summary</button>
                        <button id="charlesDetailTabChart" class="charles-detail-tab" onclick="setCharlesDetailTab('chart')">Chart</button>
                        <button id="charlesDetailTabNotes" class="charles-detail-tab" onclick="setCharlesDetailTab('notes')">Notes</button>
                    </div>
                    <div class="charles-detail-body" id="charlesDetailBody"></div>
                </div>
            </div>
        </div>
    `;
    container.dataset.shellMode = 'charles';
    applyCharlesLayoutWidth();
    bindCharlesSplitter();
}

function updateProxyDataPerfStatus() {
    const statusEl = $('proxyPerfStatus');
    if (!statusEl) return;

    const realtimeText = proxyRealtimeEnabled ? '实时推送开启' : '已暂停订阅';
    const fallbackText = proxyRealtimeEnabled ? '10秒无推送才兜底拉取' : '暂停期间仅手动刷新';
    statusEl.innerHTML = `
        <strong>性能模式</strong>：虚拟渲染 + 批量推送 + ${realtimeText}<br>
        <span style="color:var(--text-secondary);">桌面端仅渲染可见行；后端按订阅推送，${fallbackText}。</span>
    `;
}

function updateProxyDataSummaryText() {
    const summaryEl = $('proxySubviewSummary');
    const statsEl = $('proxyDataStatsHint');
    if (summaryEl) {
        summaryEl.textContent = currentProxySubview === 'charles'
            ? 'Charles 子视图基于同一原始包记录派生，不引入第二套数据源'
            : '原始代理包记录视图，与 Charles 子视图共用同一底层数据';
    }
    if (statsEl) {
        const loadedCount = Array.isArray(window._proxyPackets) ? window._proxyPackets.length : 0;
        const totalCount = Number(currentProxyTotalCount || loadedCount || 0);
        const returnedCount = Number(currentProxyReturnedCount || 0);
        const recordText = currentProxyRecordEnabled ? '已开启' : '未开启';
        const selectedText = selectedProxyInstanceId || '<none>';
        const backendInstanceText = currentProxyLastBackendInstanceId || '<none>';
        const backendText = `后端实例:${backendInstanceText} | 后端总数:${currentProxyLastBackendTotalCount} | 最近返回:${currentProxyLastBackendReturnedCount}`;
        const localText = `记录:${recordText} | 当前选中:${selectedText} | 前端已载入:${loadedCount} | 当前总数:${totalCount} | 当前返回:${returnedCount}`;
        const dropText = currentProxyLastDropReason ? ` | 丢弃:${currentProxyLastDropReason}` : '';
        statsEl.textContent = `${localText} | ${backendText}${dropText}`;
    }
}

function setProxySharedControlsState(options = {}) {
    const sharedRecordCheckbox = $('proxySharedRecordEnabled');
    const sharedBufferSizeInput = $('proxySharedBufferSize');
    const sharedSaveButton = $('proxySharedSaveButton');
    const sharedRefreshButton = $('proxySharedRefreshButton');
    const sharedClearButton = $('proxySharedClearButton');
    const disabled = options.disabled === true;

    if (sharedRecordCheckbox) {
        if (typeof options.checked === 'boolean') {
            sharedRecordCheckbox.checked = options.checked;
        }
        sharedRecordCheckbox.disabled = disabled;
    }
    if (sharedBufferSizeInput) {
        if (options.bufferSize !== undefined) {
            sharedBufferSizeInput.value = String(options.bufferSize);
        }
        sharedBufferSizeInput.disabled = disabled;
    }
    if (sharedSaveButton) sharedSaveButton.disabled = disabled;
    if (sharedRefreshButton) sharedRefreshButton.disabled = disabled;
    if (sharedClearButton) sharedClearButton.disabled = disabled;
}

function updateProxyDataPanelShell(instanceId, instanceName) {
    const content = $('proxyDataPanelContent');
    if (!content) return false;

    ensureProxyDataPanelShell(content);
    const subviewContainer = $('proxySubviewContent');

    const previousInstanceId = content.dataset.instanceId || '';
    const instanceNameEl = $('proxyDataInstanceName');
    const instanceIdEl = $('proxyDataInstanceId');

    if (instanceNameEl) instanceNameEl.textContent = '实例: ' + instanceName;
    if (instanceIdEl) instanceIdEl.textContent = '(ID: ' + instanceId + ')';
    updateProxyDataSummaryText();

    const sharedRecordCheckbox = $('proxySharedRecordEnabled');
    const sharedBufferSizeInput = $('proxySharedBufferSize');
    const sharedSaveButton = $('proxySharedSaveButton');
    const sharedRefreshButton = $('proxySharedRefreshButton');
    const sharedClearButton = $('proxySharedClearButton');
    setProxySharedControlsState({
        disabled: false,
        checked: !!currentProxyRecordEnabled,
        bufferSize: currentProxyBufferSize || 200
    });
    if (sharedRecordCheckbox) {
        sharedRecordCheckbox.setAttribute('onchange', `saveProxyDataSettings('${instanceId}')`);
    }
    if (sharedSaveButton) {
        sharedSaveButton.setAttribute('onclick', `saveProxyDataSettings('${instanceId}')`);
    }
    if (sharedRefreshButton) {
        sharedRefreshButton.setAttribute('onclick', `refreshProxyData('${instanceId}')`);
    }
    if (sharedClearButton) {
        sharedClearButton.setAttribute('onclick', `clearProxyDataForInstance('${instanceId}')`);
    }

    if (subviewContainer) {
        if (currentProxySubview === 'charles') {
            ensureCharlesShell(subviewContainer);
        } else {
            ensureProxyRawShell(subviewContainer);
            const realtimeToggle = $('proxyRealtimeToggle');
            const searchInput = $('proxyHexSearchInput');
            const searchButton = $('proxyHexSearchButton');
            const searchClearButton = $('proxyHexSearchClearButton');
            if (realtimeToggle) {
                realtimeToggle.checked = proxyRealtimeEnabled;
                realtimeToggle.setAttribute('onchange', `toggleProxyRealtime('${instanceId}')`);
            }
            if (searchInput) searchInput.value = currentProxyHexSearch;
            if (searchButton) searchButton.setAttribute('onclick', `searchProxyData('${instanceId}')`);
            if (searchClearButton) searchClearButton.setAttribute('onclick', `clearProxyDataSearch('${instanceId}')`);
            updateProxyDataPerfStatus();
        }
    }

    content.dataset.instanceId = instanceId;
    content.dataset.instanceName = instanceName;
    return previousInstanceId !== instanceId;
}

function renderCurrentProxySubview() {
    if (!selectedProxyInstanceId) {
        return;
    }

    if (currentProxySubview === 'charles') {
        renderCharlesCurrentState();
    } else {
        renderProxyRawCurrentState();
    }
}

function renderProxyRawCurrentState() {
    const container = $('proxyDataTableContainer');
    const statusBar = $('proxyRawStatusBar');
    if (!container) return;

    const packets = Array.isArray(window._proxyPackets) ? window._proxyPackets : [];
    const renderKey = currentProxyProjectionKey || [
        selectedProxyInstanceId || '',
        currentProxyHexSearch || '',
        currentProxyRecordEnabled ? 1 : 0,
        currentProxyBufferSize || 0,
        packets.length,
        currentProxyTotalCount || packets.length
    ].join('|');

    const enabledCheckbox = $('proxyRecordEnabled');
    const bufferSizeInput = $('proxyBufferSize');
    if (enabledCheckbox) {
        enabledCheckbox.checked = !!currentProxyRecordEnabled;
    }
    if (bufferSizeInput) {
        bufferSizeInput.value = String(currentProxyBufferSize || 200);
    }

    if (statusBar) {
        const sequenceText = currentProxyFocusedSequence ? `当前定位序号 #${currentProxyFocusedSequence}` : '';
        statusBar.style.display = currentProxyFocusedSequence ? 'flex' : 'none';
        statusBar.innerHTML = currentProxyFocusedSequence
            ? `<span>已从 Charles 定位到原始包：${sequenceText}</span><button class="btn btn-ghost" type="button" onclick="clearProxyFocusHighlight()">清除高亮</button>`
            : '';
    }

    if (renderKey === lastProxyDataRenderKey) {
        return;
    }
    lastProxyDataRenderKey = renderKey;

    if (packets.length === 0) {
        const emptyText = currentProxyRecordEnabled
            ? '暂无数据包记录。若刚保存过记录设置，请注意保存会清空旧缓存；请确认客户端已连接当前 SOCKSForward 实例端口，并重新产生一笔新流量。'
            : '数据包记录未开启，请勾选"记录数据包"开始记录';
        if (container.dataset.mode !== 'empty' || container.dataset.emptyText !== emptyText) {
            container.innerHTML = '<div class="info-hint">' + emptyText + '</div>';
            container.dataset.mode = 'empty';
            container.dataset.emptyText = emptyText;
        }
        lastProxyPacketRowKeys = [];
        return;
    }

    ensureProxyDataTableShell(container);
    renderProxyDataVirtualRows(packets, currentProxyShouldStickToBottom);
}

function formatProxyBytesShort(bytes) {
    const value = Number(bytes) || 0;
    if (value >= 1024 * 1024) {
        return `${(value / (1024 * 1024)).toFixed(2)} MB`;
    }
    if (value >= 1024) {
        return `${(value / 1024).toFixed(1)} KB`;
    }
    return `${value} B`;
}

function parseProxyPacketTimestampMs(timestamp) {
    const match = /^(\d{2}):(\d{2}):(\d{2})$/.exec(timestamp || '');
    if (!match) return null;
    return ((parseInt(match[1], 10) * 60 + parseInt(match[2], 10)) * 60 + parseInt(match[3], 10)) * 1000;
}

function decodeProxyPacketPreviewText(preview) {
    const bytes = parseHexBytesForDetail(preview || '');
    if (!bytes.length) return '';

    return bytes.map(hex => {
        const value = parseInt(hex, 16);
        if (value === 9 || value === 10 || value === 13 || (value >= 32 && value <= 126)) {
            return String.fromCharCode(value);
        }
        return '.';
    }).join('');
}

function sniffProxyPacketMeta(pkt) {
    const previewText = decodeProxyPacketPreviewText(pkt?.dataPreview || '');
    const firstLine = (previewText.split(/\r?\n/).find(line => line.trim()) || '').trim();
    const requestMatch = firstLine.match(/^(GET|POST|PUT|DELETE|PATCH|HEAD|OPTIONS|TRACE|CONNECT)\s+(\S+)\s+HTTP\/[0-9.]+$/i);
    const responseMatch = firstLine.match(/^HTTP\/[0-9.]+\s+(\d{3})(?:\s+([^\r\n]+))?/i);
    const hostMatch = previewText.match(/(?:^|\r?\n)Host:\s*([^\r\n]+)/i);
    const contentTypeMatch = previewText.match(/(?:^|\r?\n)Content-Type:\s*([^\r\n;]+)/i);

    return {
        previewText,
        firstLine,
        method: requestMatch ? requestMatch[1].toUpperCase() : '',
        path: requestMatch ? requestMatch[2] : '',
        statusCode: responseMatch ? responseMatch[1] : '',
        statusText: responseMatch ? (responseMatch[2] || '').trim() : '',
        host: hostMatch ? hostMatch[1].trim() : '',
        contentType: contentTypeMatch ? contentTypeMatch[1].trim() : '',
        isHttpLike: !!(requestMatch || responseMatch || hostMatch || contentTypeMatch),
        effectiveHost: hostMatch ? hostMatch[1].trim() : (pkt?.sniHost || pkt?.targetHost || ''),
        effectivePort: Number(pkt?.targetPort) || 0
    };
}

function createCharlesSession(baseKey, pkt, meta, packetMs, sessionIndex) {
    return {
        sessionId: `charles-${baseKey}-${pkt.sequence || sessionIndex}-${sessionIndex}`,
        baseKey,
        instanceId: selectedProxyInstanceId || '',
        connectionId: pkt.connectionId || 0,
        username: pkt.username || '',
        gameID: pkt.gameID || '',
        clientIP: pkt.clientIP || '',
        host: meta.effectiveHost || '',
        targetHost: pkt.targetHost || '',
        sniHost: pkt.sniHost || '',
        targetPort: meta.effectivePort || 0,
        method: meta.method || '',
        path: meta.path || '',
        statusCode: meta.statusCode || '',
        statusText: meta.statusText || '',
        contentType: meta.contentType || '',
        protocolType: meta.isHttpLike ? (pkt.sslMitmEnabled ? 'https' : 'http') : 'tcp',
        parseState: 'unparsed',
        startTimestamp: pkt.timestamp || '',
        endTimestamp: pkt.timestamp || '',
        startMs: packetMs,
        endMs: packetMs,
        firstRequestMs: pkt.isRequest ? packetMs : null,
        lastRequestMs: pkt.isRequest ? packetMs : null,
        firstResponseMs: pkt.isRequest ? null : packetMs,
        lastResponseMs: pkt.isRequest ? null : packetMs,
        requestCount: 0,
        responseCount: 0,
        requestBytes: 0,
        responseBytes: 0,
        packetSequences: [],
        requestSequences: [],
        responseSequences: [],
        packetSummaries: [],
        requestPreviewText: pkt.isRequest ? (meta.firstLine || meta.previewText || '') : '',
        responsePreviewText: !pkt.isRequest ? (meta.firstLine || meta.previewText || '') : '',
        summary: meta.firstLine || pkt.dataPreview || '',
        firstSequence: pkt.sequence || 0,
        lastSequence: pkt.sequence || 0,
        durationMs: 0,
        displayHost: '',
        displayPath: '',
        structureGroup: ''
    };
}

function shouldStartNewCharlesSession(session, pkt, meta, packetMs) {
    if (!session) {
        return true;
    }

    if (session.connectionId && pkt.connectionId && session.connectionId !== pkt.connectionId) {
        return true;
    }

    // 对于同一连接上的 generic TCP / 未解析流量，不主动切段；
    // 连接不断开，就保持在同一个“未知数据传输会话”里。
    if (session.connectionId && pkt.connectionId && session.connectionId === pkt.connectionId) {
        const currentIsGeneric = session.protocolType === 'tcp' || session.parseState === 'unparsed';
        if (currentIsGeneric && !meta.isHttpLike) {
            return false;
        }
    }

    if (packetMs !== null && session.endMs !== null) {
        const delta = packetMs - session.endMs;
        if (delta < 0 || delta > CHARLES_SESSION_IDLE_GAP_MS) {
            return true;
        }
    }

    if (pkt.isRequest && session.responseCount > 0) {
        return true;
    }

    if (pkt.isRequest && session.requestCount > 0 && session.responseCount === 0) {
        const hostChanged = meta.host && session.host && meta.host !== session.host;
        const methodChanged = meta.method && session.method && meta.method !== session.method;
        if (hostChanged || methodChanged || session.packetSequences.length >= 4) {
            return true;
        }
    }

    return false;
}

function appendPacketToCharlesSession(session, pkt, meta, packetMs) {
    session.packetSequences.push(pkt.sequence || 0);
    session.packetSummaries.push({
        sequence: pkt.sequence || 0,
        timestamp: pkt.timestamp || '',
        username: pkt.username || '',
        gameID: pkt.gameID || '',
        isRequest: !!pkt.isRequest,
        dataLength: pkt.dataLength || 0,
        dataPreview: pkt.dataPreview || '',
        firstLine: meta.firstLine || '',
        previewText: meta.previewText || ''
    });

    session.lastSequence = pkt.sequence || session.lastSequence;
    session.endTimestamp = pkt.timestamp || session.endTimestamp;
    session.endMs = packetMs;
    session.summary = session.summary || meta.firstLine || pkt.dataPreview || '';

    if (!session.host && meta.effectiveHost) session.host = meta.effectiveHost;
    if (!session.method && meta.method) session.method = meta.method;
    if (!session.path && meta.path) session.path = meta.path;
    if (!session.statusCode && meta.statusCode) session.statusCode = meta.statusCode;
    if (!session.statusText && meta.statusText) session.statusText = meta.statusText;
    if (!session.contentType && meta.contentType) session.contentType = meta.contentType;
    if (!session.targetHost && pkt.targetHost) session.targetHost = pkt.targetHost;
    if (!session.sniHost && pkt.sniHost) session.sniHost = pkt.sniHost;
    if (!session.targetPort && pkt.targetPort) session.targetPort = pkt.targetPort;
    if (!session.clientIP && pkt.clientIP) session.clientIP = pkt.clientIP;
    if (meta.isHttpLike) session.protocolType = pkt.sslMitmEnabled ? 'https' : 'http';

    if (pkt.isRequest) {
        session.requestCount += 1;
        session.requestBytes += pkt.dataLength || 0;
        session.requestSequences.push(pkt.sequence || 0);
        if (!session.requestPreviewText) {
            session.requestPreviewText = meta.firstLine || meta.previewText || '';
        }
        if (packetMs !== null) {
            if (session.firstRequestMs === null) session.firstRequestMs = packetMs;
            session.lastRequestMs = packetMs;
        }
    } else {
        session.responseCount += 1;
        session.responseBytes += pkt.dataLength || 0;
        session.responseSequences.push(pkt.sequence || 0);
        if (!session.responsePreviewText) {
            session.responsePreviewText = meta.firstLine || meta.previewText || '';
        }
        if (packetMs !== null) {
            if (session.firstResponseMs === null) session.firstResponseMs = packetMs;
            session.lastResponseMs = packetMs;
        }
    }
}

function finalizeCharlesSession(session) {
    session.durationMs = (session.startMs !== null && session.endMs !== null)
        ? Math.max(0, session.endMs - session.startMs)
        : 0;

    session.parseState = (session.protocolType === 'http' || session.protocolType === 'https')
        ? (session.requestCount > 0 && session.responseCount > 0 ? 'complete' : 'partial')
        : 'unparsed';

    session.displayHost = session.host || session.sniHost || session.targetHost || (session.protocolType === 'http' || session.protocolType === 'https'
        ? '未识别 Host'
        : `${session.username || '匿名用户'} / ${session.gameID || '无GameID'}`);

    session.displayPath = session.path
        || (session.statusCode ? `${session.statusCode}${session.statusText ? ' ' + session.statusText : ''}` : '')
        || (session.summary || '未解析流');

    session.structureGroup = session.displayHost || CHARLES_UNPARSED_GROUP_LABEL;
}

function buildCharlesEndpointLabel(session) {
    const host = session.targetHost || session.sniHost || session.displayHost || '未知目标';
    return session.targetPort ? `${host}:${session.targetPort}` : host;
}

function buildCharlesSessionDisplayTitle(session, indexInGroup) {
    if (session.method) {
        return `${session.method} ${session.path || session.displayPath || '/'}`;
    }
    if (session.protocolType === 'http' || session.protocolType === 'https') {
        return session.displayPath || `${session.protocolType.toUpperCase()} 会话 #${indexInGroup}`;
    }
    return '<unknown>';
}

function buildCharlesSessionsFromPackets(packets) {
    const sessions = [];
    const activeByBase = new Map();

    (packets || []).forEach((pkt, index) => {
        const meta = sniffProxyPacketMeta(pkt);
        const packetMs = parseProxyPacketTimestampMs(pkt.timestamp);
        const baseKey = pkt.connectionId
            ? `conn:${pkt.connectionId}`
            : `${pkt.username || '-'}|${pkt.gameID || '-'}|${meta.effectiveHost || ''}|${meta.effectivePort || 0}`;

        let session = activeByBase.get(baseKey);
        if (shouldStartNewCharlesSession(session, pkt, meta, packetMs)) {
            session = createCharlesSession(baseKey, pkt, meta, packetMs, index + 1);
            sessions.push(session);
            activeByBase.set(baseKey, session);
        }

        appendPacketToCharlesSession(session, pkt, meta, packetMs);
    });

    sessions.forEach(finalizeCharlesSession);
    return sessions;
}

function getCharlesSessionsForCurrentProxyView() {
    const packets = Array.isArray(window._proxyPackets) ? window._proxyPackets : [];
    const projectionKey = currentProxyProjectionKey || [
        selectedProxyInstanceId || '',
        packets.length,
        packets.length ? buildProxyPacketRowKey(packets[0]) : '',
        packets.length ? buildProxyPacketRowKey(packets[packets.length - 1]) : ''
    ].join('|');

    if (projectionKey !== cachedCharlesProjectionKey) {
        cachedCharlesProjectionKey = projectionKey;
        cachedCharlesSessions = buildCharlesSessionsFromPackets(packets);
    }

    return cachedCharlesSessions;
}

function filterCharlesSessions(sessions) {
    return Array.isArray(sessions) ? sessions : [];
}

function getAvailableCharlesFocusHosts(sessions) {
    const hosts = Array.from(new Set(
        (sessions || [])
            .map(session => session.displayHost || '')
            .filter(Boolean)
    )).sort((a, b) => a.localeCompare(b, 'zh-CN'));

    return hosts;
}

function getAvailableCharlesMethods(sessions) {
    return Array.from(new Set(
        (sessions || [])
            .map(session => (session.method || '').toUpperCase())
            .filter(Boolean)
    )).sort((a, b) => a.localeCompare(b));
}

function getAvailableCharlesStatuses(sessions) {
    const exactStatuses = Array.from(new Set(
        (sessions || [])
            .map(session => String(session.statusCode || ''))
            .filter(Boolean)
    )).sort((a, b) => Number(a) - Number(b));

    const statusFamilies = Array.from(new Set(
        exactStatuses.map(code => `${code.charAt(0)}xx`)
    )).sort((a, b) => a.localeCompare(b));

    return [...statusFamilies, ...exactStatuses];
}

function getAvailableCharlesContentTypes(sessions) {
    return Array.from(new Set(
        (sessions || [])
            .map(session => (session.contentType || '').split(';')[0].trim())
            .filter(Boolean)
    )).sort((a, b) => a.localeCompare(b));
}

function syncCharlesFocusHostOptions(sessions) {
    const focusHostSelect = $('charlesFocusHostFilter');
    if (!focusHostSelect) return;

    const hosts = getAvailableCharlesFocusHosts(sessions);
    if (currentCharlesFocusHost !== 'all' && !hosts.includes(currentCharlesFocusHost)) {
        currentCharlesFocusHost = 'all';
    }

    const desiredOptions = ['all', ...hosts];
    const currentOptions = Array.from(focusHostSelect.options).map(option => option.value);
    const optionsChanged = desiredOptions.length !== currentOptions.length
        || desiredOptions.some((value, index) => currentOptions[index] !== value);

    if (optionsChanged) {
        focusHostSelect.innerHTML = [
            '<option value="all">Focus Host: 全部</option>',
            ...hosts.map(host => `<option value="${escapeHtmlInline(host)}">${escapeHtmlInline(host)}</option>`)
        ].join('');
    }

    focusHostSelect.value = currentCharlesFocusHost;
}

function syncCharlesGenericSelect(selectId, currentValue, labelPrefix, values) {
    const select = $(selectId);
    if (!select) return currentValue;

    const normalizedValues = ['all', ...(values || [])];
    const currentOptions = Array.from(select.options).map(option => option.value);
    const optionsChanged = normalizedValues.length !== currentOptions.length
        || normalizedValues.some((value, index) => currentOptions[index] !== value);

    if (optionsChanged) {
        select.innerHTML = [
            `<option value="all">${labelPrefix}: 全部</option>`,
            ...(values || []).map(value => `<option value="${escapeHtmlInline(value)}">${escapeHtmlInline(value)}</option>`)
        ].join('');
    }

    if (currentValue !== 'all' && !(values || []).includes(currentValue)) {
        currentValue = 'all';
    }
    select.value = currentValue;
    return currentValue;
}

function getCurrentCharlesSelection(filteredSessions) {
    if (!filteredSessions.length) {
        currentCharlesSelectedSessionId = '';
        return null;
    }

    let selected = filteredSessions.find(session => session.sessionId === currentCharlesSelectedSessionId);
    if (!selected) {
        selected = filteredSessions[0];
        currentCharlesSelectedSessionId = selected.sessionId;
    }
    return selected;
}

function buildCharlesSessionMetaItem(label, value) {
    return `
        <div class="charles-session-meta-item">
            <div class="charles-session-meta-label">${escapeHtmlInline(label)}</div>
            <div class="charles-session-meta-value">${escapeHtmlInline(value || '-')}</div>
        </div>
    `;
}

function buildCharlesSessionItemHtml(session, options = {}) {
    const statusText = session.statusCode
        ? `${session.statusCode}${session.statusText ? ' ' + session.statusText : ''}`
        : (session.parseState === 'unparsed' ? '未解析' : '处理中');
    const titleText = options.title || session.displayHost;
    const mainLine = options.secondaryLine !== undefined
        ? options.secondaryLine
        : (session.method ? `${session.method} ${session.displayPath}` : session.displayPath);
    const trafficText = `${formatProxyBytesShort(session.requestBytes)} / ${formatProxyBytesShort(session.responseBytes)}`;
    const submetaParts = Array.isArray(options.submetaParts)
        ? options.submetaParts
        : [
            session.startTimestamp || '-',
            statusText,
            trafficText,
            session.username || '-'
        ];

    return `
        <div class="charles-session-item ${session.sessionId === currentCharlesSelectedSessionId ? 'active' : ''}" onclick="selectCharlesSession('${session.sessionId}')">
            <div class="charles-session-row">
                <div class="charles-session-compact-main">
                    <div class="charles-session-host">${escapeHtmlInline(titleText)}</div>
                    ${mainLine ? `<div class="charles-session-path">${escapeHtmlInline(mainLine)}</div>` : ''}
                    <div class="charles-session-submeta">
                        ${submetaParts.map(part => `<span>${escapeHtmlInline(part)}</span>`).join('')}
                    </div>
                </div>
            </div>
        </div>
    `;
}

function buildCharlesTreeHtml(sessions) {
    const endpointGroups = new Map();

    sessions.forEach(session => {
        const endpointLabel = buildCharlesEndpointLabel(session);
        if (!endpointGroups.has(endpointLabel)) {
            endpointGroups.set(endpointLabel, []);
        }
        endpointGroups.get(endpointLabel).push(session);
    });

    const sortedGroups = Array.from(endpointGroups.entries()).sort((a, b) => a[0].localeCompare(b[0], 'zh-CN'));

    return sortedGroups.map(([endpointLabel, groupSessions]) => {
        const isCollapsed = currentCharlesCollapsedEndpoints.has(endpointLabel);
        const encodedEndpointKey = encodeCharlesEndpointKey(endpointLabel);
        const sortedSessions = [...groupSessions].sort((a, b) => {
            const aStart = a.startMs ?? 0;
            const bStart = b.startMs ?? 0;
            if (aStart !== bStart) return aStart - bStart;
            return (a.firstSequence || 0) - (b.firstSequence || 0);
        });
        const hasActiveChild = sortedSessions.some(session => session.sessionId === currentCharlesSelectedSessionId);

        return `
            <div class="charles-structure-group ${hasActiveChild ? 'active' : ''}">
                <div class="charles-structure-header" onclick="toggleCharlesEndpointByKey('${encodedEndpointKey}')">
                    <div class="charles-structure-title-wrap">
                        <span class="charles-tree-toggle">${isCollapsed ? '▸' : '▾'}</span>
                        <div class="charles-structure-title">${escapeHtmlInline(endpointLabel)}</div>
                    </div>
                    <div class="charles-structure-count">${sortedSessions.length} 个会话</div>
                </div>
                <div class="charles-structure-body${isCollapsed ? ' is-collapsed' : ''}">
                    ${sortedSessions.map((session, index) => {
                        const titleText = buildCharlesSessionDisplayTitle(session, index + 1);
                        const secondaryLine = session.summary && session.summary !== session.displayPath ? session.summary : '';
                        const submetaParts = [
                            session.startTimestamp || '-',
                            session.protocolType === 'https' ? 'HTTPS' : session.protocolType === 'http' ? 'HTTP' : 'TCP',
                            session.statusCode || (session.parseState === 'unparsed' ? 'Unparsed' : '-'),
                            session.username || '-'
                        ];
                        return `
                            <div class="charles-tree-child ${session.sessionId === currentCharlesSelectedSessionId ? 'active' : ''}">
                                ${buildCharlesSessionItemHtml(session, {
                                    title: titleText,
                                    secondaryLine,
                                    submetaParts
                                })}
                            </div>
                        `;
                    }).join('')}
                </div>
            </div>
        `;
    }).join('');
}

function getProxyPacketBySequence(sequence) {
    return (window._proxyPackets || []).find(pkt => String(pkt.sequence) === String(sequence)) || null;
}

function openCharlesPacketHex(sequence) {
    const packets = window._proxyPackets || [];
    const index = packets.findIndex(pkt => String(pkt.sequence) === String(sequence));
    if (index < 0) {
        showToast('warning', '提示', '未找到对应的原始包记录');
        return;
    }
    showPacketHexDetail(index);
}

function switchToRawForCharlesSession(sessionId) {
    currentCharlesSelectedSessionId = sessionId || currentCharlesSelectedSessionId;
    const sessions = getCharlesSessionsForCurrentProxyView();
    const selectedSession = sessions.find(session => session.sessionId === currentCharlesSelectedSessionId);
    const preferredSequence = selectedSession && Array.isArray(selectedSession.requestSequences) && selectedSession.requestSequences.length > 0
        ? selectedSession.requestSequences[0]
        : selectedSession && Array.isArray(selectedSession.packetSequences) && selectedSession.packetSequences.length > 0
            ? selectedSession.packetSequences[0]
            : '';
    currentProxyFocusedSequence = preferredSequence ? String(preferredSequence) : '';
    currentProxyNeedsFocusScroll = !!currentProxyFocusedSequence;
    if (currentProxyFocusedSequence) {
        scheduleProxyFocusClear();
    }
    switchProxySubview('raw');
}

function buildCharlesPacketListHtml(sequences, emptyText) {
    if (!Array.isArray(sequences) || sequences.length === 0) {
        return `<div class="charles-empty-state">${escapeHtmlInline(emptyText)}</div>`;
    }

    return `<div class="charles-packet-list">
        ${sequences.map(sequence => {
            const pkt = getProxyPacketBySequence(sequence);
            if (!pkt) {
                return '';
            }
            const directionLabel = pkt.isRequest ? '请求' : '响应';
            return `
                <div class="charles-packet-item">
                    <div class="charles-packet-top">
                        <div class="charles-detail-value">#${pkt.sequence} · ${escapeHtmlInline(pkt.timestamp || '-')} · ${directionLabel} · ${formatProxyBytesShort(pkt.dataLength)}</div>
                        <button class="charles-link-btn" onclick="openCharlesPacketHex('${pkt.sequence}')">查看 Hex</button>
                    </div>
                    <div class="charles-packet-preview">${escapeHtmlInline(pkt.dataPreview || '')}</div>
                </div>
            `;
        }).join('')}
    </div>`;
}

function getProxyPacketDetailCacheKey(sequence) {
    return String(sequence || '');
}

function ensureProxyPacketDetailBySequence(sequence) {
    const seqKey = getProxyPacketDetailCacheKey(sequence);
    if (!seqKey) return false;
    if (window._proxyPacketDetails.has(seqKey)) return true;
    if (window._proxyPacketDetailRequests.has(seqKey)) return false;
    if (!selectedProxyInstanceId) return false;

    window._proxyPacketDetailRequests.add(seqKey);
    postAction('config_get_proxydata_detail', {
        instanceId: selectedProxyInstanceId,
        index: 0,
        sequence: Number(sequence) || 0
    });
    return false;
}

function getCharlesSessionPacketHexMap(session) {
    const packetHexMap = new Map();
    const missingSequences = [];

    (session.packetSequences || []).forEach(sequence => {
        const seqKey = getProxyPacketDetailCacheKey(sequence);
        if (window._proxyPacketDetails.has(seqKey)) {
            packetHexMap.set(seqKey, window._proxyPacketDetails.get(seqKey));
        } else {
            missingSequences.push(sequence);
        }
    });

    return { packetHexMap, missingSequences };
}

function buildCharlesParsedSessionArtifacts(session) {
    const { packetHexMap, missingSequences } = getCharlesSessionPacketHexMap(session);
    if (missingSequences.length > 0) {
        missingSequences.forEach(sequence => ensureProxyPacketDetailBySequence(sequence));
        return {
            ready: false,
            missingSequences
        };
    }

    const combineHex = (sequences) => sequences
        .map(sequence => packetHexMap.get(getProxyPacketDetailCacheKey(sequence)) || '')
        .filter(Boolean)
        .join(' ');

    const requestHex = combineHex(session.requestSequences || []);
    const responseHex = combineHex(session.responseSequences || []);
    const requestMessage = requestHex ? parseHttpMessageFromHex(requestHex, 'request') : { ok: false, reason: 'no-request' };
    const responseMessage = responseHex ? parseHttpMessageFromHex(responseHex, 'response') : { ok: false, reason: 'no-response' };

    return {
        ready: true,
        requestHex,
        responseHex,
        requestMessage,
        responseMessage
    };
}

function getCharlesSessionParsedArtifacts(session) {
    const cacheKey = `${selectedProxyInstanceId || ''}|${session.sessionId}|${(session.packetSequences || []).join(',')}`;
    const cached = window._charlesSessionParseCache.get(cacheKey);
    if (cached) {
        return cached;
    }

    const artifacts = buildCharlesParsedSessionArtifacts(session);
    window._charlesSessionParseCache.set(cacheKey, artifacts);
    return artifacts;
}

function invalidateCharlesSessionParseCache() {
    window._charlesSessionParseCache.clear();
}

function formatCharlesHeadersHtml(headers) {
    if (!Array.isArray(headers) || headers.length === 0) {
        return '<div class="charles-detail-muted">无可解析的 Header</div>';
    }

    return `<div class="charles-packet-list">
        ${headers.map(header => `
            <div class="charles-packet-item">
                <div class="charles-packet-top">
                    <div class="charles-detail-value">${escapeHtmlInline(header.name)}</div>
                </div>
                <div class="charles-packet-preview">${escapeHtmlInline(header.value)}</div>
            </div>
        `).join('')}
    </div>`;
}

function formatCharlesBodySection(title, parsedMessage) {
    if (!parsedMessage || !parsedMessage.ok) {
        return `
            <div class="charles-detail-section">
                <div class="charles-detail-title">${escapeHtmlInline(title)}</div>
                <div class="charles-detail-muted">当前方向暂无可解析的完整 HTTP/HTTPS 明文消息</div>
            </div>
        `;
    }

    return `
        <div class="charles-detail-section">
            <div class="charles-detail-title">${escapeHtmlInline(title)}</div>
            <div class="charles-overview-grid" style="margin-bottom:12px;">
                ${buildCharlesSessionMetaItem('Start Line', parsedMessage.startLine || '-')}
                ${buildCharlesSessionMetaItem('Content-Type', parsedMessage.contentType || '-')}
                ${buildCharlesSessionMetaItem('Body Bytes', String(parsedMessage.bodyLength || 0))}
                ${buildCharlesSessionMetaItem('Body Format', parsedMessage.bodyFormat || '-')}
            </div>
            <div class="charles-detail-title">Headers</div>
            ${formatCharlesHeadersHtml(parsedMessage.headers)}
            <div class="charles-detail-title" style="margin-top:14px;">Body</div>
            <pre class="charles-code-block">${escapeHtmlInline(parsedMessage.bodyPreview || '(空)')}</pre>
        </div>
    `;
}

function formatHexAsciiLinesFromHex(hexStr, bytesPerLine = 16) {
    const bytes = hexTokensToByteValues(hexStr || '');
    if (!bytes.length) {
        return '(空)';
    }

    const lines = [];
    for (let offset = 0; offset < bytes.length; offset += bytesPerLine) {
        const chunk = bytes.slice(offset, offset + bytesPerLine);
        const hexPart = chunk
            .map(value => value.toString(16).toUpperCase().padStart(2, '0'))
            .join(' ')
            .padEnd(bytesPerLine * 3 - 1, ' ');
        const asciiPart = chunk
            .map(value => (value >= 0x20 && value <= 0x7E) ? String.fromCharCode(value) : '.')
            .join('');
        lines.push(`${offset.toString(16).toUpperCase().padStart(8, '0')}  ${hexPart}  ${asciiPart}`);
    }

    return lines.join('\n');
}

function buildCharlesNumberedCodeHtml(text) {
    const normalized = String(text || '(空)').replace(/\r\n/g, '\n');
    const lines = normalized.split('\n');
    return `
        <div class="charles-code-shell">
            <div class="charles-code-gutter">
                ${lines.map((_, index) => `<div class="charles-code-line-no">${index + 1}</div>`).join('')}
            </div>
            <div class="charles-code-lines">
                ${lines.map(line => `<div class="charles-code-line">${line ? escapeHtmlInline(line) : '&nbsp;'}</div>`).join('')}
            </div>
        </div>
    `;
}

function formatCharlesStreamSection(title, hexStr, parsedMessage) {
    const byteLength = hexTokensToByteValues(hexStr || '').length;
    return `
        <div class="charles-detail-section">
            <div class="charles-detail-title">${escapeHtmlInline(title)}</div>
            <div class="charles-overview-grid" style="margin-bottom:12px;">
                ${buildCharlesSessionMetaItem('累计字节', String(byteLength))}
                ${buildCharlesSessionMetaItem('Start Line', parsedMessage?.startLine || '-')}
                ${buildCharlesSessionMetaItem('Content-Type', parsedMessage?.contentType || '-')}
                ${buildCharlesSessionMetaItem('Body Format', parsedMessage?.bodyFormat || '-')}
            </div>
            <pre class="charles-code-block">${escapeHtmlInline(formatHexAsciiLinesFromHex(hexStr))}</pre>
        </div>
    `;
}

function formatCharlesHeadersOnlySection(title, parsedMessage) {
    if (!parsedMessage || !parsedMessage.ok) {
        return `
            <div class="charles-detail-section">
                <div class="charles-detail-title">${escapeHtmlInline(title)}</div>
                <div class="charles-detail-muted">当前方向暂无可解析的 Header</div>
            </div>
        `;
    }

    return `
        <div class="charles-detail-section">
            <div class="charles-detail-title">${escapeHtmlInline(title)}</div>
            <div class="charles-overview-grid" style="margin-bottom:12px;">
                ${buildCharlesSessionMetaItem('Start Line', parsedMessage.startLine || '-')}
                ${buildCharlesSessionMetaItem('Header Count', String((parsedMessage.headers || []).length))}
                ${buildCharlesSessionMetaItem('Content-Type', parsedMessage.contentType || '-')}
                ${buildCharlesSessionMetaItem('Content-Length', parsedMessage.contentLength || '-')}
            </div>
            ${formatCharlesHeadersHtml(parsedMessage.headers)}
        </div>
    `;
}

function formatCharlesBodyOnlySection(title, parsedMessage) {
    if (!parsedMessage || !parsedMessage.ok) {
        return `
            <div class="charles-detail-section">
                <div class="charles-detail-title">${escapeHtmlInline(title)}</div>
                <div class="charles-detail-muted">当前方向暂无可解析的 Body</div>
            </div>
        `;
    }

    return `
        <div class="charles-detail-section">
            <div class="charles-detail-title">${escapeHtmlInline(title)}</div>
            <div class="charles-overview-grid" style="margin-bottom:12px;">
                ${buildCharlesSessionMetaItem('Body Bytes', String(parsedMessage.bodyLength || 0))}
                ${buildCharlesSessionMetaItem('Body Format', parsedMessage.bodyFormat || '-')}
                ${buildCharlesSessionMetaItem('Content-Type', parsedMessage.contentType || '-')}
                ${buildCharlesSessionMetaItem('HTTP Version', parsedMessage.httpVersion || '-')}
            </div>
            <pre class="charles-code-block">${escapeHtmlInline(parsedMessage.bodyPreview || '(空)')}</pre>
        </div>
    `;
}

function buildCharlesTimelineHtml(session) {
    const requestWeight = Math.max(1, session.requestCount || 0);
    const responseWeight = Math.max(1, session.responseCount || 0);
    let waitWeight = 1;
    if (session.firstResponseMs !== null && session.lastRequestMs !== null) {
        waitWeight = Math.max(1, Math.round((session.firstResponseMs - session.lastRequestMs) / 1000));
    }

    const totalWeight = requestWeight + responseWeight + waitWeight;
    const requestPercent = (requestWeight / totalWeight) * 100;
    const waitPercent = (waitWeight / totalWeight) * 100;
    const responsePercent = (responseWeight / totalWeight) * 100;

    return `
        <div class="charles-detail-section">
            <div class="charles-detail-title">估算时序</div>
            <div class="charles-timeline">
                <div class="charles-timeline-segment charles-timeline-request" style="width:${requestPercent}%"></div>
                <div class="charles-timeline-segment charles-timeline-wait" style="width:${waitPercent}%"></div>
                <div class="charles-timeline-segment charles-timeline-response" style="width:${responsePercent}%"></div>
            </div>
            <div class="charles-timeline-legend">
                <span>Request: ${session.requestCount} 包</span>
                <span>Latency: ${Math.max(0, (session.firstResponseMs ?? session.endMs ?? 0) - (session.firstRequestMs ?? session.startMs ?? 0))} ms</span>
                <span>Response: ${session.responseCount} 包</span>
                <span>Total: ${session.durationMs} ms</span>
            </div>
        </div>
    `;
}

function buildCharlesOverviewSummaryCards(session, requestMessage, responseMessage) {
    const statusValue = session.statusCode
        ? `${session.statusCode}${session.statusText ? ' ' + session.statusText : ''}`
        : (session.parseState === 'unparsed' ? '未解析' : '处理中');
    const contentType = session.contentType || requestMessage?.contentType || responseMessage?.contentType || '-';

    return `
        <div class="charles-overview-summary">
            <div class="charles-overview-card">
                <div class="charles-overview-card-label">Host</div>
                <div class="charles-overview-card-value">${escapeHtmlInline(session.displayHost)}</div>
            </div>
            <div class="charles-overview-card">
                <div class="charles-overview-card-label">Method / Status</div>
                <div class="charles-overview-card-value">${escapeHtmlInline((session.method || '-') + ' / ' + statusValue)}</div>
            </div>
            <div class="charles-overview-card">
                <div class="charles-overview-card-label">Traffic</div>
                <div class="charles-overview-card-value">${escapeHtmlInline(formatProxyBytesShort(session.requestBytes) + ' -> ' + formatProxyBytesShort(session.responseBytes))}</div>
            </div>
            <div class="charles-overview-card">
                <div class="charles-overview-card-label">Content-Type</div>
                <div class="charles-overview-card-value">${escapeHtmlInline(contentType)}</div>
            </div>
        </div>
    `;
}

function buildCharlesOverviewTable(session, requestMessage, responseMessage) {
    const statusValue = session.statusCode
        ? `${session.statusCode}${session.statusText ? ' ' + session.statusText : ''}`
        : (session.parseState === 'unparsed' ? 'Complete' : 'Processing');
    const protocolValue = session.protocolType === 'https'
        ? 'HTTPS (MITM)'
        : session.protocolType === 'http'
            ? 'HTTP'
            : 'TCP / Unknown';
    const contentType = session.contentType || requestMessage?.contentType || responseMessage?.contentType || '-';
    const requestSize = hexTokensToByteValues(requestMessage ? (getCharlesSessionParsedArtifacts(session).requestHex || '') : '').length;
    const responseSize = hexTokensToByteValues(responseMessage ? (getCharlesSessionParsedArtifacts(session).responseHex || '') : '').length;

    const rows = [
        ['URL', buildCharlesEndpointLabel(session)],
        ['Status', statusValue],
        ['Response Code', session.statusCode || '-'],
        ['Protocol', protocolValue],
        ['Method', session.method || '-'],
        ['Keep Alive', session.responseCount > 0 ? 'Yes' : 'No'],
        ['Content-Type', contentType],
        ['Client Address', session.clientIP || '-'],
        ['Remote Address', buildCharlesEndpointLabel(session)],
        ['Connection ID', session.connectionId ? String(session.connectionId) : '-'],
        ['Request Start Time', session.startTimestamp || '-'],
        ['Response End Time', session.endTimestamp || '-'],
        ['Duration', `${session.durationMs} ms`],
        ['Request Size', requestSize > 0 ? `${formatProxyBytesShort(requestSize)} (${requestSize} bytes)` : '-'],
        ['Response Size', responseSize > 0 ? `${formatProxyBytesShort(responseSize)} (${responseSize} bytes)` : '-']
    ];

    return `
        <div class="charles-detail-section">
            <div class="charles-overview-table">
                ${rows.map(([name, value]) => `
                    <div class="charles-overview-row">
                        <div class="charles-overview-name">${escapeHtmlInline(name)}</div>
                        <div class="charles-overview-value">${escapeHtmlInline(value)}</div>
                    </div>
                `).join('')}
            </div>
        </div>
    `;
}

function buildCharlesContentsPane(title, parsedMessage, accumulatedHex) {
    const direction = title === 'Response' ? 'response' : 'request';
    const activeMode = direction === 'response' ? currentCharlesContentViewResponse : currentCharlesContentViewRequest;
    const bytes = accumulatedHex ? hexTokensToByteValues(accumulatedHex) : [];
    const textOverview = parsedMessage && parsedMessage.ok
        ? [
            parsedMessage.startLine || '',
            ...(parsedMessage.headers || []).map(header => `${header.name}: ${header.value}`),
            '',
            parsedMessage.bodyPreview || ''
        ].join('\n').trim() || '(空)'
        : (bytes.length ? decodeByteArrayUtf8(bytes) : '(空)');
    const hexOverview = accumulatedHex
        ? formatHexAsciiLinesFromHex(accumulatedHex)
        : '(空)';
    const rawOverview = bytes.length
        ? decodeByteArrayLatin1(bytes)
        : '(空)';
    let contentText = textOverview;
    if (activeMode === 'hex') {
        contentText = hexOverview;
    } else if (activeMode === 'raw') {
        contentText = rawOverview;
    }
    const contentHtml = activeMode === 'raw'
        ? `<pre class="charles-code-block charles-code-block-raw">${escapeHtmlInline(contentText)}</pre>`
        : buildCharlesNumberedCodeHtml(contentText);
    return `
        <div class="charles-content-pane">
            <div class="charles-content-pane-header">${escapeHtmlInline(title)}</div>
            <div class="charles-content-pane-tabs">
                <button type="button" class="charles-content-tab ${activeMode === 'text' ? 'active' : ''}" onclick="setCharlesContentTab('${direction}', 'text')">Text</button>
                <button type="button" class="charles-content-tab ${activeMode === 'hex' ? 'active' : ''}" onclick="setCharlesContentTab('${direction}', 'hex')">Hex</button>
                <button type="button" class="charles-content-tab ${activeMode === 'raw' ? 'active' : ''}" onclick="setCharlesContentTab('${direction}', 'raw')">Raw</button>
            </div>
            <div class="charles-content-pane-scroll">
                ${contentHtml}
            </div>
        </div>
    `;
}

function buildCharlesSummaryTable(session, requestMessage, responseMessage) {
    const rows = [
        ['Host', session.displayHost],
        ['Method', session.method || '-'],
        ['Request Start Line', requestMessage?.startLine || '-'],
        ['Response Start Line', responseMessage?.startLine || '-'],
        ['Request Headers', String((requestMessage?.headers || []).length)],
        ['Response Headers', String((responseMessage?.headers || []).length)],
        ['Request Body Bytes', String(requestMessage?.bodyLength || 0)],
        ['Response Body Bytes', String(responseMessage?.bodyLength || 0)],
        ['Packets', String(session.packetSequences.length)],
        ['Traffic', `${formatProxyBytesShort(session.requestBytes)} / ${formatProxyBytesShort(session.responseBytes)}`]
    ];

    return `
        <div class="charles-detail-section">
            <div class="charles-overview-table">
                ${rows.map(([name, value]) => `
                    <div class="charles-overview-row">
                        <div class="charles-overview-name">${escapeHtmlInline(name)}</div>
                        <div class="charles-overview-value">${escapeHtmlInline(value)}</div>
                    </div>
                `).join('')}
            </div>
        </div>
    `;
}

function buildCharlesNotesPane(session) {
    const notes = [];
    if (session.protocolType === 'https' && !session.sniHost) {
        notes.push('SSL Proxying 未为该目标提供可识别的 SNI 主机。');
    }
    if (session.protocolType === 'tcp') {
        notes.push('当前会话为 generic TCP / unknown，会话内容按 Raw 数据流累计展示。');
    }
    if (!notes.length) {
        notes.push('当前会话暂无额外备注。');
    }

    return `
        <div class="charles-detail-section">
            <pre class="charles-code-block">${escapeHtmlInline(notes.join('\n'))}</pre>
        </div>
    `;
}

function buildCharlesComparePane(title, parsedMessage, fallbackText, paneClass) {
    if (!parsedMessage || !parsedMessage.ok) {
        return `
            <div class="charles-compare-pane ${paneClass}">
                <div class="charles-compare-heading">${escapeHtmlInline(title)}</div>
                <div class="charles-detail-muted">${escapeHtmlInline(fallbackText)}</div>
            </div>
        `;
    }

    return `
        <div class="charles-compare-pane ${paneClass}">
            <div class="charles-compare-heading">${escapeHtmlInline(title)}</div>
            <div class="charles-overview-grid" style="margin-bottom:12px;">
                ${buildCharlesSessionMetaItem('Start Line', parsedMessage.startLine || '-')}
                ${buildCharlesSessionMetaItem('Content-Type', parsedMessage.contentType || '-')}
                ${buildCharlesSessionMetaItem('Body Bytes', String(parsedMessage.bodyLength || 0))}
                ${buildCharlesSessionMetaItem('Body Format', parsedMessage.bodyFormat || '-')}
            </div>
            <pre class="charles-code-block">${escapeHtmlInline(parsedMessage.bodyPreview || '(空)')}</pre>
        </div>
    `;
}

function renderCharlesDetail(session) {
    const detailBody = $('charlesDetailBody');
    const detailMeta = $('charlesDetailMeta');
    if (!detailBody) return;

    document.querySelectorAll('.charles-detail-tab').forEach(tab => {
        tab.classList.toggle('active', tab.id === `charlesDetailTab${currentCharlesDetailTab.charAt(0).toUpperCase()}${currentCharlesDetailTab.slice(1)}`);
    });

    if (!session) {
        if (detailMeta) detailMeta.textContent = '请选择一个会话';
        detailBody.innerHTML = '<div class="charles-empty-state">请选择左侧一个会话查看 Charles 风格详情</div>';
        return;
    }

    if (detailMeta) {
        const statusText = session.statusCode
            ? `${session.statusCode}${session.statusText ? ' ' + session.statusText : ''}`
            : (session.parseState === 'unparsed' ? '未解析' : '处理中');
        detailMeta.textContent = `${session.displayHost} · ${statusText} · ${formatProxyBytesShort(session.requestBytes)} / ${formatProxyBytesShort(session.responseBytes)} · ${session.packetSequences.length} 个包`;
    }

    const parsedArtifacts = getCharlesSessionParsedArtifacts(session);
    const requestMessage = parsedArtifacts.ready ? parsedArtifacts.requestMessage : null;
    const responseMessage = parsedArtifacts.ready ? parsedArtifacts.responseMessage : null;
    const loadingHint = !parsedArtifacts.ready
        ? `<div class="charles-detail-section"><div class="charles-detail-title">正在加载完整原始包</div><div class="charles-detail-muted">Charles 正在按需拉取该会话涉及的完整数据包，以解析 Request / Response / Headers / Body。</div></div>`
        : '';

    if (currentCharlesDetailTab === 'contents') {
        detailBody.innerHTML = `
            ${loadingHint}
            <div class="charles-contents-vertical">
                ${buildCharlesContentsPane('Request', requestMessage, parsedArtifacts.ready ? parsedArtifacts.requestHex : '')}
                ${buildCharlesContentsPane('Response', responseMessage, parsedArtifacts.ready ? parsedArtifacts.responseHex : '')}
            </div>
        `;
        return;
    }

    if (currentCharlesDetailTab === 'summary') {
        detailBody.innerHTML = `
            ${loadingHint}
            ${buildCharlesSummaryTable(session, requestMessage, responseMessage)}
        `;
        return;
    }

    if (currentCharlesDetailTab === 'chart') {
        detailBody.innerHTML = `
            ${buildCharlesTimelineHtml(session)}
            <div class="charles-detail-section">
                <div class="charles-detail-title">Timing</div>
                <div class="charles-overview-table">
                    <div class="charles-overview-row">
                        <div class="charles-overview-name">Request Start Time</div>
                        <div class="charles-overview-value">${escapeHtmlInline(session.startTimestamp || '-')}</div>
                    </div>
                    <div class="charles-overview-row">
                        <div class="charles-overview-name">Response End Time</div>
                        <div class="charles-overview-value">${escapeHtmlInline(session.endTimestamp || '-')}</div>
                    </div>
                    <div class="charles-overview-row">
                        <div class="charles-overview-name">Duration</div>
                        <div class="charles-overview-value">${escapeHtmlInline(`${session.durationMs} ms`)}</div>
                    </div>
                </div>
            </div>
        `;
        return;
    }

    if (currentCharlesDetailTab === 'notes') {
        detailBody.innerHTML = buildCharlesNotesPane(session);
        return;
    }

    detailBody.innerHTML = `
        ${loadingHint}
        ${buildCharlesOverviewTable(session, requestMessage, responseMessage)}
    `;
}

function renderCharlesCurrentState() {
    const sessionList = $('charlesSessionList');
    const sessionMeta = $('charlesSessionMeta');
    if (!sessionList || !sessionMeta) {
        return;
    }

    const packets = Array.isArray(window._proxyPackets) ? window._proxyPackets : [];
    const sessions = getCharlesSessionsForCurrentProxyView();
    const filteredSessions = filterCharlesSessions(sessions);
    const selectedSession = getCurrentCharlesSelection(filteredSessions);

    // 列表 key：只在会话集合变化时重绘列表（不包含 selectedId 和 detailTab）
    const firstKey = packets.length ? (packets[0].sequence || '') : '';
    const lastKey = packets.length ? (packets[packets.length - 1].sequence || '') : '';
    const listKey = [
        currentProxyProjectionKey,
        filteredSessions.length,
        firstKey,
        lastKey
    ].join('|');

    // 详情 key：选中会话或 tab 变化时重绘详情
    const detailKey = [
        currentProxyProjectionKey,
        selectedSession ? selectedSession.sessionId : '',
        selectedSession ? (selectedSession.packetSequences || []).length : 0,
        currentCharlesDetailTab
    ].join('|');

    // 两个 key 都没变化则跳过
    if (listKey === lastCharlesListRenderKey && detailKey === lastCharlesDetailRenderKey) {
        return;
    }

    const detailTabs = {
        overview: $('charlesDetailTabOverview'),
        contents: $('charlesDetailTabContents'),
        summary: $('charlesDetailTabSummary'),
        chart: $('charlesDetailTabChart'),
        notes: $('charlesDetailTabNotes')
    };
    Object.entries(detailTabs).forEach(([tabName, el]) => {
        if (el) el.classList.toggle('active', currentCharlesDetailTab === tabName);
    });

    sessionMeta.textContent = `${filteredSessions.length} / ${sessions.length} 个会话 · ${packets.length} 个原始包`;

    if (!packets.length) {
        sessionList.innerHTML = `<div class="charles-empty-state">${currentProxyRecordEnabled ? '当前实例暂无可投影为 Charles 视图的原始包记录' : '数据包记录未开启，Charles 子视图无法生成会话'}</div>`;
        lastCharlesListRenderKey = listKey;
        lastCharlesDetailRenderKey = detailKey;
        renderCharlesDetail(null);
        return;
    }

    if (!filteredSessions.length) {
        sessionList.innerHTML = '<div class="charles-empty-state">当前过滤条件下没有匹配的 Charles 会话</div>';
        lastCharlesListRenderKey = listKey;
        lastCharlesDetailRenderKey = detailKey;
        renderCharlesDetail(null);
        return;
    }

    // 仅当列表 key 变化时才重绘列表 DOM（避免每新包进来都全量重建 innerHTML）
    if (listKey !== lastCharlesListRenderKey) {
        sessionList.innerHTML = buildCharlesTreeHtml(filteredSessions);
        lastCharlesListRenderKey = listKey;
    }

    // 仅当详情 key 变化时才重绘详情
    if (detailKey !== lastCharlesDetailRenderKey) {
        lastCharlesDetailRenderKey = detailKey;
        renderCharlesDetail(selectedSession);
    }
}

function requestProxyData(instanceId, forceFull = false) {
    const canUseIncremental = !forceFull
        && lastProxyPacketOwnerInstanceId === instanceId
        && window._proxyPacketOwnerInstanceId === instanceId
        && Array.isArray(window._proxyPackets)
        && window._proxyPackets.length > 0;

    const packets = canUseIncremental ? window._proxyPackets : [];
    postAction('config_get_proxydata', {
        instanceId,
        knownCount: packets.length,
        firstKey: packets.length ? buildProxyPacketRowKey(packets[0]) : '',
        lastKey: packets.length ? buildProxyPacketRowKey(packets[packets.length - 1]) : ''
    });
}

function loadProxyDataForInstance(instanceId, instanceName) {
    const changed = updateProxyDataPanelShell(instanceId, instanceName);
    const tableContainer = $('proxyDataTableContainer');
    const subviewContainer = $('proxySubviewContent');

    if (changed) {
        currentProxyRecordEnabled = false;
        currentProxyBufferSize = 200;
        currentProxyTotalCount = 0;
        currentProxyReturnedCount = 0;
        currentProxyLastBackendInstanceId = '';
        currentProxyLastBackendReturnedCount = 0;
        currentProxyLastBackendTotalCount = 0;
        currentProxyLastDropReason = '';
        lastProxyDataRenderKey = '';
        lastCharlesRenderKey = ''; lastCharlesListRenderKey = ''; lastCharlesDetailRenderKey = '';
        cachedCharlesProjectionKey = '';
        cachedCharlesSessions = [];
        invalidateCharlesSessionParseCache();
        currentCharlesSelectedSessionId = '';
        currentCharlesDetailTab = 'overview';
        lastProxyPacketRowKeys = [];
        lastProxyPacketOwnerInstanceId = instanceId;
        window._proxyPacketOwnerInstanceId = instanceId;
        window._proxyPackets = [];
        window._proxyPacketDetails = new Map();
        pendingProxyPacketDetailRequest = null;
        currentProxyFocusedSequence = '';
        currentProxyNeedsFocusScroll = false;
        setProxySharedControlsState({
            disabled: true,
            checked: false,
            bufferSize: 200
        });
        updateProxyDataSummaryText();
        if (proxyFocusClearTimer) {
            clearTimeout(proxyFocusClearTimer);
            proxyFocusClearTimer = null;
        }
        if (currentProxySubview === 'charles' && subviewContainer) {
            const list = $('charlesSessionList');
            const detail = $('charlesDetailBody');
            if (list) {
                list.innerHTML = '<div class="charles-empty-state">正在根据原始包记录构建 Charles 会话...</div>';
            }
            if (detail) {
                detail.innerHTML = '<div class="charles-empty-state">等待会话数据...</div>';
            }
        } else if (tableContainer) {
            tableContainer.innerHTML = '<div class="info-hint">正在加载数据包记录...</div>';
            tableContainer.dataset.mode = 'loading';
            delete tableContainer.dataset.emptyText;
        }
    }

    requestProxyData(instanceId, true);
}

function refreshProxyData(instanceId) {
    requestProxyData(instanceId, true);
}

function clearProxyDataForInstance(instanceId) {
    if (confirm('确定要清空所有代理数据记录吗？')) {
        postAction('config_clear_proxydata', { instanceId });
        setTimeout(() => refreshProxyData(instanceId), 500);
    }
}

function saveProxyDataSettings(instanceId) {
    const enabledCheckbox = $('proxySharedRecordEnabled') || $('proxyRecordEnabled');
    const bufferSizeInput = $('proxySharedBufferSize') || $('proxyBufferSize');
    if (!enabledCheckbox || !bufferSizeInput) return;

    const enabled = enabledCheckbox.checked;
    let bufferSize = parseInt(bufferSizeInput.value) || 200;
    if (bufferSize < 10) bufferSize = 10;
    if (bufferSize > 10000) bufferSize = 10000;
    bufferSizeInput.value = bufferSize;

    postAction('config_set_proxydata_settings', { instanceId, enabled, bufferSize });

    // 保存会同步刷新后端开关和缓冲区，并可能清空旧缓存；这里主动刷新一次，
    // 避免界面停留在旧状态，误以为“开启记录后前端没反应”。
    setTimeout(() => refreshProxyData(instanceId), 120);
}

function buildProxyPacketRowKey(pkt) {
    if (pkt && pkt.sequence !== undefined && pkt.sequence !== null) {
        return String(pkt.sequence);
    }
    return [
        pkt.timestamp || '',
        pkt.username || '',
        pkt.isRequest ? 1 : 0,
        pkt.dataLength || 0,
        pkt.dataPreview || ''
    ].join(',');
}

function buildProxyPacketRowHtml(pkt, idx) {
    const directionText = pkt.isRequest ? '请求 ↑' : '响应 ↓';
    const rowBg = pkt.isRequest ? 'rgba(74,158,255,0.12)' : 'rgba(255,215,0,0.12)';
    const directionColor = pkt.isRequest ? '#4a9eff' : '#ffd700';
    return `
        <tr style="background:${rowBg}; cursor:pointer;" ondblclick="showPacketHexDetail(${idx})" title="双击查看完整数据">
            <td>${pkt.timestamp}</td>
            <td>${pkt.username || '-'}</td>
            <td style="color:${directionColor}; font-weight:600;">${directionText}</td>
            <td>${pkt.dataLength} B</td>
            <td style="font-family:monospace; font-size:10px; word-break:break-all;">${pkt.dataPreview}</td>
        </tr>
    `;
}

function buildProxyPacketVirtualRowHtml(pkt, idx) {
    const directionText = pkt.isRequest ? '请求 ↑' : '响应 ↓';
    const rowBg = pkt.isRequest ? 'rgba(74,158,255,0.12)' : 'rgba(255,215,0,0.12)';
    const directionColor = pkt.isRequest ? '#4a9eff' : '#ffd700';
    const focusedClass = currentProxyFocusedSequence && String(pkt.sequence) === String(currentProxyFocusedSequence)
        ? ' is-focused'
        : '';
    return `
        <div class="proxy-virtual-row${focusedClass}" style="background:${rowBg};" ondblclick="showPacketHexDetail(${idx})" title="双击查看完整数据">
            <div class="proxy-virtual-cell proxy-col-time">${pkt.timestamp}</div>
            <div class="proxy-virtual-cell proxy-col-user">${pkt.username || '-'}</div>
            <div class="proxy-virtual-cell proxy-col-dir" style="color:${directionColor}; font-weight:600;">${directionText}</div>
            <div class="proxy-virtual-cell proxy-col-len">${pkt.dataLength} B</div>
            <div class="proxy-virtual-cell proxy-col-preview">${pkt.dataPreview}</div>
        </div>
    `;
}

function ensureProxyDataTableShell(container) {
    if (container.dataset.mode === 'table' && $('proxyDataVirtualViewport')) {
        return;
    }

    container.innerHTML = `
        <div class="proxy-data-table">
            <div class="proxy-virtual-header">
                <div class="proxy-virtual-cell proxy-col-time">时间</div>
                <div class="proxy-virtual-cell proxy-col-user">用户名</div>
                <div class="proxy-virtual-cell proxy-col-dir">方向</div>
                <div class="proxy-virtual-cell proxy-col-len">长度</div>
                <div class="proxy-virtual-cell proxy-col-preview">数据预览 (Hex)</div>
            </div>
            <div id="proxyDataVirtualViewport" class="proxy-virtual-viewport">
                <div id="proxyDataVirtualTopSpacer"></div>
                <div id="proxyDataVirtualRows"></div>
                <div id="proxyDataVirtualBottomSpacer"></div>
            </div>
        </div>
    `;
    container.dataset.mode = 'table';

    const viewport = $('proxyDataVirtualViewport');
    if (viewport && !viewport.dataset.bound) {
        viewport.dataset.bound = '1';
        viewport.addEventListener('scroll', () => {
            renderProxyDataVirtualRows(Array.isArray(window._proxyPackets) ? window._proxyPackets : []);
        });
    }
}

function renderProxyDataVirtualRows(packets, scrollToBottom = false) {
    const viewport = $('proxyDataVirtualViewport');
    const rowsEl = $('proxyDataVirtualRows');
    const topSpacer = $('proxyDataVirtualTopSpacer');
    const bottomSpacer = $('proxyDataVirtualBottomSpacer');
    if (!viewport || !rowsEl || !topSpacer || !bottomSpacer) {
        return;
    }

    const total = Array.isArray(packets) ? packets.length : 0;
    if (total === 0) {
        rowsEl.innerHTML = '';
        topSpacer.style.height = '0px';
        bottomSpacer.style.height = '0px';
        return;
    }

    const viewportHeight = Math.max(viewport.clientHeight || 0, PROXY_VIRTUAL_ROW_HEIGHT * 8);
    let effectiveScrollTop = viewport.scrollTop;
    if (currentProxyNeedsFocusScroll && currentProxyFocusedSequence) {
        const focusIndex = packets.findIndex(pkt => String(pkt.sequence) === String(currentProxyFocusedSequence));
        if (focusIndex >= 0) {
            effectiveScrollTop = Math.max(0, focusIndex * PROXY_VIRTUAL_ROW_HEIGHT - Math.floor(viewportHeight / 2));
            viewport.scrollTop = effectiveScrollTop;
            currentProxyNeedsFocusScroll = false;
        }
    } else if (scrollToBottom) {
        effectiveScrollTop = Math.max(0, total * PROXY_VIRTUAL_ROW_HEIGHT - viewportHeight);
    }

    const startIndex = Math.max(0, Math.floor(effectiveScrollTop / PROXY_VIRTUAL_ROW_HEIGHT) - PROXY_VIRTUAL_OVERSCAN);
    const endIndex = Math.min(total, Math.ceil((effectiveScrollTop + viewportHeight) / PROXY_VIRTUAL_ROW_HEIGHT) + PROXY_VIRTUAL_OVERSCAN);

    topSpacer.style.height = `${startIndex * PROXY_VIRTUAL_ROW_HEIGHT}px`;
    bottomSpacer.style.height = `${Math.max(0, total - endIndex) * PROXY_VIRTUAL_ROW_HEIGHT}px`;
    rowsEl.innerHTML = packets.slice(startIndex, endIndex)
        .map((pkt, offset) => buildProxyPacketVirtualRowHtml(pkt, startIndex + offset))
        .join('');

    if (scrollToBottom) {
        viewport.scrollTop = Math.max(0, total * PROXY_VIRTUAL_ROW_HEIGHT - viewportHeight);
    }
}

function mergeProxyPacketSummaries(packets) {
    if (!Array.isArray(packets) || packets.length === 0) {
        return [];
    }
    return packets.map(pkt => Object.assign({}, pkt));
}

function handleProxyDataList(data) {
    const instanceId = data && data.instanceId ? data.instanceId : selectedProxyInstanceId;
    currentProxyLastBackendInstanceId = instanceId || '';
    currentProxyLastBackendReturnedCount = Array.isArray(data?.packets) ? data.packets.length : 0;
    currentProxyLastBackendTotalCount = Number(data?.totalCount || currentProxyLastBackendReturnedCount || 0);

    if (!selectedProxyInstanceId && instanceId) {
        selectedProxyInstanceId = instanceId;
    }

    if (!instanceId || (selectedProxyInstanceId && instanceId !== selectedProxyInstanceId)) {
        currentProxyLastDropReason = !instanceId
            ? 'response-instance-empty'
            : `selected-mismatch(${selectedProxyInstanceId}!=${instanceId})`;
        updateProxyDataSummaryText();
        return;
    }
    currentProxyLastDropReason = '';

    const previousPackets = (lastProxyPacketOwnerInstanceId === instanceId
        && window._proxyPacketOwnerInstanceId === instanceId
        && Array.isArray(window._proxyPackets))
        ? window._proxyPackets
        : [];
    const incomingPackets = Array.isArray(data.packets) ? data.packets : [];
    if (incomingPackets.length > 0) {
        lastProxyRealtimeUpdateAt = Date.now();
    }
    let packets;
    if (data && data.reset === false) {
        if (incomingPackets.length === 0) {
            packets = previousPackets;
        } else if (previousPackets.length === 0) {
            packets = mergeProxyPacketSummaries(incomingPackets);
        } else {
            packets = previousPackets.concat(mergeProxyPacketSummaries(incomingPackets));
        }
    } else {
        packets = mergeProxyPacketSummaries(incomingPackets);
        window._proxyPacketDetails = new Map();
        window._proxyPacketDetailRequests = new Set();
        invalidateCharlesSessionParseCache();
    }
    const renderKey = [
        instanceId,
        data.searchHex || '',
        data.enabled ? 1 : 0,
        data.bufferSize || 0,
        packets.length,
        data.totalCount || packets.length,
        data.lastKey || '',
        data.reset === false ? 0 : 1
    ].join('|');
    currentProxyProjectionKey = renderKey;
    currentProxyRecordEnabled = data.enabled === true;
    currentProxyBufferSize = data.bufferSize || 200;
    currentProxyTotalCount = data.totalCount || packets.length;
    currentProxyReturnedCount = incomingPackets.length;
    currentProxyShouldStickToBottom = proxyRealtimeEnabled && data && data.reset === false && incomingPackets.length > 0;

    lastProxyPacketOwnerInstanceId = instanceId;
    window._proxyPacketOwnerInstanceId = instanceId;
    if (typeof data.searchHex === 'string') {
        currentProxyHexSearch = data.searchHex;
        const input = $('proxyHexSearchInput');
        if (input && input.value !== currentProxyHexSearch) {
            input.value = currentProxyHexSearch;
        }
    }

    if (!isMainPageActive('proxydata')) {
        window._proxyPackets = packets;
        cachedCharlesProjectionKey = '';
        updateProxyDataSummaryText();
        return;
    }
    window._proxyPackets = packets;
    cachedCharlesProjectionKey = '';
    updateProxyDataSummaryText();

    if (!data || packets.length === 0) {
        window._proxyPacketDetails = new Map();
        lastProxyPacketRowKeys = [];
    }

    renderCurrentProxySubview();
}
function parseHexBytesForDetail(hexStr) {
    return (hexStr || '')
        .split(/\s+/)
        .map(part => part.trim().toUpperCase())
        .filter(part => /^[0-9A-F]{2}$/.test(part));
}

function hexTokensToByteValues(hexStr) {
    return parseHexBytesForDetail(hexStr).map(part => parseInt(part, 16));
}

function decodeByteArrayUtf8(bytes) {
    if (!Array.isArray(bytes) || !bytes.length) return '';
    try {
        const decoder = new TextDecoder('utf-8', { fatal: false });
        return decoder.decode(new Uint8Array(bytes));
    } catch (error) {
        return bytes.map(value => (value >= 32 && value <= 126) ? String.fromCharCode(value) : '.').join('');
    }
}

function decodeByteArrayLatin1(bytes) {
    if (!Array.isArray(bytes) || !bytes.length) return '';
    return bytes.map(value => String.fromCharCode(value)).join('');
}

function normalizeHttpNewlines(text) {
    return String(text || '').replace(/\r\n/g, '\n');
}

function parseHttpHeadersBlock(headerLines) {
    const headers = [];
    let currentHeader = null;

    headerLines.forEach(line => {
        if (!line) return;
        if ((line.startsWith(' ') || line.startsWith('\t')) && currentHeader) {
            currentHeader.value += ' ' + line.trim();
            return;
        }

        const separator = line.indexOf(':');
        if (separator <= 0) {
            currentHeader = null;
            return;
        }

        currentHeader = {
            name: line.slice(0, separator).trim(),
            value: line.slice(separator + 1).trim()
        };
        headers.push(currentHeader);
    });

    return headers;
}

function detectBodyFormat(contentType, bodyText) {
    const normalizedType = (contentType || '').toLowerCase();
    const trimmed = (bodyText || '').trim();

    if (!trimmed) return { format: 'empty', text: '' };

    if (normalizedType.includes('json') || /^[\[{]/.test(trimmed)) {
        try {
            return { format: 'json', text: JSON.stringify(JSON.parse(trimmed), null, 2) };
        } catch (error) {
            return { format: 'json-raw', text: bodyText };
        }
    }

    if (normalizedType.includes('xml') || trimmed.startsWith('<?xml') || trimmed.startsWith('<')) {
        return { format: 'xml', text: prettyPrintXml(bodyText) || bodyText };
    }

    if (normalizedType.includes('x-www-form-urlencoded')) {
        try {
            const params = new URLSearchParams(trimmed);
            const lines = [];
            params.forEach((value, key) => {
                lines.push(`${key} = ${value}`);
            });
            return { format: 'form', text: lines.join('\n') || bodyText };
        } catch (error) {
            return { format: 'form-raw', text: bodyText };
        }
    }

    return { format: 'text', text: prettyPrintText(bodyText) };
}

function prettyPrintXml(xmlText) {
    const input = String(xmlText || '').trim();
    if (!input) return '';

    const tokens = input
        .replace(/>\s*</g, '><')
        .replace(/</g, '\n<')
        .replace(/\n+/g, '\n')
        .trim()
        .split('\n')
        .filter(Boolean);

    let indent = 0;
    const lines = [];

    tokens.forEach(token => {
        const isClosing = /^<\//.test(token);
        const isSelfClosing = /\/>$/.test(token) || /^<\?/.test(token) || /^<!/.test(token);

        if (isClosing) {
            indent = Math.max(0, indent - 1);
        }

        lines.push(`${'  '.repeat(indent)}${token}`);

        if (!isClosing && !isSelfClosing && /^<[^!?][^>]*>$/.test(token) && !/<\/.+>$/.test(token)) {
            indent += 1;
        }
    });

    return lines.join('\n');
}

function prettyPrintText(text) {
    return normalizeHttpNewlines(String(text || ''));
}

function parseHttpMessageFromHex(hexStr, direction) {
    const bytes = hexTokensToByteValues(hexStr);
    if (!bytes.length) {
        return {
            ok: false,
            direction,
            reason: 'empty'
        };
    }

    const latin1Text = decodeByteArrayLatin1(bytes);
    const headerEnd = latin1Text.indexOf('\r\n\r\n');
    const altHeaderEnd = headerEnd >= 0 ? -1 : latin1Text.indexOf('\n\n');
    const separatorIndex = headerEnd >= 0 ? headerEnd : altHeaderEnd;

    if (separatorIndex < 0) {
        return {
            ok: false,
            direction,
            reason: 'no-http-separator',
            rawPreview: latin1Text.slice(0, 512)
        };
    }

    const separatorLength = headerEnd >= 0 ? 4 : 2;
    const headerTextRaw = latin1Text.slice(0, separatorIndex);
    const bodyBytes = bytes.slice(separatorIndex + separatorLength);
    const headerText = normalizeHttpNewlines(headerTextRaw);
    const headerLines = headerText.split('\n');
    const startLine = (headerLines.shift() || '').trim();
    const headers = parseHttpHeadersBlock(headerLines);
    const contentTypeHeader = headers.find(header => header.name.toLowerCase() === 'content-type');
    const contentLengthHeader = headers.find(header => header.name.toLowerCase() === 'content-length');
    const bodyText = decodeByteArrayUtf8(bodyBytes);
    const bodyInfo = detectBodyFormat(contentTypeHeader ? contentTypeHeader.value : '', bodyText);

    const requestMatch = startLine.match(/^(GET|POST|PUT|DELETE|PATCH|HEAD|OPTIONS|TRACE|CONNECT)\s+(\S+)\s+HTTP\/([0-9.]+)$/i);
    const responseMatch = startLine.match(/^HTTP\/([0-9.]+)\s+(\d{3})(?:\s+(.+))?$/i);

    return {
        ok: true,
        direction,
        startLine,
        headers,
        bodyText,
        bodyPreview: bodyInfo.text,
        bodyFormat: bodyInfo.format,
        bodyLength: bodyBytes.length,
        contentType: contentTypeHeader ? contentTypeHeader.value : '',
        contentLength: contentLengthHeader ? contentLengthHeader.value : '',
        method: requestMatch ? requestMatch[1].toUpperCase() : '',
        path: requestMatch ? requestMatch[2] : '',
        httpVersion: requestMatch ? requestMatch[3] : (responseMatch ? responseMatch[1] : ''),
        statusCode: responseMatch ? responseMatch[2] : '',
        statusText: responseMatch ? (responseMatch[3] || '') : ''
    };
}

function escapeHtmlInline(text) {
    return String(text || '')
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
}

function renderPacketHexInteractiveView(container, bytes) {
    if (!container) return;

    const bytesPerLine = 16;
    let html = '<div class="packet-hex-view">';
    html += '<div class="packet-hex-header">';
    html += '<div class="packet-hex-offset-head">Offset</div>';
    html += '<div class="packet-hex-bytes-head">';
    for (let i = 0; i < bytesPerLine; i++) {
        html += `<span class="packet-hex-head-cell">${i.toString(16).toUpperCase().padStart(2, '0')}</span>`;
    }
    html += '</div>';
    html += '<div class="packet-hex-ascii-head">ASCII</div>';
    html += '</div>';

    for (let rowStart = 0; rowStart < bytes.length; rowStart += bytesPerLine) {
        const rowBytes = bytes.slice(rowStart, rowStart + bytesPerLine);
        html += '<div class="packet-hex-row">';
        html += `<div class="packet-hex-offset">${rowStart.toString(16).toUpperCase().padStart(8, '0')}</div>`;
        html += '<div class="packet-hex-bytes">';
        for (let j = 0; j < bytesPerLine; j++) {
            const absoluteIndex = rowStart + j;
            if (j < rowBytes.length) {
                html += `<span class="packet-hex-byte" data-byte-index="${absoluteIndex}">${rowBytes[j]}</span>`;
            } else {
                html += '<span class="packet-hex-byte is-empty"></span>';
            }
        }
        html += '</div>';
        html += '<div class="packet-hex-ascii">';
        for (let j = 0; j < rowBytes.length; j++) {
            const absoluteIndex = rowStart + j;
            const value = parseInt(rowBytes[j], 16);
            const asciiChar = (value >= 0x20 && value <= 0x7e) ? String.fromCharCode(value) : '.';
            html += `<span class="packet-ascii-byte" data-byte-index="${absoluteIndex}">${escapeHtmlInline(asciiChar)}</span>`;
        }
        html += '</div>';
        html += '</div>';
    }

    html += '</div>';
    container.innerHTML = html;

    let selectionStart = -1;
    let selectionEnd = -1;
    let dragging = false;

    const applySelection = () => {
        const minIndex = Math.min(selectionStart, selectionEnd);
        const maxIndex = Math.max(selectionStart, selectionEnd);
        container.querySelectorAll('[data-byte-index]').forEach(node => {
            const index = parseInt(node.dataset.byteIndex, 10);
            const selected = selectionStart >= 0 && selectionEnd >= 0 && index >= minIndex && index <= maxIndex;
            node.classList.toggle('is-selected', selected);
        });
    };

    const startSelection = (index) => {
        selectionStart = index;
        selectionEnd = index;
        dragging = true;
        applySelection();
    };

    const updateSelection = (index) => {
        if (!dragging) return;
        selectionEnd = index;
        applySelection();
    };

    container.querySelectorAll('[data-byte-index]').forEach(node => {
        node.addEventListener('mousedown', (event) => {
            event.preventDefault();
            startSelection(parseInt(node.dataset.byteIndex, 10));
        });
        node.addEventListener('mouseenter', () => {
            updateSelection(parseInt(node.dataset.byteIndex, 10));
        });
    });

    const stopSelection = () => { dragging = false; };
    document.addEventListener('mouseup', stopSelection, { once: true });
}

function showPacketHexOverlay(pkt, hexStr, loading = false) {
    const existing = $('hexDetailOverlay');
    if (existing) {
        existing.remove();
    }

    const directionText = pkt.isRequest ? '请求 (客户端→服务器)' : '响应 (服务器→客户端)';
    const directionColor = pkt.isRequest ? '#4a9eff' : '#ffd700';
    const overlay = document.createElement('div');
    overlay.id = 'hexDetailOverlay';
    overlay.style.cssText = 'position:fixed; top:0; left:0; right:0; bottom:0; background:rgba(0,0,0,0.7); z-index:10000; display:flex; align-items:center; justify-content:center;';
    overlay.onclick = function(e) { if (e.target === overlay) overlay.remove(); };

    const detailBody = loading
        ? '<div style="color:var(--text-secondary,#aaa); font-size:13px;">正在加载完整十六进制数据...</div>'
        : '<div id="packetHexInteractiveView"></div>';

    overlay.innerHTML = `
        <div style="background:var(--bg-primary, #1a1a2e); border:1px solid var(--border, #333); border-radius:8px; width:780px; max-width:90vw; max-height:85vh; display:flex; flex-direction:column; box-shadow:0 8px 32px rgba(0,0,0,0.5);">
            <div style="padding:16px 20px; border-bottom:1px solid var(--border, #333); display:flex; justify-content:space-between; align-items:center;">
                <div>
                    <span style="font-size:15px; font-weight:600; color:var(--text, #fff);">数据包详情</span>
                    <span style="color:${directionColor}; font-size:13px; margin-left:12px; font-weight:500;">${directionText}</span>
                </div>
                <button onclick="this.closest('#hexDetailOverlay').remove()" style="background:none; border:none; color:var(--text-muted, #888); font-size:20px; cursor:pointer; padding:0 4px;">&times;</button>
            </div>
            <div style="padding:12px 20px; border-bottom:1px solid var(--border, #333); font-size:12px; color:var(--text-secondary, #aaa); display:flex; gap:24px;">
                <span>时间: <b style="color:var(--text, #fff);">${pkt.timestamp}</b></span>
                <span>用户: <b style="color:var(--text, #fff);">${pkt.username || '-'}</b></span>
                <span>长度: <b style="color:var(--text, #fff);">${pkt.dataLength} Bytes</b></span>
            </div>
            <div style="padding:16px 20px; overflow:auto; flex:1;">
                ${detailBody}
            </div>
        </div>
    `;

    document.body.appendChild(overlay);

    if (!loading) {
        renderPacketHexInteractiveView($('packetHexInteractiveView'), parseHexBytesForDetail(hexStr));
    }
}

function handleProxyDataDetail(data) {
    if (!data || selectedProxyInstanceId !== data.instanceId) return;
    const pkt = data.sequence
        ? getProxyPacketBySequence(data.sequence)
        : (window._proxyPackets && window._proxyPackets[data.index]);
    if (!pkt) return;

    const pendingRequest = pendingProxyPacketDetailRequest;
    pendingProxyPacketDetailRequest = null;

    const detailKey = buildProxyPacketRowKey(pkt);
    const sequenceKey = getProxyPacketDetailCacheKey(pkt.sequence || data.sequence);
    if (sequenceKey) {
        window._proxyPacketDetailRequests.delete(sequenceKey);
    }

    if (data.success) {
        window._proxyPacketDetails.set(detailKey, data.fullDataHex || '');
        invalidateCharlesSessionParseCache();

        if (pendingRequest &&
            pendingRequest.instanceId === data.instanceId &&
            (
                (pendingRequest.sequence && pendingRequest.sequence === data.sequence) ||
                (!pendingRequest.sequence && pendingRequest.index === data.index)
            )) {
            showPacketHexOverlay(pkt, window._proxyPacketDetails.get(detailKey) || pkt.dataPreview || '');
        } else if (currentProxySubview === 'charles') {
            lastCharlesRenderKey = ''; lastCharlesListRenderKey = ''; lastCharlesDetailRenderKey = '';
            renderCharlesCurrentState();
        } else {
            showPacketHexOverlay(pkt, window._proxyPacketDetails.get(detailKey) || pkt.dataPreview || '');
        }
        return;
    }

    if (pendingRequest &&
        pendingRequest.instanceId === data.instanceId &&
        (
            (pendingRequest.sequence && pendingRequest.sequence === data.sequence) ||
            (!pendingRequest.sequence && pendingRequest.index === data.index)
        )) {
        showToast('warning', '提示', '完整数据包详情获取失败，已显示预览数据');
        showPacketHexOverlay(pkt, window._proxyPacketDetails.get(detailKey) || pkt.dataPreview || '');
    } else if (currentProxySubview === 'charles') {
        lastCharlesRenderKey = ''; lastCharlesListRenderKey = ''; lastCharlesDetailRenderKey = '';
        renderCharlesCurrentState();
    }
}

function showPacketHexDetail(index) {
    const pkt = window._proxyPackets && window._proxyPackets[index];
    if (!pkt) return;

    const detailKey = buildProxyPacketRowKey(pkt);

    if (window._proxyPacketDetails.has(detailKey)) {
        showPacketHexOverlay(pkt, window._proxyPacketDetails.get(detailKey));
        return;
    }

    pendingProxyPacketDetailRequest = { instanceId: selectedProxyInstanceId, index };
    showPacketHexOverlay(pkt, '', true);
    postAction('config_get_proxydata_detail', {
        instanceId: selectedProxyInstanceId,
        index,
        sequence: pkt.sequence || 0
    });
}
// 页面切换时清理定时器
const originalNavigateTo = navigateTo;
navigateTo = function(pageId, options = {}) {
    const changed = originalNavigateTo(pageId, options);
    if (!changed && !options.force) {
        return false;
    }
    if (pageId !== 'proxydata') {
        stopProxyDataRefresh();
        setProxyRealtimeSubscription(selectedProxyInstanceId, false);
        lastProxyPacketRowKeys = [];
    } else {
        syncProxyRealtimeState({ forceFullRefresh: true });
    }
    return changed;
};

backendUiReady = true;
if (pendingBackendMessages.length > 0) {
    scheduleBackendMessageFlush();
}
