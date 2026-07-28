// WebView2 message-based API
function postAction(action, params) {
    if (window.chrome && window.chrome.webview) {
        window.chrome.webview.postMessage(JSON.stringify(
            Object.assign({ action: action }, params || {})
        ));
    } else {
        console.error('WebView2 not available! window.chrome:', window.chrome, 'window.chrome.webview:', window.chrome?.webview);
        showToast('error', '错误', 'WebView2未初始化，无法与后端通信');
    }
}

// Login Tab Switching
function initLoginTabs() {
    const tabs = document.querySelectorAll('.login-tab');
    const contents = document.querySelectorAll('.tab-content');

    tabs.forEach(tab => {
        tab.addEventListener('click', () => {
            const targetTab = tab.getAttribute('data-tab');

            // Remove active class from all tabs and contents
            tabs.forEach(t => t.classList.remove('active'));
            contents.forEach(c => c.classList.remove('active'));

            // Add active class to clicked tab and corresponding content
            tab.classList.add('active');
            document.getElementById('tab-' + targetTab).classList.add('active');
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

// DOM refs
const $ = id => document.getElementById(id);

// Page navigation
function navigateTo(page) {
    document.querySelectorAll('.nav-item').forEach(n => n.classList.remove('active'));
    document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));

    const navItem = document.querySelector(`.nav-item[data-page="${page}"]`);
    if (navItem) navItem.classList.add('active');

    const pageEl = document.getElementById(`page-${page}`);
    if (pageEl) pageEl.classList.add('active');
}

document.querySelectorAll('.nav-item').forEach(item => {
    item.addEventListener('click', (e) => {
        e.preventDefault();
        const page = item.dataset.page;
        navigateTo(page);

        // Request data for the page
        if (page === 'instances') {
            postAction('get_instances');
        } else if (page === 'accounts') {
            postAction('get_socks5_pools');
        } else if (page === 'online') {
            postAction('get_online_stats');
        } else if (page === 'anticc') {
            postAction('anticc_get_config');
        } else if (page === 'wpe') {
            postAction('get_wpe_filters');
        } else if (page === 'logs') {
            postAction('get_logs');
        }
    });
});

// ========== Login Screen ==========
const loginScreen = $('loginScreen');
const mainApp = $('mainApp');
const btnLoginSubmit = $('btnLoginSubmit');
const loginUsername = $('loginUsername');
const loginPassword = $('loginPassword');

// Show login screen, hide main app
function showLoginScreen() {
    loginScreen.style.display = 'flex';
    mainApp.style.display = 'none';
}

// Show main app, hide login screen
function showMainApp() {
    loginScreen.style.display = 'none';
    mainApp.style.display = 'flex';
}

btnLoginSubmit.addEventListener('click', () => {
    const username = loginUsername.value.trim();
    const password = loginPassword.value.trim();

    if (!username || !password) {
        showToast('warning', '提示', '请输入账号和密码');
        return;
    }

    btnLoginSubmit.disabled = true;
    $('loginBtnText').textContent = '登录中...';
    postAction('cloud_login', { username, password });
});

// Allow Enter key to submit login
loginPassword.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') btnLoginSubmit.click();
});
loginUsername.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') loginPassword.focus();
});

// ========== Home Page (主菜单) ==========
const btnLogout = $('btnLogout');

btnLogout.addEventListener('click', () => {
    if (confirm('确定要登出吗？')) {
        postAction('cloud_logout');
    }
});

// Recharge button
$('btnRecharge').addEventListener('click', () => {
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

function renderInstances(instances) {
    window.cachedInstances = instances; // Cache for config page
    const container = $('instanceList');

    // 🔥 过滤掉 Socks5Pool 类型的实例（账号库实例在 SOCKS5管理 页面显示）
    const filteredInstances = instances.filter(inst => inst.type !== 'Socks5Pool');

    if (!filteredInstances || filteredInstances.length === 0) {
        container.innerHTML = '<div class="empty-state">暂无实例，点击"创建实例"开始</div>';
        return;
    }

    // 🔥 同时更新代​理数据页面的实例列表
    renderProxyInstanceList(instances);

    container.innerHTML = filteredInstances.map(inst => `
        <div class="list-item">
            <div class="list-item-info">
                <div class="list-item-title">${inst.name}</div>
                <div class="list-item-subtitle">
                    类型: ${getInstanceTypeName(inst.type)} | 端口: ${inst.port} |
                    状态: ${getInstanceStateName(inst.state)} |
                    连接数: ${inst.currentConnections}
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
    `).join('');
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
let showAddAccountForm = true;
let showCCProxyImport = false;
let showCCProxyAPI = false;
let editingAccount = null;
let cachedPools = [];

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

function renderSocks5Pools(pools) {
    cachedPools = pools || [];
    const container = $('poolListPanel');
    if (!pools || pools.length === 0) {
        container.innerHTML = '<div style="text-align:center; padding:20px; color:var(--text-muted); font-size:13px;">暂无账号库实例<br>请先创建一个账号库</div>';
        return;
    }

    container.innerHTML = pools.map(pool => `
        <div class="pool-item ${selectedPoolId === pool.id ? 'selected' : ''}"
             onclick="selectPool('${pool.id}')"
             oncontextmenu="showPoolContextMenu(event, '${pool.id}'); return false;">
            <span style="font-size:13px; color:var(--text);">${pool.name} (${pool.accountCount || 0}个账号)</span>
        </div>
    `).join('');
}

function selectPool(poolId) {
    selectedPoolId = poolId;
    postAction('socks5_pool_get_accounts', { poolId });
    // Re-render pool list to update selection highlight using cached pools
    if (cachedPools.length > 0) {
        renderSocks5Pools(cachedPools);
    }
}

function showPoolContextMenu(e, poolId) {
    e.preventDefault();
    if (confirm('确定要删除此账号库吗？')) {
        postAction('socks5_pool_delete', { poolId });
    }
}

function renderAccountPanel(poolId, poolName, accounts) {
    selectedPoolId = poolId;
    selectedPoolName = poolName;
    currentPoolAccounts = accounts || [];

    const content = $('accountPanelContent');
    if (!content) return;

    const accCount = accounts.length;

    content.innerHTML = `
        <div style="margin-bottom:12px;">
            <span style="color:#6ee7e7; font-size:16px; font-weight:600;">账号库: ${poolName}</span>
            <span style="color:var(--text-muted); font-size:13px; margin-left:8px;">(ID: ${poolId})</span>
        </div>

        <!-- 添加新账号 (折叠) -->
        <div style="border:1px solid var(--border); border-radius:var(--radius-sm); margin-bottom:16px; background:var(--bg-input);">
            <div style="padding:10px 14px; cursor:pointer; display:flex; justify-content:space-between; align-items:center;"
                 onclick="toggleAddAccountForm()">
                <span style="font-size:14px; font-weight:500;">添加新账号</span>
                <span style="color:var(--text-muted); font-size:14px;">${showAddAccountForm ? '▼' : '▶'}</span>
            </div>
            <div id="addAccountFormContainer" style="display:${showAddAccountForm ? 'block' : 'none'}; padding:0 14px 14px;">
                <div style="margin-bottom:8px; display:flex; align-items:center;">
                    <span style="min-width:80px; font-size:13px; color:var(--text-secondary);">用户名:</span>
                    <input type="text" id="newAccUsername" class="input" style="width:180px; height:30px; font-size:13px;">
                </div>
                <div style="margin-bottom:8px; display:flex; align-items:center;">
                    <span style="min-width:80px; font-size:13px; color:var(--text-secondary);">密码:</span>
                    <input type="password" id="newAccPassword" class="input" style="width:180px; height:30px; font-size:13px;">
                    <label style="margin-left:8px; font-size:12px; color:var(--text-secondary); cursor:pointer; display:flex; align-items:center; gap:4px;">
                        <input type="checkbox" onchange="togglePasswordVisibility('newAccPassword')"> 显示
                    </label>
                </div>
                <div style="margin-bottom:8px; display:flex; align-items:center;">
                    <span style="min-width:80px; font-size:13px; color:var(--text-secondary);">到期时间:</span>
                    <input type="text" id="newAccExpire" class="input" style="width:180px; height:30px; font-size:13px;">
                    <span style="margin-left:8px; font-size:12px; color:var(--text-muted);">(格式: YYYY-MM-DD HH:MM:SS)</span>
                </div>
                <div style="margin-bottom:10px; display:flex; align-items:center;">
                    <span style="min-width:80px; font-size:13px; color:var(--text-secondary);">最大连接:</span>
                    <input type="number" id="newAccMaxConn" class="input" value="0" min="0" style="width:100px; height:30px; font-size:13px;">
                    <span style="margin-left:8px; font-size:12px; color:var(--text-muted);">(0=不限制)</span>
                </div>
                <button class="btn btn-primary" style="padding:6px 20px; font-size:13px;" onclick="submitAddAccount()">添加账号</button>
            </div>
        </div>

        <!-- 账号列表 -->
        <div style="margin-bottom:16px;">
            <div style="margin-bottom:8px;">
                <span style="color:#ff0; font-size:14px; font-weight:500;">账号列表:</span>
                <span style="color:#ff0; font-size:13px; margin-left:8px;">共 ${accCount} 个账号</span>
            </div>
            <div style="overflow-x:auto; max-height:300px; border:1px solid var(--border); border-radius:var(--radius-sm);">
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
                    <tbody>
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

        <!-- 从CCProxy远程导入账号 (折叠) -->
        <div style="border:1px solid var(--border); border-radius:var(--radius-sm); margin-bottom:16px; background:var(--bg-input);">
            <div style="padding:10px 14px; cursor:pointer; display:flex; justify-content:space-between; align-items:center;"
                 onclick="toggleCCProxyImport()">
                <span style="font-size:14px; font-weight:500;">从CCProxy远程导入账号</span>
                <span style="color:var(--text-muted); font-size:14px;">${showCCProxyImport ? '▼' : '▶'}</span>
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
                <span style="font-size:14px; font-weight:500;">CCProxy API兼容服务</span>
                <span style="color:var(--text-muted); font-size:14px;">${showCCProxyAPI ? '▼' : '▶'}</span>
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

    // Request current API service status for this pool
    postAction('socks5_api_status', { poolId });
}

function toggleAddAccountForm() {
    showAddAccountForm = !showAddAccountForm;
    renderAccountPanel(selectedPoolId, selectedPoolName, currentPoolAccounts);
}

function toggleCCProxyImport() {
    showCCProxyImport = !showCCProxyImport;
    renderAccountPanel(selectedPoolId, selectedPoolName, currentPoolAccounts);
}

function toggleCCProxyAPI() {
    showCCProxyAPI = !showCCProxyAPI;
    renderAccountPanel(selectedPoolId, selectedPoolName, currentPoolAccounts);
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

function submitAddAccount() {
    const username = $('newAccUsername')?.value.trim();
    const password = $('newAccPassword')?.value.trim();
    const expireTime = $('newAccExpire')?.value.trim();
    const maxConnections = parseInt($('newAccMaxConn')?.value) || 0;

    if (!username || !password) {
        showToast('warning', '提示', '请输入用户名和密码');
        return;
    }

    postAction('socks5_account_add', {
        poolId: selectedPoolId,
        account: { username, password, expireTime, maxConnections }
    });
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
            <input type="text" id="editAccExpire" class="input" value="${acc.expireTime || ''}" placeholder="YYYY-MM-DD HH:MM:SS">
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
    const expireTime = $('editAccExpire')?.value.trim();
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
    const host = $('ccHost')?.value.trim();
    const port = $('ccPort')?.value.trim();
    const username = $('ccUsername')?.value.trim();
    const password = $('ccPassword')?.value.trim();

    if (!host || !port || !username || !password) {
        showToast('warning', '提示', '请填写完整的CCProxy连接信息');
        return;
    }

    postAction('socks5_ccproxy_import', {
        poolId: selectedPoolId,
        host, port, username, password
    });
}

// ========== WPE Filter Management ==========
let wpeFilters = [];
let wpeSelectedIds = new Set();
let currentEditFilterId = null;
let hexGridSearchOffset = 500; // grid index, 0=pos(-500), 500=pos(0)
let hexGridModifyOffset = 500;
let hexGridEnableOffset = 500;
let hexGridDisableOffset = 500;
const WPE_GRID_COLS = 24; // 搜索/修改每行显示24列
const WPE_ADV_GRID_COLS = 16; // 高级触发每行显示16列
const WPE_GRID_MIN_POS = -500;
const WPE_GRID_MAX_POS = 500;
const WPE_GRID_TOTAL = 1001; // -500 to +500

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

function renderWpeTargetInstances(selectedIds) {
    const instances = window.cachedInstances || [];
    const socksForward = instances.filter(i => i.type === 'SocksForward');
    if (socksForward.length === 0) {
        return '<div style="padding:8px; text-align:center; color:var(--text-muted); font-size:12px;">暂无Socks转发实例</div>';
    }
    return socksForward.map(inst => {
        const checked = selectedIds.includes(inst.id) ? 'checked' : '';
        return `<label style="display:flex; align-items:center; gap:6px; padding:4px 8px; font-size:13px; cursor:pointer;">
            <input type="checkbox" class="wpeTargetInst" value="${inst.id}" ${checked}>
            <span>${inst.id} (${inst.name})</span>
        </label>`;
    }).join('');
}

function wpeToggleGlobalTarget() {
    const global = $('wpeGlobal')?.checked;
    const list = $('wpeTargetInstanceList');
    if (list) list.style.display = global ? 'none' : 'block';
}

function renderWPEHexGrid(gridType, gridOffset, patternMap, cols) {
    cols = cols || WPE_GRID_COLS;
    const gridId = {search:'searchHexGridView', modify:'modifyHexGridView', enable:'enableHexGridView', disable:'disableHexGridView'}[gridType];
    const container = $(gridId);
    if (!container) return;

    const startPos = gridOffset + WPE_GRID_MIN_POS;
    const endPos = startPos + cols - 1;

    // 位置编号行
    let html = '<div class="hex-grid-compact">';
    html += '<div class="hex-row">';
    for (let col = 0; col < cols; col++) {
        const idx = gridOffset + col;
        if (idx >= WPE_GRID_TOTAL) break;
        const pos = idx + WPE_GRID_MIN_POS;
        html += `<div class="hex-pos-label">${pos}</div>`;
    }
    html += '</div>';

    // 十六进制输入框行
    html += '<div class="hex-row">';
    for (let col = 0; col < cols; col++) {
        const idx = gridOffset + col;
        if (idx >= WPE_GRID_TOTAL) break;
        const pos = idx + WPE_GRID_MIN_POS;
        const val = patternMap.get(pos) || '';
        const cls = val === '??' ? 'hex-cell wildcard' : (val ? 'hex-cell filled' : 'hex-cell');
        html += `<input type="text" class="${cls}" maxlength="2"
            data-pos="${pos}" data-grid="${gridType}"
            value="${val}" placeholder=""
            onchange="onHexCellChange(this)"
            onkeydown="onHexCellKeydown(event, this)">`;
    }
    html += '</div></div>';
    container.innerHTML = html;

    // 更新范围显示
    const rangeId = {search:'searchGridRange', modify:'modifyGridRange', enable:'enableGridRange', disable:'disableGridRange'}[gridType];
    const rangeEl = $(rangeId);
    if (rangeEl) rangeEl.textContent = `[${startPos} ~ ${endPos}]`;
}

function onHexCellChange(cell) {
    const val = cell.value.trim().toUpperCase();
    if (val && val !== '??' && !/^[0-9A-F?]{1,2}$/.test(val)) {
        cell.value = '';
        return;
    }
    cell.value = val;
    // 更新样式
    cell.className = val === '??' ? 'hex-cell wildcard' : (val ? 'hex-cell filled' : 'hex-cell');
}

function onHexCellKeydown(e, cell) {
    // 输入2个字符后自动跳到下一个格子
    if (cell.value.length >= 2 && e.key.length === 1 && /[0-9a-fA-F?]/.test(e.key)) {
        const next = cell.nextElementSibling;
        if (next && next.classList.contains('hex-cell')) {
            setTimeout(() => next.focus(), 0);
        }
    }
}

function getGridCols(gridType) {
    return (gridType === 'enable' || gridType === 'disable') ? WPE_ADV_GRID_COLS : WPE_GRID_COLS;
}
function getGridOffset(gridType) {
    return {search: hexGridSearchOffset, modify: hexGridModifyOffset, enable: hexGridEnableOffset, disable: hexGridDisableOffset}[gridType];
}
function setGridOffset(gridType, val) {
    if (gridType === 'search') hexGridSearchOffset = val;
    else if (gridType === 'modify') hexGridModifyOffset = val;
    else if (gridType === 'enable') hexGridEnableOffset = val;
    else if (gridType === 'disable') hexGridDisableOffset = val;
}
function getGridPatternInput(gridType) {
    return {search: $('wpeSearchPattern'), modify: $('wpeModifyPattern'), enable: $('wpeAdvEnablePattern'), disable: $('wpeAdvDisablePattern')}[gridType];
}

function scrollHexGrid(gridType, delta) {
    const cols = getGridCols(gridType);
    let offset = getGridOffset(gridType);
    offset = Math.max(0, Math.min(WPE_GRID_TOTAL - cols, offset + delta));
    setGridOffset(gridType, offset);
    const textInput = getGridPatternInput(gridType);
    renderWPEHexGrid(gridType, offset, parsePatternString(textInput?.value || ''), cols);
}

function jumpHexGrid(gridType) {
    const inputId = {search:'searchGridJump', modify:'modifyGridJump', enable:'enableGridJump', disable:'disableGridJump'}[gridType];
    const val = parseInt($(inputId)?.value);
    if (isNaN(val)) return;
    const cols = getGridCols(gridType);
    const target = Math.max(0, Math.min(WPE_GRID_TOTAL - cols, val - WPE_GRID_MIN_POS));
    setGridOffset(gridType, target);
    const textInput = getGridPatternInput(gridType);
    renderWPEHexGrid(gridType, target, parsePatternString(textInput?.value || ''), cols);
}

function centerHexGrid(gridType) {
    const center = 500; // pos(0) = index 500
    setGridOffset(gridType, center);
    const cols = getGridCols(gridType);
    const textInput = getGridPatternInput(gridType);
    renderWPEHexGrid(gridType, center, parsePatternString(textInput?.value || ''), cols);
}

function pasteHexGrid(gridType) {
    navigator.clipboard.readText().then(text => {
        if (!text) return;
        const hexBytes = [];
        const cleaned = text.trim();
        if (/^[0-9a-fA-F\s]+$/.test(cleaned) && cleaned.includes(' ')) {
            cleaned.split(/\s+/).forEach(h => { if (h.length === 2) hexBytes.push(h.toUpperCase()); });
        } else if (/^[0-9a-fA-F,]+$/.test(cleaned) && cleaned.includes(',')) {
            cleaned.split(',').forEach(h => { h = h.trim(); if (h.length === 2) hexBytes.push(h.toUpperCase()); });
        } else if (/^[0-9a-fA-F]+$/.test(cleaned) && cleaned.length % 2 === 0) {
            for (let i = 0; i < cleaned.length; i += 2) hexBytes.push(cleaned.substr(i, 2).toUpperCase());
        }
        if (hexBytes.length === 0) { showToast('warning', '提示', '无法解析剪贴板内容'); return; }

        const cells = document.querySelectorAll(`.hex-cell[data-grid="${gridType}"]`);
        let filled = 0;
        cells.forEach((cell, i) => {
            if (i < hexBytes.length) {
                cell.value = hexBytes[i];
                cell.className = 'hex-cell filled';
                filled++;
            }
        });
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
    return serializePatternMap(map);
}

function syncGridFromText(gridType) {
    const textInput = getGridPatternInput(gridType);
    const offset = getGridOffset(gridType);
    const cols = getGridCols(gridType);
    if (textInput) {
        renderWPEHexGrid(gridType, offset, parsePatternString(textInput.value), cols);
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
    const textInput = gridType === 'search' ? $('wpeSearchPattern') : $('wpeModifyPattern');
    if (textInput) textInput.value = '';
}

function showWPEFilterEditor(filter) {
    currentEditFilterId = filter.id;
    document.querySelectorAll('.page').forEach(p => p.classList.remove('active'));
    document.getElementById('page-wpe-edit').classList.add('active');

    const title = $('wpeEditTitle');
    if (title) title.textContent = `编辑滤镜: ${filter.name} (ID: ${filter.id})`;

    const content = $('wpeEditContent');
    if (!content) return;

    hexGridSearchOffset = 500;
    hexGridModifyOffset = 500;
    hexGridEnableOffset = 500;
    hexGridDisableOffset = 500;

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
                            <input type="number" id="wpeMinLength" class="input" value="${filter.minLength || 0}" placeholder="最小" style="width:80px;">
                            <span style="line-height:36px;">~</span>
                            <input type="number" id="wpeMaxLength" class="input" value="${filter.maxLength || 65535}" placeholder="最大" style="width:80px;">
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
                    <div class="form-group">
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
                    <button class="btn btn-ghost btn-sm" style="color:#f66;" onclick="clearWPEHexGrid('modify')">清空</button>
                    <span style="font-size:11px; color:var(--text-muted);">(留空=跳过)</span>
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

    renderWPEHexGrid('search', hexGridSearchOffset, parsePatternString(filter.searchPattern || ''));
    renderWPEHexGrid('modify', hexGridModifyOffset, parsePatternString(filter.modifyPattern || ''));
    renderWPEHexGrid('enable', hexGridEnableOffset, parsePatternString(adv.enablePattern || ''), WPE_ADV_GRID_COLS);
    renderWPEHexGrid('disable', hexGridDisableOffset, parsePatternString(adv.disablePattern || ''), WPE_ADV_GRID_COLS);
}

function saveWPEFilterFull() {
    const name = $('wpeFilterName')?.value?.trim();
    if (!name) {
        showToast('warning', '提示', '请输入滤镜名称');
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

function renderOnlineStats(data) {
    const container = $('onlineList');
    const accounts = data.accounts || [];

    // Render stats cards
    if (data.stats) {
        const statsCard = $('onlineStatsCards');
        if (statsCard) {
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

    // Render instance selector
    if (data.instances) {
        const selector = $('onlineInstanceSelector');
        if (selector) {
            const currentValue = selector.value;
            selector.innerHTML = '<option value="">请选择实例</option>' +
                data.instances.map(inst => `
                    <option value="${inst.id}" ${currentValue === inst.id ? 'selected' : ''}>${inst.name}</option>
                `).join('');
        }
    }

    if (accounts.length === 0) {
        container.innerHTML = '<div class="empty-state">请先选择一个SOCKS转发实例</div>';
        return;
    }

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
            <tbody>
                ${accounts.map(acc => {
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
                }).join('')}
            </tbody>
        </table>
    `;
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
    const config = {
        enabled: $('anticcEnabled').checked,
        timeWindow: parseInt($('anticcTimeWindow').value),
        maxRequests: parseInt($('anticcMaxRequests').value),
        banTime: parseInt($('anticcBanTime').value),
        authRequired: $('anticcAuthRequired').checked,
        autoKickUnauthenticated: $('anticcAutoKickUnauthenticated').checked,
        authTimeout: parseInt($('anticcAuthTimeout').value),
        maxConnectionsPerIP: parseInt($('anticcMaxConnectionsPerIP').value),
        maxConnectionsTotal: parseInt($('anticcMaxConnectionsTotal').value),
        maxBytesPerSecond: parseInt($('anticcMaxBytesPerSecond').value),
        maxPacketsPerSecond: parseInt($('anticcMaxPacketsPerSecond').value),
        threadPoolMode: $('anticcThreadPoolMode').value,
        minThreads: parseInt($('anticcMinThreads').value),
        maxThreads: parseInt($('anticcMaxThreads').value),
        firewallEnabled: $('anticcFirewallEnabled').checked,
        autoBlockEnabled: $('anticcAutoBlockEnabled').checked,
        whitelist: $('anticcWhitelist').value.split('\n').filter(ip => ip.trim()),
        blacklist: $('anticcBlacklist').value.split('\n').filter(ip => ip.trim())
    };
    postAction('anticc_save_config', config);
});

$('btnResetAntiCC').addEventListener('click', () => {
    if (confirm('确定要重置为默认配置吗？')) {
        postAction('anticc_reset_config');
    }
});

// Load AntiCC config when page is shown
function loadAntiCCConfig() {
    postAction('anticc_get_config');
}

function updateAntiCCForm(config) {
    if (!config) return;

    $('anticcEnabled').checked = config.enabled || false;
    $('anticcTimeWindow').value = config.timeWindow || 10;
    $('anticcMaxRequests').value = config.maxRequests || 20;
    $('anticcBanTime').value = config.banTime || 300;
    $('anticcAuthRequired').checked = config.authRequired || false;
    $('anticcAutoKickUnauthenticated').checked = config.autoKickUnauthenticated || false;
    $('anticcAuthTimeout').value = config.authTimeout || 30;
    $('anticcMaxConnectionsPerIP').value = config.maxConnectionsPerIP || 10;
    $('anticcMaxConnectionsTotal').value = config.maxConnectionsTotal || 1000;
    $('anticcMaxBytesPerSecond').value = config.maxBytesPerSecond || 0;
    $('anticcMaxPacketsPerSecond').value = config.maxPacketsPerSecond || 0;
    $('anticcThreadPoolMode').value = config.threadPoolMode || 'traditional';
    $('anticcMinThreads').value = config.minThreads || 2;
    $('anticcMaxThreads').value = config.maxThreads || 16;
    $('anticcFirewallEnabled').checked = config.firewallEnabled || false;
    $('anticcAutoBlockEnabled').checked = config.autoBlockEnabled || false;
    $('anticcWhitelist').value = (config.whitelist || []).join('\n');
    $('anticcBlacklist').value = (config.blacklist || []).join('\n');
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
    document.getElementById('page-wpe-edit').classList.remove('active');
    document.getElementById('page-wpe').classList.add('active');
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

function renderWPEFilters(filters) {
    wpeFilters = filters || [];

    const enabledCount = wpeFilters.filter(f => f.enabled).length;
    const totalExec = wpeFilters.reduce((sum, f) => sum + (f.executionCount || 0), 0);
    const statusEl = $('wpeFilterStatus');
    if (statusEl) {
        statusEl.textContent = `已启用: ${enabledCount} | 总数: ${wpeFilters.length} | 总执行次数: ${totalExec}`;
    }

    const tbody = $('wpeFilterBody');
    if (!tbody) return;

    if (wpeFilters.length === 0) {
        tbody.innerHTML = '<tr><td colspan="10" style="text-align:center; color:var(--text-muted); padding:30px;">暂无滤镜，点击"新建滤镜"开始</td></tr>';
        return;
    }

    tbody.innerHTML = wpeFilters.map(filter => {
        const isSelected = wpeSelectedIds.has(filter.id);
        const modeText = WPE_MODE_NAMES[filter.mode] || filter.mode || '普通';
        const actionText = WPE_ACTION_NAMES[filter.action] || filter.action || '替换';
        const dirText = getDirectionText(filter.direction);
        const globalText = filter.global ? '是' : '否';
        // 目标实例列
        let targetText = '未指定';
        if (filter.global) {
            targetText = '全部实例';
        } else if (filter.targetInstances && filter.targetInstances.length > 0) {
            targetText = filter.targetInstances.join(', ');
        }

        return `<tr ondblclick="wpeIsNewFilter=false; postAction('wpe_filter_get', {filterId: ${filter.id}})" style="cursor:pointer;">
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
    }).join('');
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
$('btnClearLogs').addEventListener('click', () => {
    if (confirm('确定要清空所有日志吗？')) {
        postAction('log_clear');
    }
});

$('btnRefreshLogs').addEventListener('click', () => {
    postAction('get_logs');
});

$('btnCopyLogs').addEventListener('click', () => {
    copyLogs();
});

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
    updateLogStats();
    filterLogs();
}

function updateLogStats() {
    const total = allLogs.length;
    const errorCount = allLogs.filter(log => log.level === 'ERROR').length;
    const warningCount = allLogs.filter(log => log.level === 'WARNING' || log.level === 'WARN').length;
    const infoCount = allLogs.filter(log => log.level === 'INFO').length;

    $('logStatTotal').textContent = total;
    $('logStatError').textContent = errorCount;
    $('logStatWarning').textContent = warningCount;
    $('logStatInfo').textContent = infoCount;
}

function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

// Store all logs for filtering
let allLogs = [];

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

    if (filtered.length === 0) {
        tbody.innerHTML = '<tr><td colspan="4" class="empty-state">无匹配日志</td></tr>';
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

    tbody.innerHTML = filtered.map(log => `
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
    `).join('');

    // Auto-scroll if enabled
    if ($('logAutoScroll').checked) {
        container.scrollTop = container.scrollHeight;
    }
}

// ========== WebView2 Message Listener ==========
if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', event => {
        const data = event.data;
        if (!data || !data.type) return;

        switch (data.type) {
            case 'status':
                updateStatus(data);
                break;
            case 'message':
                showToast(data.messageType, data.title, data.message);
                break;
            case 'instances':
                renderInstances(data.instances);
                break;
            case 'instance_config':
                showInstanceConfig(data.instance);
                break;
            case 'socks5_pools':
                renderSocks5Pools(data.pools);
                break;
            case 'socks5_pool_accounts':
                renderAccountPanel(data.poolId, data.poolName, data.accounts);
                postAction('get_socks5_pools'); // Refresh pool list to update selection
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
                renderOnlineStats(data);
                break;
            case 'wpe_filters':
                renderWPEFilters(data.filters);
                break;
            case 'wpe_filter_detail':
                wpeIsNewFilter = false;
                showWPEFilterEditor(data.filter);
                break;
            case 'wpe_import_options':
                handleWpeImportOptions(data);
                break;
            case 'logs':
                renderLogs(data.logs);
                break;
            case 'cloud_login_result':
                handleLoginResult(data);
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
            case 'anticc_config':
                updateAntiCCForm(data.config);
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
            case 'config_traffic':
                handleTrafficConfigData(data);
                break;
            case 'config_userfilter':
                handleUserFilterConfigData(data);
                break;
            case 'config_accountfilter':
                handleAccountFilterData(data);
                break;
            case 'proxydata_list':
                handleProxyDataList(data);
                break;
        }
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

    statusBadge.className = 'status-badge';

    if (data.loggedIn) {
        // Show main app, hide login screen
        showMainApp();

        statusBadge.classList.add('logged-in');
        statusText.textContent = '已登录';

        // Update sidebar user info
        if (sidebarUsername) sidebarUsername.textContent = data.username || '-';
        if (sidebarExpire) sidebarExpire.textContent = data.expireTime || '-';
        if (sidebarRemaining) sidebarRemaining.textContent = data.remainingDays !== undefined ? `${data.remainingDays}天` : '-';

        // Update home page user info
        $('homeUsername').textContent = `账号: ${data.username}`;
        $('homeExpireInfo').textContent = `到期时间: ${data.expireTime} | 剩余: ${data.remainingDays}天`;
    } else {
        // Show login screen, hide main app
        showLoginScreen();

        statusText.textContent = '未登录';
        topUserInfo.textContent = '';

        // Clear sidebar user info
        if (sidebarUsername) sidebarUsername.textContent = '-';
        if (sidebarExpire) sidebarExpire.textContent = '-';
        if (sidebarRemaining) sidebarRemaining.textContent = '-';
    }

    if (data.runningInstances !== undefined) {
        const instanceText = `${data.runningInstances}/${data.totalInstances}`;
        if (sidebarInstances) sidebarInstances.textContent = instanceText;
    }
}

function handleLoginResult(data) {
    btnLoginSubmit.disabled = false;
    $('loginBtnText').textContent = '登录';

    if (data.success) {
        showToast('success', '登录成功', `欢迎回来，${data.username}`);
        loginPassword.value = '';
        // Status update will trigger showMainApp()
    } else {
        showToast('error', '登录失败', data.error || '未知错误');
    }
}

function handleRegisterResult(data) {
    const btnRegister = $('btnRegisterSubmit');
    const btnText = $('registerBtnText');
    const messageArea = $('registerMessage');

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
    if (data.notice) {
        // Show on login screen
        const loginNoticeArea = $('loginNoticeArea');
        const loginNoticeContent = $('loginNoticeContent');
        if (loginNoticeContent) {
            loginNoticeContent.textContent = data.notice;
            loginNoticeArea.style.display = 'block';
        }

        // Show on home page
        const noticeCard = $('noticeCard');
        const noticeContent = $('noticeContent');
        if (noticeContent) {
            noticeContent.textContent = data.notice;
            noticeCard.style.display = 'block';
        }
    }
}

// ========== Instance Config Page Functions ==========
// currentConfigInstance is already declared at line 274
let currentConfigMenu = 'basic';

function showInstanceConfigPage(inst) {
    currentConfigInstance = inst;
    currentConfigMenu = 'basic';

    // Update page title
    $('configInstanceTitle').textContent = `配置实例: ${inst.name}`;
    $('configInstanceSubtitle').textContent = `实例ID: ${inst.id} | 类型: Socks转发`;

    // Navigate to config page
    navigateTo('instance-config');

    // Load initial menu
    switchConfigMenu('basic');
}

function backToInstances() {
    navigateTo('instances');
    postAction('get_instances');
}

function switchConfigMenu(menuId) {
    currentConfigMenu = menuId;

    // Update menu active state
    document.querySelectorAll('.menu-item').forEach(item => {
        item.classList.remove('active');
    });
    const menuItem = document.querySelector(`.menu-item[data-menu="${menuId}"]`);
    if (menuItem) menuItem.classList.add('active');

    // Render content
    const contentDiv = $('configContent');
    if (!currentConfigInstance) {
        contentDiv.innerHTML = '<p>未选择实例</p>';
        return;
    }

    const inst = currentConfigInstance;
    const isRunning = inst.state === 'Running';

    switch(menuId) {
        case 'basic':
            contentDiv.innerHTML = renderBasicInfo(inst);
            break;
        case 'proxy':
            contentDiv.innerHTML = renderProxyConfig(inst, isRunning);
            loadProxyConfig(inst.id);
            break;
        case 'thread':
            contentDiv.innerHTML = renderThreadConfig(inst, isRunning);
            loadThreadConfig(inst.id);
            break;
        case 'auth':
            contentDiv.innerHTML = renderAuthConfig(inst, isRunning);
            loadAuthConfig(inst.id);
            break;
        case 'packet':
            contentDiv.innerHTML = renderPacketConfig(inst, isRunning);
            loadPacketConfig(inst.id);
            break;
        case 'traffic':
            contentDiv.innerHTML = renderTrafficConfig(inst, isRunning);
            loadTrafficConfig(inst.id);
            break;
        case 'userfilter':
            contentDiv.innerHTML = renderUserFilterConfig(inst, isRunning);
            loadUserFilterConfig(inst.id);
            break;
        case 'accountfilter':
            contentDiv.innerHTML = renderAccountFilterConfig(inst, isRunning);
            loadAccountFilterConfig(inst.id);
            break;
    }
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
    if (enabled && poolId) {
        statusDiv.innerHTML = `
            <div class="info-hint" style="border-left-color: var(--success);">
                认证已启用<br>
                绑定账号库: ${poolId}<br>
                可用账号数: ${accountCount || 0}
            </div>
        `;
    } else if (enabled && !poolId) {
        statusDiv.innerHTML = `
            <div class="info-hint" style="border-left-color: var(--warning);">
                请选择账号实例
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

function renderTrafficRules(rules) {
    const container = $('trafficRuleList');
    if (!rules || rules.length === 0) {
        container.innerHTML = '<p style="color: var(--text-muted);">暂无过滤规则</p>';
        return;
    }

    const ruleTypeNames = ['端口匹配', '域名匹配', 'IP匹配', '端口+SNI域名'];
    let selectedIdx = -1;

    let html = `<table class="data-table">
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
        <tbody>`;

    rules.forEach((rule, i) => {
        html += `
            <tr>
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
            </tr>
        `;
    });

    html += '</tbody></table>';
    container.innerHTML = html;
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

// Menu 6: User Filter
function renderUserFilterConfig(inst, isRunning) {
    return `
        <h3>用户滤镜配置</h3>
        <div class="info-hint">
            说明：启用用户滤镜模式后，每个SOCKS用户可以通过网页管理自己的WPE滤镜配置。
        </div>
        <div class="checkbox-group">
            <input type="checkbox" id="enableUserFilterMode">
            <label for="enableUserFilterMode">启用用户滤镜模式</label>
        </div>

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
    `;
}

function loadUserFilterConfig(instanceId) {
    postAction('config_get_userfilter', { instanceId });
}

function handleUserFilterConfigData(data) {
    if ($('enableUserFilterMode')) {
        $('enableUserFilterMode').checked = data.enabled === true;
        $('httpPort').value = data.httpPort || 8080;

        // Event listener (prevent duplicate)
        const cb = $('enableUserFilterMode');
        if (!cb._listenerAdded) {
            cb._listenerAdded = true;
            cb.addEventListener('change', () => {
                postAction('config_set_userfilter_mode', {
                    instanceId: currentConfigInstance.id,
                    enabled: cb.checked
                });
            });
        }

        // HTTP server status
        const statusDiv = $('httpServerStatus');
        const isRunning = data.httpServerRunning;
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

        // Access URLs
        const urlsDiv = $('accessUrls');
        const localUrl = `http://127.0.0.1:${data.httpPort || 8080}`;
        const remoteUrl = data.remoteUrl || '';

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
            ` : ''}
        `;

        // Default filters
        renderDefaultFilters(data.availableFilters || [], data.defaultFilters || []);
    }
}

function renderDefaultFilters(available, defaults) {
    const container = $('defaultFilterList');
    if (!available || available.length === 0) {
        container.innerHTML = '<p style="color: var(--text-muted);">暂无可用滤镜</p>';
        return;
    }

    container.innerHTML = available.map(filter => `
        <div class="checkbox-group">
            <input type="checkbox" id="defaultFilter_${filter.id}" value="${filter.id}"
                ${defaults.includes(filter.id) ? 'checked' : ''}>
            <label for="defaultFilter_${filter.id}">[${filter.id}] ${filter.name}</label>
        </div>
    `).join('');
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

function handleAccountFilterData(data) {
    const container = $('accountFilterList');
    if (!data || !data.accounts || data.accounts.length === 0) {
        container.innerHTML = `
            <p style="color: var(--text-muted);">暂无账号滤镜配置数据</p>
            <p style="color: var(--text-secondary); font-size: 13px;">
                请确保实例已启动、已启用用户滤镜模式、且已绑定账号库
            </p>
        `;
        return;
    }

    container.innerHTML = data.accounts.map(account => {
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
            filtersHtml = `<div style="padding-left: 20px; margin-top: 4px; color: var(--text-muted); font-size: 13px;">
                无生效滤镜
            </div>`;
        }

        return `
            <div class="list-item" style="flex-direction: column; align-items: flex-start;">
                <div>
                    <span style="color: ${configColor}; font-weight: 600;">[${configType}]</span>
                    <span style="font-weight: 600;">${account.username}</span>
                </div>
                ${filtersHtml}
            </div>
        `;
    }).join('');
}

// Request initial data on page load
postAction('get_status');
postAction('get_notice');

// Initialize titlebar drag
initTitlebarDrag();

// Initialize login tabs
initLoginTabs();

// Initialize register and renew
initRegister();
initRenew();

// ========== Proxy Data Viewer Page ==========
let selectedProxyInstanceId = null;
let proxyDataRefreshInterval = null;

function initProxyDataPage() {
    // 加载 SOCKS 转发实例列表
    postAction('get_instances');
}

function renderProxyInstanceList(instances) {
    const container = $('proxyInstanceListPanel');
    if (!container) return;

    const socksForwardInstances = instances.filter(inst => inst.type === 'SocksForward');

    if (socksForwardInstances.length === 0) {
        container.innerHTML = '<div style="text-align:center; padding:20px; color:var(--text-muted); font-size:13px;">暂无SOCKS转发实例</div>';
        return;
    }

    container.innerHTML = socksForwardInstances.map(inst => `
        <div class="pool-item ${selectedProxyInstanceId === inst.id ? 'selected' : ''}"
             onclick="selectProxyInstance('${inst.id}', '${inst.name}')"
             style="cursor:pointer; padding:12px; margin-bottom:8px; border-radius:6px; background:${selectedProxyInstanceId === inst.id ? 'rgba(110, 231, 231, 0.1)' : 'transparent'}; border:1px solid ${selectedProxyInstanceId === inst.id ? '#6ee7e7' : 'var(--border)'};">
            <div style="font-size:13px; color:var(--text); font-weight:500;">${inst.name}</div>
            <div style="font-size:11px; color:var(--text-muted); margin-top:4px;">端口: ${inst.port} | ${inst.state === 'Running' ? '<span style="color:#0f0;">运行中</span>' : '<span style="color:#888;">已停止</span>'}</div>
        </div>
    `).join('');
}

function selectProxyInstance(instanceId, instanceName) {
    selectedProxyInstanceId = instanceId;

    // 重新渲染实例列表以更新选中状态
    postAction('get_instances');

    // 加载数据包
    loadProxyDataForInstance(instanceId, instanceName);

    // 启动自动刷新（每2秒）
    if (proxyDataRefreshInterval) {
        clearInterval(proxyDataRefreshInterval);
    }
    proxyDataRefreshInterval = setInterval(() => {
        if (selectedProxyInstanceId === instanceId) {
            postAction('config_get_proxydata', { instanceId });
        }
    }, 2000);
}

function loadProxyDataForInstance(instanceId, instanceName) {
    const content = $('proxyDataPanelContent');
    content.innerHTML = `
        <div style="margin-bottom:16px;">
            <span style="color:#6ee7e7; font-size:16px; font-weight:600;">实例: ${instanceName}</span>
            <span style="color:var(--text-muted); font-size:13px; margin-left:8px;">(ID: ${instanceId})</span>
        </div>
        <div style="display:flex; align-items:center; gap:16px; margin-bottom:16px; padding:12px; background:rgba(110,231,231,0.05); border:1px solid var(--border); border-radius:var(--radius-sm);">
            <label style="display:flex; align-items:center; gap:8px; cursor:pointer; font-size:13px; color:var(--text);">
                <span>记录数据包</span>
                <input type="checkbox" id="proxyRecordEnabled" onchange="saveProxyDataSettings('${instanceId}')" style="width:16px; height:16px; cursor:pointer;">
            </label>
            <div style="display:flex; align-items:center; gap:6px; font-size:13px; color:var(--text);">
                <span>缓冲区大小:</span>
                <input type="number" id="proxyBufferSize" min="10" max="10000" value="200" style="width:80px; padding:4px 8px; background:var(--bg-secondary); border:1px solid var(--border); border-radius:4px; color:var(--text); font-size:13px;">
                <span style="color:var(--text-muted);">条</span>
                <button class="btn btn-primary" onclick="saveProxyDataSettings('${instanceId}')" style="padding:4px 12px; font-size:12px;">保存</button>
            </div>
        </div>
        <div class="toolbar" style="margin-bottom:16px;">
            <button class="btn btn-primary" onclick="refreshProxyData('${instanceId}')">刷新</button>
            <button class="btn btn-danger" onclick="clearProxyDataForInstance('${instanceId}')">清空</button>
            <span style="color:var(--text-secondary); font-size:12px; margin-left:auto;">
                <span style="color:#4a9eff;">■</span> 请求 (客户端→服务器) &nbsp;&nbsp;
                <span style="color:#ffd700;">■</span> 响应 (服务器→客户端)
            </span>
        </div>
        <div id="proxyDataTableContainer"></div>
    `;

    postAction('config_get_proxydata', { instanceId });
}

function refreshProxyData(instanceId) {
    postAction('config_get_proxydata', { instanceId });
}

function clearProxyDataForInstance(instanceId) {
    if (confirm('确定要清空所有代理数据记录吗？')) {
        postAction('config_clear_proxydata', { instanceId });
        setTimeout(() => refreshProxyData(instanceId), 500);
    }
}

function saveProxyDataSettings(instanceId) {
    const enabledCheckbox = $('proxyRecordEnabled');
    const bufferSizeInput = $('proxyBufferSize');
    if (!enabledCheckbox || !bufferSizeInput) return;

    const enabled = enabledCheckbox.checked;
    let bufferSize = parseInt(bufferSizeInput.value) || 200;
    if (bufferSize < 10) bufferSize = 10;
    if (bufferSize > 10000) bufferSize = 10000;
    bufferSizeInput.value = bufferSize;

    postAction('config_set_proxydata_settings', { instanceId, enabled, bufferSize });
}

function handleProxyDataList(data) {
    const container = $('proxyDataTableContainer');
    if (!container) return;

    // 同步记录开关和缓冲区大小控件
    const enabledCheckbox = $('proxyRecordEnabled');
    const bufferSizeInput = $('proxyBufferSize');
    if (enabledCheckbox && data.enabled !== undefined) {
        enabledCheckbox.checked = data.enabled;
    }
    if (bufferSizeInput && data.bufferSize !== undefined) {
        bufferSizeInput.value = data.bufferSize;
    }

    if (!data || !data.packets || data.packets.length === 0) {
        container.innerHTML = '<div class="info-hint">' + (data.enabled ? '暂无数据包记录' : '数据包记录未开启，请勾选"记录数据包"开始记录') + '</div>';
        return;
    }

    // 保存packets数据供双击查看详情
    window._proxyPackets = data.packets;

    container.innerHTML = `
        <div style="overflow-x:auto; max-height:calc(100vh - 350px); border:1px solid var(--border); border-radius:var(--radius-sm);">
            <table class="data-table" style="font-size:12px;">
                <thead>
                    <tr>
                        <th style="width:80px;">时间</th>
                        <th style="width:100px;">用户名</th>
                        <th style="width:100px;">方向</th>
                        <th style="width:80px;">长度</th>
                        <th>数据预览 (Hex)</th>
                    </tr>
                </thead>
                <tbody>
                    ${data.packets.map((pkt, idx) => {
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
                    }).join('')}
                </tbody>
            </table>
        </div>
    `;
}

function showPacketHexDetail(index) {
    const pkt = window._proxyPackets && window._proxyPackets[index];
    if (!pkt) return;

    const directionText = pkt.isRequest ? '请求 (客户端→服务器)' : '响应 (服务器→客户端)';
    const directionColor = pkt.isRequest ? '#4a9eff' : '#ffd700';
    const hexStr = pkt.fullDataHex || pkt.dataPreview || '';

    // 将hex字符串转为标准hex dump格式（地址 | hex | ASCII）
    const bytes = hexStr.split(' ').filter(b => b && b !== '...');
    let hexDump = '';
    const bytesPerLine = 16;
    for (let i = 0; i < bytes.length; i += bytesPerLine) {
        const offset = i.toString(16).toUpperCase().padStart(8, '0');
        const lineBytes = bytes.slice(i, i + bytesPerLine);

        // hex部分，每8字节加额外空格
        let hexPart = '';
        for (let j = 0; j < bytesPerLine; j++) {
            if (j === 8) hexPart += ' ';
            if (j < lineBytes.length) {
                hexPart += lineBytes[j].toUpperCase() + ' ';
            } else {
                hexPart += '   ';
            }
        }

        // ASCII部分
        let asciiPart = '';
        for (let j = 0; j < lineBytes.length; j++) {
            const val = parseInt(lineBytes[j], 16);
            asciiPart += (val >= 0x20 && val <= 0x7e) ? String.fromCharCode(val) : '.';
        }

        hexDump += offset + '  ' + hexPart + ' |' + asciiPart + '|\n';
    }

    // 创建弹窗
    const overlay = document.createElement('div');
    overlay.id = 'hexDetailOverlay';
    overlay.style.cssText = 'position:fixed; top:0; left:0; right:0; bottom:0; background:rgba(0,0,0,0.7); z-index:10000; display:flex; align-items:center; justify-content:center;';
    overlay.onclick = function(e) { if (e.target === overlay) overlay.remove(); };

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
                <div style="font-family:'Consolas','Courier New',monospace; font-size:12px; line-height:1.6; color:#e0e0e0; background:#0d0d1a; padding:12px 16px; border-radius:6px; white-space:pre; overflow-x:auto; border:1px solid rgba(255,255,255,0.06);"><span style="color:#666;">Offset    00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F  |ASCII           |</span>
<span style="color:#666;">${'─'.repeat(77)}</span>
${hexDump}</div>
            </div>
        </div>
    `;

    document.body.appendChild(overlay);
}

// 页面切换时清理定时器
const originalNavigateTo = navigateTo;
navigateTo = function(pageId) {
    if (pageId !== 'proxydata' && proxyDataRefreshInterval) {
        clearInterval(proxyDataRefreshInterval);
        proxyDataRefreshInterval = null;
    }
    if (pageId === 'proxydata') {
        initProxyDataPage();
    }
    originalNavigateTo(pageId);
};
