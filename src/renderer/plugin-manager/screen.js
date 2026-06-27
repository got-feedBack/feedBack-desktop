// Plugin Manager UI
(function() {
    'use strict';

    const plugins = window.feedBackDesktop?.plugins;
    if (!plugins) {
        const panel = document.getElementById('plugin-manager-panel');
        if (panel) panel.innerHTML = '<div class="p-8 text-center text-slate-400">Plugin manager is only available in the Slopsmith Desktop app.</div>';
        return;
    }

    const $ = (id) => document.getElementById(id);
    const gitUrlInput = $('pm-git-url');
    const installBtn = $('pm-install-btn');
    const installMsg = $('pm-install-msg');
    const listContainer = $('pm-list');
    const refreshBtn = $('pm-refresh');

    function showMessage(msg, success) {
        installMsg.textContent = msg;
        installMsg.className = `mt-2 text-sm ${success ? 'text-emerald-400' : 'text-red-400'}`;
        installMsg.classList.remove('hidden');
        setTimeout(() => installMsg.classList.add('hidden'), 5000);
    }

    async function refreshList() {
        listContainer.innerHTML = '<div class="text-sm text-slate-500">Loading...</div>';

        try {
            const installed = await plugins.listInstalled();

            if (installed.length === 0) {
                listContainer.innerHTML = '<div class="text-sm text-slate-500 italic">No user-installed plugins. Official plugins are loaded from the Slopsmith server.</div>';
                return;
            }

            listContainer.innerHTML = '';
            for (const plugin of installed) {
                const div = document.createElement('div');
                div.className = 'flex items-center gap-3 p-3 rounded bg-slate-800/50 border border-slate-700';

                const name = plugin.manifest?.name || plugin.name;
                const desc = plugin.manifest?.description || '';
                const version = plugin.version || 'unknown';

                div.innerHTML = `
                    <div class="flex-1">
                        <div class="text-sm font-medium text-slate-200">${name}</div>
                        <div class="text-xs text-slate-400">${desc}</div>
                        <div class="text-xs text-slate-500 mt-0.5">v${version}</div>
                    </div>
                    <div class="flex gap-2">
                        ${plugin.hasGit ? `<button class="pm-update text-xs px-2 py-1 rounded bg-blue-600 hover:bg-blue-500" data-name="${plugin.name}">Update</button>` : ''}
                        <button class="pm-remove text-xs px-2 py-1 rounded bg-red-600/50 hover:bg-red-500" data-name="${plugin.name}">Remove</button>
                    </div>
                `;
                listContainer.appendChild(div);
            }

            // Bind update buttons
            listContainer.querySelectorAll('.pm-update').forEach(btn => {
                btn.addEventListener('click', async () => {
                    btn.disabled = true;
                    btn.textContent = 'Updating...';
                    const result = await plugins.update(btn.dataset.name);
                    showMessage(result.message, result.success);
                    await refreshList();
                });
            });

            // Bind remove buttons
            listContainer.querySelectorAll('.pm-remove').forEach(btn => {
                btn.addEventListener('click', async () => {
                    if (!confirm(`Remove plugin "${btn.dataset.name}"?`)) return;
                    btn.disabled = true;
                    const result = await plugins.remove(btn.dataset.name);
                    showMessage(result.message, result.success);
                    await refreshList();
                });
            });
        } catch (e) {
            listContainer.innerHTML = `<div class="text-sm text-red-400">Error loading plugins: ${e.message}</div>`;
        }
    }

    // Install
    installBtn.addEventListener('click', async () => {
        const url = gitUrlInput.value.trim();
        if (!url) return;

        installBtn.disabled = true;
        installBtn.textContent = 'Installing...';

        try {
            const result = await plugins.install(url);
            showMessage(result.message, result.success);
            if (result.success) {
                gitUrlInput.value = '';
                await refreshList();
            }
        } catch (e) {
            showMessage('Error: ' + e.message, false);
        }

        installBtn.disabled = false;
        installBtn.textContent = 'Install';
    });

    // Refresh
    refreshBtn.addEventListener('click', refreshList);

    // ── LAN access toggle ───────────────────────────────────────────────
    const network = window.feedBackDesktop?.network;
    const lanToggle = $('pm-lan-toggle');
    const lanStatus = $('pm-lan-status');

    // Escape before interpolating into innerHTML — this renderer runs with
    // webSecurity:false, and extraNote can carry a main-process error string.
    const esc = (s) => String(s).replace(/[&<>"']/g, (c) => (
        { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]
    ));

    function renderLanStatus(enabled, urls, extraNote) {
        if (!lanStatus) return;
        if (!enabled) {
            lanStatus.className = 'mt-3 text-xs text-slate-500';
            lanStatus.textContent = extraNote || 'Only this computer can connect.';
            lanStatus.classList.remove('hidden');
            return;
        }
        const list = (urls && urls.length)
            ? urls.map((u) => `<li><code class="text-emerald-300">${esc(u)}</code></li>`).join('')
            : '<li class="text-slate-400 italic">No network address detected — are you connected to Wi-Fi/Ethernet?</li>';
        lanStatus.className = 'mt-3 text-xs text-slate-300';
        lanStatus.innerHTML = `
            ${extraNote ? `<div class="text-amber-300 mb-1">${esc(extraNote)}</div>` : ''}
            <div class="text-slate-400">Other devices on your network can open:</div>
            <ul class="list-disc list-inside mt-1 space-y-0.5">${list}</ul>
            <div class="text-slate-500 mt-2">Your OS firewall may prompt the first time — allow Slopsmith Desktop so other devices can connect.</div>
        `;
        lanStatus.classList.remove('hidden');
    }

    if (network && lanToggle) {
        network.getLanAccess()
            .then(({ enabled, urls }) => {
                lanToggle.checked = enabled;
                renderLanStatus(enabled, urls);
            })
            .catch(() => { /* leave toggle in default unchecked state */ });

        lanToggle.addEventListener('change', async () => {
            const wanted = lanToggle.checked;
            lanToggle.disabled = true;
            renderLanStatus(wanted, [], 'Restarting server to apply…');
            try {
                const res = await network.setLanAccess(wanted);
                renderLanStatus(res.enabled, res.urls);
            } catch (e) {
                lanToggle.checked = !wanted; // revert on failure
                renderLanStatus(!wanted, [], 'Failed to change network setting: ' + (e?.message || e));
            } finally {
                lanToggle.disabled = false;
            }
        });
    } else if (lanToggle) {
        lanToggle.disabled = true;
    }

    // Initial load
    refreshList();
})();
