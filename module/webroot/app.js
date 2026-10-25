/**
 * ZRAM PSI Recompress - WebUI App
 * KernelSU Module WebUI
 */

import { exec, toast, moduleInfo } from './kernelsu.js';

// ========== 全局状态 ==========
let moduleDir = '';
let currentLogType = 'psi';
let autoRefreshTimer = null;

// ========== 初始化 ==========
document.addEventListener('DOMContentLoaded', async () => {
    await init();
});

async function init() {
    try {
        // 获取模块信息
        const info = moduleInfo();
        if (info) {
            const parsed = JSON.parse(info);
            moduleDir = parsed.moduleDir || '/data/adb/modules/ZRAM-PSI-Recompress';
            document.getElementById('info-version').textContent = parsed.version || '-';
            document.getElementById('info-path').textContent = moduleDir;
        }
    } catch (e) {
        moduleDir = '/data/adb/modules/ZRAM-PSI-Recompress';
    }

    // 初始化滑块（绑定事件 + 数值气泡）
    initSliders();

    // 加载配置
    await loadConfig();

    // 刷新状态
    await refreshStatus();
    await refreshStats();

    // 自动刷新（每 30 秒）
    startAutoRefresh();
}

// ========== 状态刷新 ==========
async function refreshStatus() {
    try {
        // 检查守护进程
        const { errno: pidErrno, stdout: pidOut } = await exec(
            `cat ${moduleDir}/psi_recomp.pid 2>/dev/null || echo ""`
        );
        const pid = pidOut.trim();

        const daemonEl = document.getElementById('daemon-status');
        const pidEl = document.getElementById('daemon-pid');
        const badge = document.getElementById('status-badge');

        if (pid) {
            // 验证进程是否存活
            const { errno: aliveErrno } = await exec(`kill -0 ${pid} 2>/dev/null && echo "alive" || echo "dead"`);
            const isAlive = aliveErrno === 0;

            if (isAlive) {
                daemonEl.textContent = '运行中';
                daemonEl.className = 'status-value ok';
                pidEl.textContent = pid;
                badge.textContent = '正常';
                badge.className = 'badge badge-success';
            } else {
                daemonEl.textContent = '已停止';
                daemonEl.className = 'status-value err';
                pidEl.textContent = pid + ' (僵尸)';
                badge.textContent = '异常';
                badge.className = 'badge badge-danger';
            }
        } else {
            daemonEl.textContent = '未运行';
            daemonEl.className = 'status-value warn';
            pidEl.textContent = '-';
            badge.textContent = '未启动';
            badge.className = 'badge badge-warning';
        }

        // 读取当前 huge 页数
        const { errno: hugeErrno, stdout: hugeOut } = await exec(
            `awk '{print $8}' /sys/block/zram0/mm_stat 2>/dev/null || echo "-1"`
        );
        const huge = parseInt(hugeOut.trim());
        const hugeEl = document.getElementById('current-huge');
        if (huge >= 0) {
            hugeEl.textContent = huge.toLocaleString();
            hugeEl.className = 'status-value';
        } else {
            hugeEl.textContent = 'N/A';
            hugeEl.className = 'status-value err';
        }

        // 读取阈值（从 psi.conf 或默认值）
        const { stdout: threshOut } = await exec(
            `grep "^THRESH_NORMAL=" ${moduleDir}/psi.conf 2>/dev/null | cut -d= -f2 || echo "40000"`
        );
        document.getElementById('current-thresh').textContent = parseInt(threshOut.trim()).toLocaleString();

        // PSI 状态
        const { errno: psiErrno } = await exec(`test -e /proc/pressure/memory && echo "ok" || echo "missing"`);
        const psiEl = document.getElementById('psi-status');
        if (psiErrno === 0) {
            psiEl.textContent = '可用';
            psiEl.className = 'status-value ok';
        } else {
            psiEl.textContent = '不可用';
            psiEl.className = 'status-value warn';
        }

        // 看门狗状态（读心跳时间戳）
        const { stdout: wdOut } = await exec(
            `cat ${moduleDir}/psi_watchdog.heartbeat 2>/dev/null || echo "0"`
        );
        const wdTs = parseInt(wdOut.trim()) || 0;
        const wdEl = document.getElementById('watchdog-status');
        if (wdEl) {
            if (wdTs > 0) {
                const age = Math.floor(Date.now() / 1000) - wdTs;
                /* 超过 2 个检测周期没心跳，认为已停止 */
                if (age <= 700) {
                    wdEl.textContent = '运行中';
                    wdEl.className = 'status-value ok';
                } else {
                    wdEl.textContent = '已停止';
                    wdEl.className = 'status-value warn';
                }
            } else {
                wdEl.textContent = '未启用';
                wdEl.className = 'status-value';
            }
        }

        // ZRAM 算法
        const { stdout: algoOut } = await exec(
            `cat /sys/block/zram0/comp_algorithm 2>/dev/null | grep -o '\\[.*\\]' | tr -d '[]' || echo "-"`
        );
        document.getElementById('zram-algo').textContent = algoOut.trim() || '-';

        // ZRAM 大小
        const { stdout: sizeOut } = await exec(
            `cat /sys/block/zram0/disksize 2>/dev/null || echo "0"`
        );
        const sizeBytes = parseInt(sizeOut.trim());
        if (sizeBytes > 0) {
            const sizeGB = (sizeBytes / 1024 / 1024 / 1024).toFixed(1);
            document.getElementById('info-zram-size').textContent = sizeGB + ' GB';
        } else {
            document.getElementById('info-zram-size').textContent = '-';
        }

        // ZRAM 算法（配置）
        const { stdout: cfgAlgoOut } = await exec(
            `grep "^ZRAM_ALGO=" ${moduleDir}/config.prop 2>/dev/null | cut -d= -f2 || echo "-"`
        );
        document.getElementById('info-zram-algo').textContent = cfgAlgoOut.trim() || '-';

    } catch (e) {
        console.error('refreshStatus error:', e);
    }
}

// ========== 统计刷新 ==========

/* 解析 key=value 格式的 psi_stats 状态文件 */
function parseStatsFile(content) {
    const out = {};
    content.split('\n').forEach(line => {
        const t = line.trim();
        if (!t) return;
        const i = t.indexOf('=');
        if (i < 0) return;
        out[t.substring(0, i).trim()] = t.substring(i + 1).trim();
    });
    return out;
}

async function refreshStats() {
    try {
        let checks = 0, psiEvents = 0, compress = 0, saved = 0, peak = 0, huge = -1;
        let lastComp = 0;

        /*
         * 优先读取守护进程实时写入的 psi_stats。
         * 原先只解析日志里的 stats/exit 行，而 stats 每 10 次压缩才输出、
         * exit 只在进程退出时输出 —— 运行中两者都没有，导致恒显示 0。
         */
        const { stdout: statOut } = await exec(
            `cat ${moduleDir}/psi_stats 2>/dev/null || echo ""`
        );
        const st = parseStatsFile(statOut);

        if (st.checks !== undefined) {
            checks = parseInt(st.checks) || 0;
            psiEvents = parseInt(st.psi_events) || 0;
            const throttled = parseInt(st.psi_throttled) || 0;
            compress = parseInt(st.recompress) || 0;
            peak = parseInt(st.peak) || 0;
            saved = parseInt(st.total_saved) || 0;
            huge = parseInt(st.huge);
            lastComp = parseInt(st.last_comp) || 0;
        } else {
            /* 回退：老版本守护进程没有状态文件，仍解析日志 */
            const { stdout: logOut } = await exec(
                `tail -50 ${moduleDir}/psi.log 2>/dev/null || echo ""`
            );
            const statsMatch = logOut.match(/stats: total_comp=(\d+), total_saved=(\d+) pages.*?peak=(\d+)/);
            const exitMatch = logOut.match(/exit: checks=(\d+) psi_events=(\d+) recompress=(\d+) peak=(\d+) total_saved=(\d+) pages/);
            if (statsMatch) {
                compress = parseInt(statsMatch[1]);
                saved = parseInt(statsMatch[2]);
                peak = parseInt(statsMatch[3]);
            } else if (exitMatch) {
                checks = parseInt(exitMatch[1]);
                psiEvents = parseInt(exitMatch[2]);
                compress = parseInt(exitMatch[3]);
                peak = parseInt(exitMatch[4]);
                saved = parseInt(exitMatch[5]);
            }
            const { stdout: lineCount } = await exec(
                `wc -l < ${moduleDir}/psi.log 2>/dev/null || echo "0"`
            );
            checks = Math.max(checks, parseInt(lineCount.trim()) || 0);
        }

        /* 4096 字节/页 -> MiB：pages / 256 */
        const savedMiB = (saved / 256).toFixed(1);

        document.getElementById('stat-checks').textContent = checks.toLocaleString();
        document.getElementById('stat-compress').textContent = compress.toLocaleString();
        document.getElementById('stat-saved').textContent = savedMiB;
        document.getElementById('stat-peak').textContent = peak > 0 ? peak.toLocaleString() : '-';

        /* 有 psi_events 字段时顺带展示 PSI 事件占比 */
        const psiEl = document.getElementById('stat-psi-events');
        if (psiEl) {
            /* psi_events 恒 <= checks，占比不会超过 100% */
            const pct = checks > 0 ? Math.min(100, Math.round(psiEvents / checks * 100)) : 0;
            psiEl.textContent = checks > 0
                ? psiEvents.toLocaleString() + ' / ' + checks.toLocaleString() + ' (' + pct + '%)'
                : '-';
        }
        const thrEl = document.getElementById('stat-throttled');
        if (thrEl) {
            thrEl.textContent = throttled > 0
                ? throttled.toLocaleString() + ' 次'
                : (checks > 0 ? '0' : '-');
        }

        /*
         * 「当前 huge」在「运行状态」卡片里已有实时读取的版本
         * （直接读 /sys/block/zram0/mm_stat），此处不再重复显示
         * psi_stats 里的快照值，改为展示更有用的「距上次压缩」。
         */
        const lastCompEl = document.getElementById('stat-last-comp');
        if (lastCompEl) {
            if (lastComp > 0) {
                const diff = Math.floor(Date.now() / 1000) - lastComp;
                let txt;
                if (diff < 60) txt = diff + ' 秒前';
                else if (diff < 3600) txt = Math.floor(diff / 60) + ' 分钟前';
                else txt = (diff / 3600).toFixed(1) + ' 小时前';
                lastCompEl.textContent = txt;
            } else {
                lastCompEl.textContent = compress > 0 ? '-' : '从未';
            }
        }

    } catch (e) {
        console.error('refreshStats error:', e);
    }
}

// ========== 滑块辅助（v1.2.1） ==========

/* 滑块显示格式化：不同参数用不同单位 */
function formatSliderVal(id, v) {
    switch (id) {
        case 'cfg-hyst-sec':
        case 'cfg-idle-timeout': {
            const sec = Number(v);
            if (sec === 0) return '关闭';
            if (sec < 60) return sec + ' 秒';
            const m = sec / 60;
            return (Number.isInteger(m) ? m : m.toFixed(1)) + ' 分钟';
        }
        case 'cfg-min-gap':
            return Number(v) + ' 秒';
        case 'cfg-skip-every':
            return '每 ' + Number(v) + ' 次';
        default:
            return Number(v).toLocaleString();
    }
}

/* 实时更新滑块数值气泡 + 已填充进度色 */
function updateSliderVal(id) {
    const el = document.getElementById(id);
    const valEl = document.getElementById(id + '-val');
    if (!el || !valEl) return;
    valEl.textContent = formatSliderVal(id, el.value);

    const min = Number(el.min) || 0;
    const max = Number(el.max) || 100;
    const pct = ((Number(el.value) - min) / (max - min)) * 100;
    el.style.background =
        'linear-gradient(90deg, #4a9eff 0%, #4a9eff ' + pct + '%, #2a2f3a ' + pct + '%, #2a2f3a 100%)';
}

/* 页面加载后为所有滑块绑定事件并初始化显示 */
function initSliders() {
    const ids = ['cfg-thresh', 'cfg-hyst-sec', 'cfg-hyst-margin', 'cfg-min-gap',
                 'cfg-idle-timeout', 'cfg-log-max', 'cfg-log-keep', 'cfg-skip-every'];
    ids.forEach(id => {
        const el = document.getElementById(id);
        if (!el) return;
        el.addEventListener('input', () => updateSliderVal(id));
        updateSliderVal(id);
    });
}

// ========== 配置管理 ==========
async function loadConfig() {
    try {
        const { stdout } = await exec(`cat ${moduleDir}/psi.conf 2>/dev/null || echo ""`);
        parseConfig(stdout);
        showActionResult('配置已加载', 'success');
    } catch (e) {
        showActionResult('加载配置失败: ' + e.message, 'error');
    }
}

function parseConfig(content) {
    const lines = content.split('\n');
    const config = {};

    for (const line of lines) {
        const trimmed = line.trim();
        if (!trimmed || trimmed.startsWith('#')) continue;

        const eqIdx = trimmed.indexOf('=');
        if (eqIdx < 0) continue;

        const key = trimmed.substring(0, eqIdx).trim();
        const value = trimmed.substring(eqIdx + 1).trim();
        config[key] = value;
    }

    // 填充表单
    setInputValue('cfg-thresh', config.THRESH_NORMAL);
    setInputValue('cfg-hyst-sec', config.HYSTERESIS_SEC);
    setInputValue('cfg-hyst-margin', config.HYST_MARGIN);
    setInputValue('cfg-min-gap', config.MIN_GAP_SEC);
    setInputValue('cfg-idle-timeout', config.IDLE_TIMEOUT_MS);
    setInputValue('cfg-psi-trigger', config.PSI_TRIGGER);
    setInputValue('cfg-log-max', config.LOG_MAX_LINES);
    setInputValue('cfg-log-keep', config.LOG_KEEP_LINES);
    setInputValue('cfg-skip-every', config.SKIP_LOG_EVERY);
}

function setInputValue(id, value) {
    const el = document.getElementById(id);
    if (!el || value === undefined) return;

    /* IDLE_TIMEOUT_MS 在配置文件里是毫秒，滑块用秒显示 */
    let v = value;
    if (id === 'cfg-idle-timeout') {
        v = Math.round(Number(value) / 1000);
    }
    el.value = v;

    /* range 需要同步数值气泡和进度色 */
    if (el.type === 'range') updateSliderVal(id);
}

async function saveConfig() {
    try {
        const config = buildConfigFromForm();
        const content = generateConfigText(config);

        // 写入配置文件
        const escaped = content.replace(/'/g, "'\\''");
        const { errno } = await exec(`echo '${escaped}' > ${moduleDir}/psi.conf`);

        if (errno === 0) {
            showActionResult('配置已保存，重启模块后生效', 'success');
            toast('配置已保存');
        } else {
            showActionResult('保存配置失败', 'error');
        }
    } catch (e) {
        showActionResult('保存配置失败: ' + e.message, 'error');
    }
}

function buildConfigFromForm() {
    return {
        THRESH_NORMAL: getInputValue('cfg-thresh', '40000'),
        HYSTERESIS_SEC: getInputValue('cfg-hyst-sec', '5400'),
        HYST_MARGIN: getInputValue('cfg-hyst-margin', '8000'),
        MIN_GAP_SEC: getInputValue('cfg-min-gap', '60'),
        /* 滑块单位是秒，配置里是毫秒 */
        IDLE_TIMEOUT_MS: String(Number(getInputValue('cfg-idle-timeout', '1800')) * 1000),
        PSI_TRIGGER: getInputValue('cfg-psi-trigger', 'some 15000 1000000'),
        LOG_MAX_LINES: getInputValue('cfg-log-max', '600'),
        LOG_KEEP_LINES: getInputValue('cfg-log-keep', '300'),
        SKIP_LOG_EVERY: getInputValue('cfg-skip-every', '5'),
    };
}

function getInputValue(id, defaultVal) {
    const el = document.getElementById(id);
    return el && el.value ? el.value : defaultVal;
}

function generateConfigText(config) {
    return `# psi_recomp 配置文件
# 由 WebUI 生成于 ${new Date().toISOString()}

# 常态触发阈值（huge 页数）
THRESH_NORMAL=${config.THRESH_NORMAL}

# 滞回持续时间（秒）
HYSTERESIS_SEC=${config.HYSTERESIS_SEC}

# 滞回余量（页数）
HYST_MARGIN=${config.HYST_MARGIN}

# 两次检查的最小间隔（秒）
MIN_GAP_SEC=${config.MIN_GAP_SEC}

# 兜底轮询间隔（毫秒）
IDLE_TIMEOUT_MS=${config.IDLE_TIMEOUT_MS}

# PSI 触发条件
PSI_TRIGGER=${config.PSI_TRIGGER}

# 日志轮转
LOG_MAX_LINES=${config.LOG_MAX_LINES}
LOG_KEEP_LINES=${config.LOG_KEEP_LINES}

# Skip 日志降频
SKIP_LOG_EVERY=${config.SKIP_LOG_EVERY}
`;
}

async function resetConfig() {
    try {
        const defaultConfig = `THRESH_NORMAL=40000
HYSTERESIS_SEC=5400
HYST_MARGIN=8000
MIN_GAP_SEC=60
IDLE_TIMEOUT_MS=1800000
PSI_TRIGGER=some 15000 1000000
LOG_MAX_LINES=600
LOG_KEEP_LINES=300
SKIP_LOG_EVERY=5`;

        const escaped = defaultConfig.replace(/'/g, "'\\''");
        const { errno } = await exec(`echo '${escaped}' > ${moduleDir}/psi.conf`);

        if (errno === 0) {
            parseConfig(defaultConfig);
            showActionResult('已恢复默认配置', 'success');
            toast('已恢复默认配置');
        } else {
            showActionResult('恢复默认配置失败', 'error');
        }
    } catch (e) {
        showActionResult('恢复默认配置失败: ' + e.message, 'error');
    }
}

// ========== 日志管理 ==========
async function refreshLog() {
    const viewer = document.getElementById('log-viewer');

    let logFile;
    switch (currentLogType) {
        case 'psi': logFile = 'psi.log'; break;
        case 'module': logFile = 'zram_module.log'; break;
        case 'recompress': logFile = 'recompress.log'; break;
        default: logFile = 'psi.log';
    }

    try {
        const { stdout } = await exec(`tail -100 ${moduleDir}/${logFile} 2>/dev/null || echo "(日志文件不存在或为空)"`);
        viewer.textContent = stdout || '(空)';
        viewer.scrollTop = viewer.scrollHeight;
    } catch (e) {
        viewer.textContent = '读取日志失败: ' + e.message;
    }
}

function switchLog(type) {
    currentLogType = type;

    // 更新 tab 样式
    document.querySelectorAll('.log-tab').forEach(tab => {
        tab.classList.toggle('active', tab.dataset.log === type);
    });

    refreshLog();
}

async function clearLog() {
    try {
        let logFile;
        switch (currentLogType) {
            case 'psi': logFile = 'psi.log'; break;
            case 'module': logFile = 'zram_module.log'; break;
            case 'recompress': logFile = 'recompress.log'; break;
        }

        await exec(`: > ${moduleDir}/${logFile}`);
        showActionResult('日志已清空', 'success');
        refreshLog();
    } catch (e) {
        showActionResult('清空日志失败: ' + e.message, 'error');
    }
}

// ========== 快捷操作 ==========
async function triggerRecompress() {
    try {
        showActionResult('正在触发重压缩...', 'info');

        const { errno, stdout, stderr } = await exec(
            `echo "type=huge" > /sys/block/zram0/recompress 2>&1 && echo "OK" || echo "FAIL: $(cat /sys/block/zram0/recompress 2>&1)"`
        );

        if (errno === 0 && stdout.includes('OK')) {
            showActionResult('重压缩已触发', 'success');
            toast('重压缩已触发');

            // 延迟刷新统计
            setTimeout(async () => {
                await refreshStats();
                await refreshStatus();
            }, 2000);
        } else {
            showActionResult('触发失败: ' + (stderr || stdout), 'error');
        }
    } catch (e) {
        showActionResult('触发失败: ' + e.message, 'error');
    }
}

async function restartDaemon() {
    try {
        showActionResult('正在重启守护进程...', 'info');

        // 停止现有进程
        await exec(`kill $(cat ${moduleDir}/psi_recomp.pid 2>/dev/null) 2>/dev/null; sleep 1`);

        // 重新启动
        await exec(
            `chmod 0755 ${moduleDir}/psi_recomp 2>/dev/null; ` +
            `setsid ${moduleDir}/psi_recomp ${moduleDir}/psi.log ${moduleDir}/psi.conf &`
        );

        /* `cmd &` 的退出码恒为 0，无法判断是否真的起来了，
           必须等一小会儿后用 ps 实际检测进程。
           同时重写 PID 文件：旧进程退出时可能删掉新进程写的 PID。 */
        await new Promise(r => setTimeout(r, 1500));
        /* 扫描 /proc/<pid>/comm，比 ps|grep 可靠：
           不受 ps 输出格式影响，也不会把 grep 自身算进去 */
        const { stdout: psOut } = await exec(
            `p=""; for d in /proc/[0-9]*; do ` +
            `[ "$(cat $d/comm 2>/dev/null)" = "psi_recomp" ] && { p=${d#/proc/}; break; }; done; ` +
            `echo ${p:-0}`
        );
        const pidOut = psOut.trim().split(/\s+/).pop() || '0';
        const alive = parseInt(pidOut) > 0;
        if (alive) await exec(`echo ${pidOut} > ${moduleDir}/psi_recomp.pid`);

        if (alive) {
            showActionResult('守护进程已重启', 'success');
            toast('守护进程已重启');

            setTimeout(async () => {
                await refreshStatus();
                await refreshStats();
            }, 2000);
        } else {
            showActionResult('重启失败', 'error');
        }
    } catch (e) {
        showActionResult('重启失败: ' + e.message, 'error');
    }
}

async function testPSI() {
    try {
        showActionResult('正在测试 PSI...', 'info');

        const { errno, stdout } = await exec(
            `test -e /proc/pressure/memory && echo "PSI 接口存在" || echo "PSI 接口不存在"; ` +
            `cat /proc/pressure/memory 2>/dev/null | head -3`
        );

        if (errno === 0) {
            showActionResult('PSI 测试结果:\n' + stdout, 'success');
        } else {
            showActionResult('PSI 测试失败', 'error');
        }
    } catch (e) {
        showActionResult('PSI 测试失败: ' + e.message, 'error');
    }
}

// ========== 工具函数 ==========
function showActionResult(message, type = 'info') {
    const el = document.getElementById('action-result');
    el.textContent = message;
    el.className = 'action-result ' + type;
}

function startAutoRefresh() {
    if (autoRefreshTimer) clearInterval(autoRefreshTimer);
    autoRefreshTimer = setInterval(async () => {
        await refreshStatus();
        await refreshStats();
    }, 30000);
}

function stopAutoRefresh() {
    if (autoRefreshTimer) {
        clearInterval(autoRefreshTimer);
        autoRefreshTimer = null;
    }
}

// 页面隐藏时停止自动刷新
document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
        stopAutoRefresh();
    } else {
        startAutoRefresh();
    }
});

// 暴露到全局（供 HTML onclick 调用）
window.refreshStatus = refreshStatus;
window.refreshStats = refreshStats;
window.loadConfig = loadConfig;
window.saveConfig = saveConfig;
window.resetConfig = resetConfig;
window.refreshLog = refreshLog;
window.switchLog = switchLog;
window.clearLog = clearLog;
window.triggerRecompress = triggerRecompress;
window.restartDaemon = restartDaemon;
window.testPSI = testPSI;
