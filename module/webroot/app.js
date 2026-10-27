/**
 * ZRAM PSI Recompress - WebUI App
 * KernelSU Module WebUI
 */

import { exec, toast, moduleInfo } from './kernelsu.js';

// ========== 全局状态 ==========
let moduleDir = '';
let currentLogType = 'psi';
let autoRefreshTimer = null;

/*
 * WebUI 构建标记：每次改动 WebUI 就 +1。
 * KernelSU 的 WebView 会缓存 index.html / app.js，
 * 刷了新模块后页面可能仍是旧的，而 module.prop 版本号
 * 由 app.js 实时读取、永远显示最新 —— 无法据此判断页面是否新鲜。
 * 页面上显示此标记，与仓库当前值比对即可确认。
 */
const WEBUI_BUILD = 9;

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
            const b = document.getElementById('info-build');
            if (b) b.textContent = String(WEBUI_BUILD);
        }
    } catch (e) {
        moduleDir = '/data/adb/modules/ZRAM-PSI-Recompress';
    }

    // 初始化滑块（绑定事件 + 数值气泡）
    initSliders();

    // 页面版本自检（HTML 是否被 WebView 缓存住）
    checkPageFreshness();

    // 加载配置
    await loadConfig();
    await loadZramOptions();

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

// ========== 页面版本自检 ==========

/*
 * KernelSU WebView 会缓存 index.html，刷了新模块后页面可能仍是旧版。
 * 旧版 HTML 缺少新版新增的 DOM（apply 日志 tab、弹窗、构建标记行），
 * 表现为「功能能跑但界面缺东西」，极难定位。
 *
 * 逐个检查这些标记元素，缺失则说明 HTML 是缓存的旧版，
 * 直接提示清缓存，而不是让用户自己猜。
 */
function checkPageFreshness() {
    const marks = [
        ['modal-mask', '确认弹窗（v1.3.1）'],
        ['info-build', '构建标记行（v1.3.2）'],
    ];
    const missing = marks.filter(([id]) => !document.getElementById(id))
                         .map(([, d]) => d);
    const applyTab = document.querySelector('.log-tab[data-log="apply"]');
    if (!applyTab) missing.push('apply 日志 tab（v1.3.1）');

    if (missing.length) {
        const msg = `当前页面是旧版缓存，缺少：${missing.join('、')}。\n` +
                    `请在 KernelSU 管理器中清除应用数据，或重启手机后重开。`;
        toast(msg, -1);
        console.warn('[ZRAM] WebUI 页面疑似旧版缓存:', missing);
        return false;
    }
    return true;
}

// ========== 弹窗 ==========

/*
 * KernelSU 的 WebView 未实现原生 confirm()
 * （kernelsu.js 里没有任何对话框相关代码，WebView 没配 onJsConfirm），
 * 原生 confirm() 会立即返回 false，导致点了按钮"没反应"。
 * 故自绘弹窗替代。
 */
function showConfirm(title, body, okText = '确定') {
    return new Promise(resolve => {
        const mask = document.getElementById('modal-mask');
        const okBtn = document.getElementById('modal-ok');
        const cancelBtn = document.getElementById('modal-cancel');
        if (!mask || !okBtn || !cancelBtn) {
            /* 弹窗 DOM 缺失时不能静默失败，退回原生以免功能彻底不可用 */
            resolve(window.confirm(body));
            return;
        }
        document.getElementById('modal-title').textContent = title;
        document.getElementById('modal-body').textContent = body;
        okBtn.textContent = okText;
        mask.classList.add('show');

        const done = v => {
            mask.classList.remove('show');
            okBtn.onclick = null;
            cancelBtn.onclick = null;
            resolve(v);
        };
        okBtn.onclick = () => done(true);
        cancelBtn.onclick = () => done(false);
        mask.onclick = e => { if (e.target === mask) done(false); };
    });
}

// ========== ZRAM 配置 ==========

/*
 * 从内核接口抓取算法列表。
 * comp_algorithm 形如：
 *   lzo lzo-rle lz4 zstd lz4k lz4kd zstdn zstdn_o [lz4kd]
 * 方括号标记当前选中项，需剥离。
 * recomp_algorithm 形如两行 "#1: ..." "#2: ..."。
 * 抓取而非写死，避免列出内核实际不支持的算法。
 */
function parseAlgoList(raw) {
    if (!raw) return [];
    const cleaned = raw.replace(/\[|\]/g, ' ');
    return cleaned.split(/\s+/).map(x => x.trim())
                  /* 剔除 recomp_algorithm 每行的 "#1:" / "#2:" 序号标记 */
                  .filter(x => x && x !== '#' && !/^#?\d+:$/.test(x));
}

async function loadZramOptions() {
    try {
        // 主算法
        const { stdout: compRaw } = await exec(
            `cat /sys/block/zram0/comp_algorithm 2>/dev/null || echo ""`
        );
        const compList = parseAlgoList(compRaw.trim());
        // 重压缩算法（与 comp 列表在 Oplus 内核上基本一致，但以 recomp 接口为准更可靠）
        const { stdout: recompRaw } = await exec(
            `cat /sys/block/zram0/recomp_algorithm 2>/dev/null || echo ""`
        );
        const recompList = parseAlgoList(recompRaw.trim());
        const rList = recompList.length ? [...new Set([...compList, ...recompList])] : compList;

        const fill = (id, list) => {
            const el = document.getElementById(id);
            if (!el || !list.length) return;
            el.innerHTML = list.map(a => `<option value="${a}">${a}</option>`).join('');
        };
        fill('cfg-algo', compList);
        fill('cfg-recomp1', rList);
        fill('cfg-recomp2', rList);

        // 当前主算法（方括号内）
        const curM = compRaw.match(/\[([^\]]+)\]/);
        if (curM) document.getElementById('cfg-algo').value = curM[1];

        // 当前重压缩一/二级：从 recomp_algorithm 各行的方括号提取
        const lines = recompRaw.trim().split('\n');
        const cur = lines.map(l => { const m = l.match(/\[([^\]]+)\]/); return m ? m[1] : ''; });
        if (cur[0]) document.getElementById('cfg-recomp1').value = cur[0];
        if (cur[1]) document.getElementById('cfg-recomp2').value = cur[1];

        // 当前大小
        const { stdout: sizeRaw } = await exec(
            `cat /sys/block/zram0/disksize 2>/dev/null || echo "0"`
        );
        const sz = parseInt(sizeRaw.trim()) || 0;
        const sizeEl = document.getElementById('cfg-size');
        if (sz && [...sizeEl.options].some(o => parseInt(o.value) === sz)) {
            sizeEl.value = String(sz);
        }

        // config.prop 里的值优先（重启后以它为准）
        const { stdout: cfgRaw } = await exec(`cat ${moduleDir}/config.prop 2>/dev/null || echo ""`);
        const m1 = cfgRaw.match(/^RECOMP_ALGO1=(\S+)/m);
        const m2 = cfgRaw.match(/^RECOMP_ALGO2=(\S+)/m);
        if (m1 && document.querySelector(`#cfg-recomp1 option[value="${m1[1]}"]`))
            document.getElementById('cfg-recomp1').value = m1[1];
        if (m2 && document.querySelector(`#cfg-recomp2 option[value="${m2[1]}"]`))
            document.getElementById('cfg-recomp2').value = m2[1];

    } catch (e) {
        console.error('loadZramOptions error:', e);
    }
}

/* 应用重压缩算法：直接写 sysfs，不需要 swapoff */
async function applyRecompAlgo() {
    const a1 = document.getElementById('cfg-recomp1').value;
    const a2 = document.getElementById('cfg-recomp2').value;
    try {
        const { stdout: r1 } = await exec(
            `echo "algo=${a1} priority=1" > /sys/block/zram0/recomp_algorithm 2>&1; echo rc=$?`
        );
        const { stdout: r2 } = await exec(
            `echo "algo=${a2} priority=2" > /sys/block/zram0/recomp_algorithm 2>&1; echo rc=$?`
        );
        const ok1 = /rc=0/.test(r1), ok2 = /rc=0/.test(r2);

        // 写回 config.prop，保证重启后保持
        await exec(`sed -i 's/^RECOMP_ALGO1=.*/RECOMP_ALGO1=${a1}/' ${moduleDir}/config.prop 2>/dev/null; ` +
                   `grep -q '^RECOMP_ALGO1=' ${moduleDir}/config.prop || echo 'RECOMP_ALGO1=${a1}' >> ${moduleDir}/config.prop`);
        await exec(`sed -i 's/^RECOMP_ALGO2=.*/RECOMP_ALGO2=${a2}/' ${moduleDir}/config.prop 2>/dev/null; ` +
                   `grep -q '^RECOMP_ALGO2=' ${moduleDir}/config.prop || echo 'RECOMP_ALGO2=${a2}' >> ${moduleDir}/config.prop`);

        if (ok1 && ok2) {
            toast(`已应用：一级 ${a1} / 二级 ${a2}`);
        } else {
            /*
             * 运行时（zram 已初始化并 swapon）无法变更 recomp_algorithm，
             * 实测 rc=1。此时不能只报"失败"了事 —— 需明确告知
             * 必须重建 zram，并给出直接重建的入口。
             */
            const go = await showConfirm(
                '需重建才能生效',
                `运行时无法直接修改重压缩算法（rc≠0）。\n\n` +
                `zram 设备一旦初始化（disksize>0 并 swapon）就不再接受\n` +
                `recomp_algorithm 变更，必须 swapoff 重建。\n\n` +
                `目标：一级 ${a1} / 二级 ${a2}\n` +
                `重建需读回已换出数据，过程会卡顿。是否立即重建？`,
                '立即重建'
            );
            if (go) await applyZramDevice();
            else toast('已取消，算法未变更', -1);
        }
        await refreshStatus();
    } catch (e) {
        toast('应用失败', -1);
    }
}

/*
 * 应用主算法与大小：必须 swapoff → reset → 重设 → swapon。
 *
 * 风险点在 swapoff：已换出的页要全部读回内存。
 * 若可用内存不足，会触发 OOM 杀进程甚至卡死。
 * 故先用 mm_stat 第 1 字段（orig_data_size，解压后字节数）估算所需内存，
 * 与 MemAvailable 比较，不足则拒绝执行。
 */
async function applyZramDevice() {
    const algo = document.getElementById('cfg-algo').value;
    const size = document.getElementById('cfg-size').value;
    /* 重建会清空 recomp_algorithm，需一并重设回当前选择 */
    const a1 = document.getElementById('cfg-recomp1').value;
    const a2 = document.getElementById('cfg-recomp2').value;

    try {
        // ---- 安全检查 ----
        const { stdout: memRaw } = await exec(`awk '/MemAvailable/{print $2}' /proc/meminfo`);
        const { stdout: mmRaw } = await exec(`awk '{print $1}' /sys/block/zram0/mm_stat`);
        const availKB = parseInt(memRaw.trim()) || 0;
        const origBytes = parseInt(mmRaw.trim()) || 0;
        const needKB = Math.floor(origBytes / 1024);
        const sizeGB = (parseInt(size) / 1073741824).toFixed(0);

        /*
         * 余量系数 1.15：解压过程本身需要少量额外内存。
         * 不足时不直接拒绝 —— 内核 LMK 会先杀后台腾空间，
         * 多数情况下仍能完成，只是体验差。
         * 改为强警告 + 二次确认，把决定权交给用户。
         */
        const enough = availKB >= needKB * 1.15;
        const needMiB = (needKB / 1024).toFixed(0);
        const availMiB = (availKB / 1024).toFixed(0);

        let confirmMsg;
        if (enough) {
            confirmMsg =
                `即将重建 zram：\n算法 ${algo}，大小 ${sizeGB} GB\n\n` +
                `需读回 ${needMiB} MiB，当前可用 ${availMiB} MiB，内存充足。\n` +
                `过程会有短暂卡顿，确定继续？`;
        } else {
            confirmMsg =
                `⚠️ 可用内存可能不足！\n\n` +
                `需读回 ${needMiB} MiB，当前可用仅 ${availMiB} MiB。\n` +
                `swapoff 期间系统会杀后台腾空间，可能明显卡顿甚至短暂无响应。\n\n` +
                `强烈建议先清理后台。仍要继续？`;
        }
        const go = await showConfirm('重建 ZRAM', confirmMsg, '仍要继续');
        if (!go) { toast('已取消'); return; }

        toast('正在重建 zram，请勿操作...');

        /*
         * 分步执行并记录每步返回值到 zram_apply.log。
         * 原先整条命令串成一行，任何一步失败都看不出来；
         * 且 swapoff 大容量耗时可能较长，需要可追溯的执行记录。
         */
        const LOG = `${moduleDir}/zram_apply.log`;
        await exec(`: > ${LOG}`);
        await exec(`echo "$(date '+%F %H:%M:%S') apply algo=${algo} size=${size}" >> ${LOG}`);

        /*
         * 顺序必须与 service.sh 一致：
         * recomp_algorithm 只能在 zram 未初始化（disksize=0）时写入。
         * 实测把它放在 disksize/swapon 之后会 rc=1 失败 ——
         * 设备一旦初始化就不再接受重压缩算法变更。
         * 故 recomp 必须排在 disksize 之前。
         */
        const steps = [
            ['swapoff',      `swapoff /dev/block/zram0 2>&1`],
            ['reset',        `echo 1 > /sys/block/zram0/reset 2>&1`],
            /* reset 后 disksize 已为 0，再写 0 可能返回 EINVAL，属可容忍 */
            ['disksize0',    `echo 0 > /sys/block/zram0/disksize 2>&1`, true],
            ['streams',      `echo 8 > /sys/block/zram0/max_comp_streams 2>&1`],
            ['comp_algo',    `echo '${algo}' > /sys/block/zram0/comp_algorithm 2>&1`],
            ['recomp1',      `echo "algo=${a1} priority=1" > /sys/block/zram0/recomp_algorithm 2>&1`],
            ['recomp2',      `echo "algo=${a2} priority=2" > /sys/block/zram0/recomp_algorithm 2>&1`],
            ['disksize',     `echo '${size}' > /sys/block/zram0/disksize 2>&1`],
            ['mkswap',       `mkswap /dev/block/zram0 2>&1`],
            ['swapon',       `swapon /dev/block/zram0 2>&1`],
        ];

        let failed = [];
        let tolerant = [];
        for (const [name, cmd, canFail] of steps) {
            const { errno, stdout, stderr } = await exec(`${cmd}; echo "rc=$?"`);
            const rc = (stdout || '').match(/rc=(\d+)/);
            const code = rc ? parseInt(rc[1]) : -1;
            const out = ((stdout || '') + (stderr || '')).replace(/rc=\d+/, '').trim();
            await exec(`echo "[${name}] rc=${code}${canFail ? ' (可容忍)' : ''} ${out}" >> ${LOG}`);
            if (code !== 0) {
                if (canFail) tolerant.push(`${name}(rc=${code})`);
                else failed.push(`${name}(rc=${code})`);
            }
            /* reset 后需给内核一点时间完成设备回收 */
            if (name === 'reset') await exec(`sleep 1`);
        }

        /* 复核重压缩算法是否真正生效 */
        const { stdout: rcNow } = await exec(`cat /sys/block/zram0/recomp_algorithm 2>&1`);
        await exec(`echo "--- recomp_algorithm ---" >> ${LOG}`);
        await exec(`echo "${rcNow.trim().replace(/"/g, "'")}" >> ${LOG}`);
        const recompOk = /\bzstdn\b|\bzstdn_o\b|\blz4\b|\blzo\b|\bzstd\b/.test(rcNow);

        // 写回 config.prop
        await exec(`sed -i 's/^ZRAM_ALGO=.*/ZRAM_ALGO=${algo}/' ${moduleDir}/config.prop 2>/dev/null`);
        await exec(`sed -i 's/^ZRAM_SIZE=.*/ZRAM_SIZE=${size}/' ${moduleDir}/config.prop 2>/dev/null`);

        if (failed.length) {
            toast(`部分步骤失败：${failed.join(', ')}，详见 zram_apply.log`, -1);
        } else if (!recompOk) {
            toast(`已重建，但重压缩算法未生效（recomp_algorithm 为空）——重压缩将无效`, -1);
        } else if (tolerant.length) {
            toast(`已重建：${algo} / ${sizeGB} GB（${tolerant.join(', ')} 失败，不影响）`);
        } else {
            toast(`已重建：${algo} / ${sizeGB} GB`);
        }
        await refreshStatus();
    } catch (e) {
        toast('重建失败', -1);
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
        /* 必须在 if 块外声明：下方 thrEl 处会用到，
           块内 const 会导致 ReferenceError 被 catch 静默吞掉，
           连带后面「距上次压缩」也不再执行，两格恒显示 - */
        let throttled = 0;

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
            throttled = parseInt(st.psi_throttled) || 0;
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
/*
 * 检测各日志文件是否存在，不存在则把 tab 置灰并提示原因。
 * recompress.log 只在 helper 启动失败、回退到 shell 循环时才创建，
 * 正常走 PSI 时不会生成 —— 不提示会让人以为模块有问题。
 */
async function updateLogTabs() {
    const files = {
        psi: 'psi.log',
        module: 'zram_module.log',
        recompress: 'recompress.log',
        apply: 'zram_apply.log',
    };
    const reasons = {
        recompress: '未生成（正常）：仅当 PSI 守护启动失败、回退 shell 循环时才会创建',
        apply: '未生成：尚未在 WebUI 执行过「应用主算法与大小」',
    };
    for (const [key, name] of Object.entries(files)) {
        const btn = document.querySelector(`.log-tab[data-log="${key}"]`);
        if (!btn) continue;
        const { stdout } = await exec(`test -s ${moduleDir}/${name} && echo y || echo n`);
        if (stdout.trim() === 'y') {
            btn.classList.remove('disabled');
            btn.title = '';
        } else {
            btn.classList.add('disabled');
            btn.title = reasons[key] || '文件不存在或为空';
        }
    }
}

async function refreshLog() {
    const viewer = document.getElementById('log-viewer');

    let logFile;
    switch (currentLogType) {
        case 'psi': logFile = 'psi.log'; break;
        case 'module': logFile = 'zram_module.log'; break;
        case 'recompress': logFile = 'recompress.log'; break;
        case 'apply': logFile = 'zram_apply.log'; break;
        default: logFile = 'psi.log';
    }

    try {
        await updateLogTabs();
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
        case 'apply': logFile = 'zram_apply.log'; break;
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
        const { stdout: psOut } = await exec(
            `ps -A 2>/dev/null | grep -c "[p]si_recomp" || echo 0`
        );
        const alive = parseInt(psOut.trim()) > 0;
        if (alive) {
            await exec(`${moduleDir}/psi_recomp --pid 2>/dev/null; true`);
            const { stdout: p } = await exec(`ps -A 2>/dev/null | grep "[p]si_recomp" | awk '{print $2}' | head -1`);
            if (p.trim()) await exec(`echo ${p.trim()} > ${moduleDir}/psi_recomp.pid`);
        }

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

/*
 * 以下三个也必须暴露，否则 HTML 里的 onclick/oninput 会报
 * "ReferenceError: xxx is not defined"，表现为点了按钮毫无反应。
 *
 * 成因：index.html 用 <script type="module"> 加载 app.js，
 * ES module 顶层声明只存在于模块作用域，不会挂到 window，
 * 而内联 onclick 是在全局作用域里求值的。
 * v1.3.0 新增这几个函数时漏加了暴露，导致按钮静默失效。
 */
window.applyRecompAlgo = applyRecompAlgo;
window.applyZramDevice = applyZramDevice;
window.updateSliderVal = updateSliderVal;
