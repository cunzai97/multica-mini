(function () {
  "use strict";

  var state = { agents: [], squads: [], issues: [], runs: [], version: "—" };
  var view = "issues";
  var filter = "all";
  var searchQuery = "";
  var activeIssue = null;
  var detailBusy = false;
  var themeMode = document.documentElement.dataset.themeMode || "system";
  var systemTheme = window.matchMedia("(prefers-color-scheme: dark)");
  var statusText = { todo: "Todo", queued: "Queued", in_progress: "In Progress", in_review: "In Review", done: "Done", failed: "Failed", cancelled: "Cancelled" };
  var viewText = {
    issues: { title: "任务", subtitle: "跟踪并协调本机智能体工作", create: "新建任务" },
    agents: { title: "智能体", subtitle: "管理命令、职责和 skill", create: "登记智能体" },
    squads: { title: "小队", subtitle: "由 leader 协调多个智能体", create: "新建小队" },
    data: { title: "数据", subtitle: "备份、恢复和查看本地存储", create: "" }
  };

  function el(id) { return document.getElementById(id); }
  function esc(value) {
    return String(value == null ? "" : value).replace(/[&<>"']/g, function (char) {
      return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[char];
    });
  }
  function two(value) { return Number(value) < 10 ? "0" + value : String(value); }
  function formatTime(value) {
    if (!value) return "—";
    var date = new Date(value);
    if (isNaN(date.getTime())) return value;
    var delta = Date.now() - date.getTime();
    if (delta >= 0 && delta < 60000) return "刚刚";
    if (delta >= 0 && delta < 3600000) return Math.max(1, Math.floor(delta / 60000)) + " 分钟前";
    if (delta >= 0 && delta < 86400000) return Math.floor(delta / 3600000) + " 小时前";
    return two(date.getMonth() + 1) + "/" + two(date.getDate()) + " " + two(date.getHours()) + ":" + two(date.getMinutes());
  }
  function formatFullTime(value) {
    if (!value) return "—";
    var date = new Date(value);
    return isNaN(date.getTime()) ? value : date.toLocaleString("zh-CN", { hour12: false });
  }
  function truncate(value, size) {
    value = String(value || "").replace(/\s+/g, " ");
    return value.length > size ? value.slice(0, size - 1) + "..." : value;
  }
  function byId(kind, id) {
    return (state[kind] || []).find(function (item) { return item.id === id; });
  }
  function squadReadiness(squad) {
    var leader = byId("agents", squad.leader_id);
    if (!leader) return "Leader 配置不存在";
    if (leader.enabled === false) return "Leader 已停用";
    if (!(squad.members || []).length) return "尚无成员";
    for (var i = 0; i < squad.members.length; i += 1) {
      var member = byId("agents", squad.members[i].agent_id);
      if (!member) return "成员 " + squad.members[i].agent_id + " 不存在";
      if (member.enabled === false) return "成员 " + (member.name || member.id) + " 已停用";
    }
    return "";
  }
  function assigneeName(issue) {
    var item = byId(issue.assignee_type === "agent" ? "agents" : "squads", issue.assignee_id);
    return item ? (item.name || item.id) : issue.assignee_id;
  }
  function avatarText(value) {
    var clean = String(value || "M").trim();
    return (clean.charAt(0) || "M").toUpperCase();
  }
  function isIssueRunning(issue) {
    return state.runs.some(function (run) { return run.issue_id === issue.id && run.status === "running"; });
  }

  async function api(path, options) {
    options = options || {};
    var response = await fetch(path, {
      method: options.method || "GET",
      headers: options.body ? { "Content-Type": "application/json" } : {},
      body: options.body ? JSON.stringify(options.body) : undefined,
      cache: "no-store"
    });
    var payload;
    try { payload = await response.json(); } catch (_) { payload = {}; }
    if (!response.ok) throw new Error(payload.error || ("HTTP " + response.status));
    return payload;
  }

  function toast(message, error) {
    var node = document.querySelector(".toast");
    node.textContent = message;
    node.className = "toast" + (error ? " error" : "") + " show";
    clearTimeout(node.timer);
    node.timer = setTimeout(function () { node.classList.remove("show"); }, 3000);
  }

  function resolvedTheme(mode) {
    return mode === "system" ? (systemTheme.matches ? "dark" : "light") : mode;
  }
  function renderThemeMenu() {
    var names = { light: "亮色", dark: "暗色", system: "跟随系统" };
    el("theme-current").textContent = names[themeMode];
    document.querySelectorAll("[data-theme-mode]").forEach(function (button) {
      button.classList.toggle("selected", button.dataset.themeMode === themeMode);
    });
  }
  function applyTheme(mode, announce) {
    themeMode = mode;
    document.documentElement.dataset.themeMode = mode;
    document.documentElement.dataset.theme = resolvedTheme(mode);
    try { localStorage.setItem("multica-mini-theme", mode); } catch (_) {}
    renderThemeMenu();
    if (announce) toast("界面已切换为" + el("theme-current").textContent);
  }
  function closeThemeMenu() {
    el("theme-menu").classList.remove("open");
    el("theme-toggle").setAttribute("aria-expanded", "false");
  }

  async function refresh(silent) {
    try {
      state = await api("/api/state");
      render();
      el("sync-time").textContent = "已同步 · " + new Date().toLocaleTimeString("zh-CN", { hour: "2-digit", minute: "2-digit", hour12: false });
      if (!silent) toast("本地状态已同步");
    } catch (error) {
      el("sync-time").textContent = "连接异常";
      if (!silent) toast(error.message, true);
    }
  }

  function renderSummary() {
    var runningAgents = new Set(state.runs.filter(function (run) { return run.status === "running"; }).map(function (run) { return run.agent_id; }));
    el("nav-issue-count").textContent = state.issues.length;
    el("nav-agent-count").textContent = state.agents.length;
    el("nav-squad-count").textContent = state.squads.length;
    el("working-count").textContent = runningAgents.size;
    el("version").textContent = "Core " + state.version;
    if (el("data-directory")) el("data-directory").textContent = state.data_dir || "—";
  }

  function renderIssueCard(issue) {
    var name = assigneeName(issue);
    var running = isIssueRunning(issue);
    return '<article class="issue-card" data-issue="' + esc(issue.id) + '" tabindex="0">' +
      '<div class="issue-card-top"><span class="issue-id">↗ ' + esc(issue.id.toUpperCase()) + '</span>' +
      (running || issue.status === "queued" || issue.status === "in_progress"
        ? '<span class="run-badge"><i></i>' + esc(issue.status === "queued" ? "排队中" : "运行中") + '</span>'
        : (issue.status === "failed" || issue.status === "cancelled"
          ? '<span class="issue-state issue-state-' + esc(issue.status) + '">' + esc(statusText[issue.status]) + '</span>'
          : '<span class="issue-signal">⌁</span>')) + '</div>' +
      '<h3>' + esc(issue.title) + '</h3><p>' + esc(truncate(issue.description, 82)) + '</p>' +
      '<footer class="issue-card-footer"><span class="assignee-avatar">' + esc(avatarText(name)) + '</span>' +
      '<span class="assignee-name">' + esc(name) + '</span><time class="timestamp">' + esc(formatTime(issue.updated_at)) + '</time></footer>' +
    '</article>';
  }

  function renderIssues() {
    var allIssues = state.issues.slice().sort(function (a, b) { return String(b.id).localeCompare(String(a.id)); });
    var issues = filter === "all" ? allIssues : allIssues.filter(function (issue) { return issue.assignee_type === filter; });
    if (searchQuery) {
      issues = issues.filter(function (issue) {
        return [issue.id, issue.title, issue.description, assigneeName(issue)].join(" ").toLowerCase().includes(searchQuery);
      });
    }
    var columns = [
      { statuses: ["todo", "failed", "cancelled"], status: "todo", label: "Todo / Needs action" },
      { statuses: ["queued", "in_progress"], status: "in_progress", label: "In Progress" },
      { statuses: ["in_review"], status: "in_review", label: "In Review" },
      { statuses: ["done"], status: "done", label: "Done" }
    ];
    el("issue-list").innerHTML = columns.map(function (column) {
      var items = issues.filter(function (issue) { return column.statuses.includes(issue.status); });
      return '<section class="kanban-column" data-status="' + column.status + '">' +
        '<header class="column-head"><i></i><strong>' + column.label + '</strong><span>' + items.length + '</span><button type="button" aria-label="更多">···</button></header>' +
        '<div class="column-cards">' + items.map(renderIssueCard).join("") + '</div></section>';
    }).join("");
    el("issue-count").textContent = issues.length + " 个任务";
    el("issue-empty").classList.toggle("hidden", allIssues.length > 0);
    el("issue-list").classList.toggle("hidden", allIssues.length === 0);
  }

  function renderAgents() {
    el("agent-count").textContent = state.agents.length + " 个智能体";
    el("agent-empty").classList.toggle("hidden", state.agents.length > 0);
    el("agent-list").innerHTML = state.agents.map(function (agent, index) {
      var skills = (agent.skills || []).map(function (skill) {
        var parts = skill.split("/");
        return parts[parts.length - 1] === "SKILL.md" ? parts[parts.length - 2] : parts[parts.length - 1];
      });
      return '<article class="node-card agent-card" data-agent="' + esc(agent.id) + '" tabindex="0" aria-label="查看并编辑智能体 ' + esc(agent.name || agent.id) + '"><span class="node-index">AGENT ' + two(index + 1) + '</span>' +
        '<h3>' + esc(agent.name || agent.id) + '</h3><span class="node-id">@' + esc(agent.id) + '</span>' +
        (agent.enabled === false ? '<span class="config-warning">已停用 · 不接收任务</span>' : '') +
        '<p class="node-role">' + esc(agent.role || "未设置职责") + '</p>' +
        '<div class="command-line">$ ' + esc((agent.command || []).join(" ")) + '</div>' +
        '<div class="tag-row">' + (skills.length ? skills.map(function (skill) { return '<span class="tag">' + esc(skill) + '</span>'; }).join("") : '<span class="tag">未配置 skill</span>') + '</div>' +
        '<footer class="node-card-action"><span>' + esc(agent.updated_at ? "更新于 " + formatTime(agent.updated_at) : "创建于 " + formatTime(agent.created_at)) + '</span><b>查看与编辑&nbsp; →</b></footer></article>';
    }).join("");
  }

  function renderSquads() {
    el("squad-count").textContent = state.squads.length + " 个小队";
    el("squad-empty").classList.toggle("hidden", state.squads.length > 0);
    el("squad-list").innerHTML = state.squads.map(function (squad, index) {
      var leader = byId("agents", squad.leader_id);
      var readinessError = squadReadiness(squad);
      var roster = '<div class="roster-line"><b>LEADER</b><span>' + esc(leader ? leader.name : squad.leader_id) + ' <small>@' + esc(squad.leader_id) + '</small></span></div>';
      roster += (squad.members || []).map(function (member) {
        var agent = byId("agents", member.agent_id);
        return '<div class="roster-line worker"><b>MEMBER</b><span>' + esc(agent ? agent.name : member.agent_id) + ' · ' + esc(member.role || "未设置职责") + '</span></div>';
      }).join("");
      return '<article class="node-card squad-card" data-squad="' + esc(squad.id) + '" tabindex="0" aria-label="查看并编辑小队 ' + esc(squad.name || squad.id) + '"><span class="node-index">SQUAD ' + two(index + 1) + '</span>' +
        '<h3>' + esc(squad.name || squad.id) + '</h3><span class="node-id">#' + esc(squad.id) + '</span>' +
        (readinessError ? '<span class="config-warning">' + esc(readinessError) + ' · 不可运行</span>' : '') +
        '<div class="squad-roster">' + roster + '</div>' +
        '<footer class="node-card-action"><span>' + esc(squad.updated_at ? "更新于 " + formatTime(squad.updated_at) : "创建于 " + formatTime(squad.created_at)) + '</span><b>查看与编辑&nbsp; →</b></footer></article>';
    }).join("");
  }

  function render() {
    renderSummary();
    renderIssues();
    renderAgents();
    renderSquads();
  }

  function switchView(next) {
    view = next;
    document.querySelectorAll(".nav-item").forEach(function (button) { button.classList.toggle("active", button.dataset.view === next); });
    document.querySelectorAll(".view").forEach(function (section) { section.classList.toggle("active", section.id === "view-" + next); });
    el("view-title").textContent = viewText[next].title;
    el("tab-title").textContent = viewText[next].title;
    el("view-subtitle").textContent = viewText[next].subtitle;
    el("create-button").innerHTML = '<span>＋</span> ' + viewText[next].create;
    el("create-button").classList.toggle("hidden", next === "data");
  }

  function addOverlay(className) {
    var node = document.createElement("div");
    node.className = className;
    document.body.appendChild(node);
    return node;
  }
  function closeModal() {
    var layer = document.querySelector(".modal-layer");
    if (layer) layer.remove();
  }
  function optionsForAssignees(selected) {
    var agents = state.agents.filter(function (agent) {
      return agent.enabled !== false || "agent:" + agent.id === selected;
    }).map(function (agent) {
      var value = "agent:" + agent.id;
      return '<option value="' + esc(value) + '"' + (value === selected ? " selected" : "") + '>智能体 · ' + esc(agent.name || agent.id) + (agent.enabled === false ? '（已停用）' : '') + '</option>';
    });
    var squads = state.squads.filter(function (squad) {
      return !squadReadiness(squad) || "squad:" + squad.id === selected;
    }).map(function (squad) {
      var value = "squad:" + squad.id;
      var warning = squadReadiness(squad);
      return '<option value="' + esc(value) + '"' + (value === selected ? " selected" : "") + '>小队 · ' + esc(squad.name || squad.id) + (warning ? '（' + esc(warning) + '）' : '') + '</option>';
    });
    return agents.concat(squads).join("");
  }
  function optionsForAgents(selected) {
    return state.agents.map(function (agent) {
      return '<option value="' + esc(agent.id) + '"' + (agent.id === selected ? " selected" : "") + '>' + esc(agent.name || agent.id) + ' · @' + esc(agent.id) + '</option>';
    }).join("");
  }

  function openAgentEditor(id) {
    var agent = byId("agents", id);
    if (!agent) return toast("找不到这个智能体", true);
    var command = agent.command || [];
    var layer = addOverlay("modal-layer open");
    layer.innerHTML = '<div class="modal agent-editor"><button class="modal-close" aria-label="关闭">×</button>' +
      '<p class="eyebrow">AGENT PROFILE</p><div class="editor-heading"><div><h2>' + esc(agent.name || agent.id) + '</h2><p>创建于 ' + esc(formatFullTime(agent.created_at)) + '</p></div><span class="profile-avatar">' + esc(avatarText(agent.name || agent.id)) + '</span></div>' +
      '<form id="edit-agent-form" class="form-grid" data-agent="' + esc(agent.id) + '">' +
      '<div class="field"><label>ID</label><input name="id" value="' + esc(agent.id) + '" readonly><span class="field-note">ID 被任务和小队引用，创建后不可修改。</span></div>' +
      '<div class="field"><label>显示名称</label><input name="name" value="' + esc(agent.name || "") + '" placeholder="RTL Reviewer"></div>' +
      '<div class="field full"><label>职责</label><input name="role" value="' + esc(agent.role || "") + '" placeholder="说明这个智能体负责什么"></div>' +
      '<div class="field full"><label>可执行文件 *</label><input name="exec" required value="' + esc(command[0] || "") + '" placeholder="/opt/agent/bin/agent-cli"></div>' +
      '<div class="field full"><label>参数 · 每行一个</label><textarea name="args" placeholder="--print&#10;{prompt}">' + esc(command.slice(1).join("\n")) + '</textarea><span class="field-note">支持 {prompt}、{cwd}、{issue_id}、{agent_id}。</span></div>' +
      '<div class="field full"><label>skill 路径 · 每行一个</label><textarea name="skills" placeholder="/opt/skills/rtl-review/SKILL.md">' + esc((agent.skills || []).join("\n")) + '</textarea></div>' +
      '<label class="check-field full"><input name="enabled" type="checkbox"' + (agent.enabled === false ? "" : " checked") + '><span><b>启用这个智能体</b><small>停用后保留配置和历史记录，但不能接收新任务。</small></span></label>' +
      '<div class="field full"><div class="test-result" id="agent-test-result"></div><p class="form-error"></p><div class="form-actions"><button class="danger-button" id="delete-agent" type="button">删除智能体</button><button class="soft-button" id="test-agent" type="button">测试连接</button><button class="form-submit">保存更改</button></div></div></form></div>';
    layer.querySelector(".modal-close").addEventListener("click", closeModal);
    layer.addEventListener("click", function (event) { if (event.target === layer) closeModal(); });
    layer.querySelector("#edit-agent-form").addEventListener("submit", submitAgentEdit);
    layer.querySelector("#test-agent").addEventListener("click", function () { testAgent(id, layer); });
    layer.querySelector("#delete-agent").addEventListener("click", function () { deleteAgent(id, agent.name || id); });
    layer.querySelector('input[name="name"]').focus();
  }

  function squadMemberEditorRow(member) {
    member = member || {};
    return '<div class="squad-member-edit">' +
      '<select name="member_agent" aria-label="小队成员">' + optionsForAgents(member.agent_id) + '</select>' +
      '<input name="member_role" value="' + esc(member.role || "") + '" placeholder="成员职责">' +
      '<button class="remove-member" type="button" aria-label="移除成员">×</button></div>';
  }

  function openSquadEditor(id) {
    var squad = byId("squads", id);
    if (!squad) return toast("找不到这个小队", true);
    var leader = byId("agents", squad.leader_id);
    var members = (squad.members || []).map(squadMemberEditorRow).join("");
    var layer = addOverlay("modal-layer open");
    layer.innerHTML = '<div class="modal squad-editor"><button class="modal-close" aria-label="关闭">×</button>' +
      '<p class="eyebrow">SQUAD PROFILE</p><div class="editor-heading"><div><h2>' + esc(squad.name || squad.id) + '</h2><p>创建于 ' + esc(formatFullTime(squad.created_at)) + '</p></div><span class="profile-avatar">' + esc(avatarText(squad.name || squad.id)) + '</span></div>' +
      '<form id="edit-squad-form" class="form-grid" data-squad="' + esc(squad.id) + '">' +
      '<div class="field"><label>ID</label><input name="id" value="' + esc(squad.id) + '" readonly><span class="field-note">ID 被任务引用，创建后不可修改。</span></div>' +
      '<div class="field"><label>小队名称</label><input name="name" value="' + esc(squad.name || "") + '" placeholder="Verification Team"></div>' +
      '<div class="field full"><label>Leader *</label><select name="leader_id" required>' + optionsForAgents(squad.leader_id) + '</select><span class="field-note">当前 Leader：' + esc(leader ? leader.name : squad.leader_id) + '</span></div>' +
      '<div class="field full squad-editor-members"><div class="member-editor-heading"><label>成员与职责</label><button id="add-squad-member" class="mini-button" type="button">＋ 添加成员</button></div>' +
      '<div id="squad-members-editor">' + (members || '<p class="member-empty">尚未添加成员</p>') + '</div></div>' +
      '<div class="field full"><span class="field-note">Leader 不能同时作为成员，同一成员不能重复添加。</span><p class="form-error"></p><div class="form-actions"><button class="danger-button" id="delete-squad" type="button">删除小队</button><span></span><button class="form-submit">保存小队</button></div></div></form></div>';
    layer.querySelector(".modal-close").addEventListener("click", closeModal);
    layer.addEventListener("click", function (event) { if (event.target === layer) closeModal(); });
    layer.querySelector("#edit-squad-form").addEventListener("submit", submitSquadEdit);
    layer.querySelector("#delete-squad").addEventListener("click", function () { deleteSquad(id, squad.name || id); });
    layer.querySelector("#add-squad-member").addEventListener("click", function () {
      var form = layer.querySelector("#edit-squad-form");
      var selected = Array.from(form.querySelectorAll('[name="member_agent"]')).map(function (node) { return node.value; });
      selected.push(form.leader_id.value);
      var candidate = state.agents.find(function (agent) { return !selected.includes(agent.id); });
      if (!candidate) return toast("没有可添加的智能体", true);
      var container = layer.querySelector("#squad-members-editor");
      var empty = container.querySelector(".member-empty");
      if (empty) empty.remove();
      container.insertAdjacentHTML("beforeend", squadMemberEditorRow({ agent_id: candidate.id, role: candidate.role || "" }));
    });
    layer.querySelector("#squad-members-editor").addEventListener("click", function (event) {
      var remove = event.target.closest(".remove-member");
      if (!remove) return;
      remove.closest(".squad-member-edit").remove();
      var container = layer.querySelector("#squad-members-editor");
      if (!container.querySelector(".squad-member-edit")) container.innerHTML = '<p class="member-empty">尚未添加成员</p>';
    });
    layer.querySelector('input[name="name"]').focus();
  }

  function openIssueEditor(issue) {
    var layer = addOverlay("modal-layer open");
    layer.innerHTML = '<div class="modal"><button class="modal-close" aria-label="关闭">×</button>' +
      '<p class="eyebrow">EDIT ISSUE</p><h2>编辑任务</h2><form id="edit-issue-form" class="form-grid" data-issue="' + esc(issue.id) + '">' +
      '<div class="field full"><label>标题 *</label><input name="title" required maxlength="160" value="' + esc(issue.title) + '"></div>' +
      '<div class="field full"><label>任务描述 *</label><textarea name="description" required>' + esc(issue.description) + '</textarea></div>' +
      '<div class="field"><label>负责人 *</label><select name="assignee" required>' + optionsForAssignees(issue.assignee_type + ":" + issue.assignee_id) + '</select></div>' +
      '<div class="field"><label>工作目录 *</label><input name="cwd" required value="' + esc(issue.cwd) + '"></div>' +
      '<div class="field"><label>完成策略</label><select name="review_policy"><option value="auto"' + (issue.review_policy !== "manual" ? " selected" : "") + '>自动完成</option><option value="manual"' + (issue.review_policy === "manual" ? " selected" : "") + '>需要人工验收</option></select></div>' +
      '<div class="field"><label>单次超时 · 秒</label><input name="timeout_seconds" type="number" min="1" max="86400" value="' + esc(issue.timeout_seconds || 900) + '"></div>' +
      '<div class="field"><label>失败重试次数</label><input name="max_retries" type="number" min="0" max="10" value="' + esc(issue.max_retries || 0) + '"></div>' +
      '<div class="field full"><p class="form-error"></p><button class="form-submit">保存任务</button></div></form></div>';
    layer.querySelector(".modal-close").addEventListener("click", closeModal);
    layer.addEventListener("click", function (event) { if (event.target === layer) closeModal(); });
    layer.querySelector("#edit-issue-form").addEventListener("submit", submitIssueEdit);
    layer.querySelector('input[name="title"]').focus();
  }

  function openCreateModal() {
    if (view === "issues" && !optionsForAssignees()) {
      switchView("agents");
      toast("请先登记并启用一个智能体，或配置一个可运行小队", true);
      return;
    }
    if (view === "squads" && state.agents.length < 2) {
      switchView("agents");
      toast("创建小队至少需要两个智能体", true);
      return;
    }
    var layer = addOverlay("modal-layer open");
    var content = "";
    if (view === "issues") {
      content = '<p class="eyebrow">NEW ISSUE</p><h2>新建任务</h2><form id="create-form" class="form-grid">' +
        '<div class="field full"><label>标题 *</label><input name="title" required maxlength="160" placeholder="例如：检查 RTL lint 并修复阻塞项"></div>' +
        '<div class="field full"><label>任务描述 *</label><textarea name="description" required placeholder="说明目标、边界和验收条件"></textarea></div>' +
        '<div class="field"><label>负责人 *</label><select name="assignee" required>' + optionsForAssignees() + '</select></div>' +
        '<div class="field"><label>工作目录 *</label><input name="cwd" required value="." placeholder="/path/to/project"></div>' +
        '<div class="field"><label>完成策略</label><select name="review_policy"><option value="auto">自动完成</option><option value="manual">需要人工验收</option></select></div>' +
        '<div class="field"><label>单次超时 · 秒</label><input name="timeout_seconds" type="number" min="1" max="86400" value="900"></div>' +
        '<div class="field"><label>失败重试次数</label><input name="max_retries" type="number" min="0" max="10" value="0"></div>' +
        '<div class="field full"><span class="field-note">目录必须已存在于运行 multica-core 的机器上。</span><p class="form-error"></p><button class="form-submit">创建任务</button></div></form>';
    } else if (view === "agents") {
      content = '<p class="eyebrow">NEW AGENT</p><h2>登记智能体</h2><form id="create-form" class="form-grid">' +
        '<div class="field"><label>ID *</label><input name="id" required pattern="[A-Za-z0-9._-]+" placeholder="rtl-reviewer"></div>' +
        '<div class="field"><label>显示名称</label><input name="name" placeholder="RTL Reviewer"></div>' +
        '<div class="field full"><label>职责</label><input name="role" placeholder="检查 RTL、定位 lint 问题并验证修复"></div>' +
        '<div class="field full"><label>可执行文件 *</label><input name="exec" required placeholder="/opt/agent/bin/agent-cli"></div>' +
        '<div class="field full"><label>参数 · 每行一个</label><textarea name="args" placeholder="run&#10;--prompt&#10;{prompt}">{prompt}</textarea><span class="field-note">支持 {prompt}、{cwd}、{issue_id}、{agent_id}。</span></div>' +
        '<div class="field full"><label>skill 路径 · 每行一个</label><textarea name="skills" placeholder="/opt/skills/rtl-review/SKILL.md"></textarea><p class="form-error"></p><button class="form-submit">登记智能体</button></div></form>';
    } else {
      var agentOptions = optionsForAgents();
      content = '<p class="eyebrow">NEW SQUAD</p><h2>新建小队</h2><form id="create-form" class="form-grid">' +
        '<div class="field"><label>ID *</label><input name="id" required pattern="[A-Za-z0-9._-]+" placeholder="verification-team"></div>' +
        '<div class="field"><label>小队名称</label><input name="name" placeholder="Verification Team"></div>' +
        '<div class="field"><label>Leader *</label><select name="leader_id" required>' + agentOptions + '</select></div>' +
        '<div class="field"><label>首位成员</label><select name="worker_id"><option value="">稍后添加</option>' + agentOptions + '</select></div>' +
        '<div class="field full"><label>成员职责</label><input name="worker_role" placeholder="实现与验证"></div>' +
        '<div class="field full"><span class="field-note">Leader 与成员必须是不同的智能体。</span><p class="form-error"></p><button class="form-submit">创建小队</button></div></form>';
    }
    layer.innerHTML = '<div class="modal"><button class="modal-close" aria-label="关闭">×</button>' + content + '</div>';
    layer.querySelector(".modal-close").addEventListener("click", closeModal);
    layer.addEventListener("click", function (event) { if (event.target === layer) closeModal(); });
    layer.querySelector("#create-form").addEventListener("submit", submitCreate);
    var first = layer.querySelector("input");
    if (first) first.focus();
  }

  function lines(value) {
    return String(value || "").split(/\r?\n/).map(function (line) { return line.trim(); }).filter(Boolean);
  }
  async function submitCreate(event) {
    event.preventDefault();
    var form = event.currentTarget;
    var values = new FormData(form);
    var button = form.querySelector(".form-submit");
    var errorNode = form.querySelector(".form-error");
    button.disabled = true;
    errorNode.textContent = "";
    try {
      var body;
      var path;
      if (view === "issues") {
        var parts = values.get("assignee").split(":");
        body = { title: values.get("title"), description: values.get("description"), assignee_type: parts[0], assignee_id: parts.slice(1).join(":"), cwd: values.get("cwd"), review_policy: values.get("review_policy"), timeout_seconds: Number(values.get("timeout_seconds")), max_retries: Number(values.get("max_retries")) };
        path = "/api/issues";
      } else if (view === "agents") {
        body = { id: values.get("id"), name: values.get("name"), role: values.get("role"), command: [values.get("exec")].concat(lines(values.get("args"))), skills: lines(values.get("skills")) };
        path = "/api/agents";
      } else {
        var worker = values.get("worker_id");
        if (worker && worker === values.get("leader_id")) throw new Error("Leader 与成员不能相同");
        body = { id: values.get("id"), name: values.get("name"), leader_id: values.get("leader_id"), members: worker ? [{ agent_id: worker, role: values.get("worker_role") }] : [] };
        path = "/api/squads";
      }
      await api(path, { method: "POST", body: body });
      closeModal();
      await refresh(true);
      toast("记录已保存");
    } catch (error) {
      errorNode.textContent = error.message;
      button.disabled = false;
    }
  }

  async function submitAgentEdit(event) {
    event.preventDefault();
    var form = event.currentTarget;
    var values = new FormData(form);
    var button = form.querySelector(".form-submit");
    var errorNode = form.querySelector(".form-error");
    button.disabled = true;
    errorNode.textContent = "";
    try {
      await api("/api/agents/" + encodeURIComponent(form.dataset.agent), {
        method: "PUT",
        body: {
          name: values.get("name"),
          role: values.get("role"),
          command: [values.get("exec")].concat(lines(values.get("args"))),
          skills: lines(values.get("skills")),
          enabled: values.get("enabled") === "on"
        }
      });
      closeModal();
      await refresh(true);
      toast("智能体配置已更新");
    } catch (error) {
      errorNode.textContent = error.message;
      button.disabled = false;
    }
  }

  async function testAgent(id, layer) {
    var button = layer.querySelector("#test-agent");
    var resultNode = layer.querySelector("#agent-test-result");
    button.disabled = true;
    resultNode.className = "test-result pending";
    resultNode.textContent = "正在调用智能体执行最小测试…";
    try {
      var result = await api("/api/agents/" + encodeURIComponent(id) + "/test", { method: "POST", body: { cwd: "." } });
      resultNode.className = "test-result " + (result.ok ? "success" : "error");
      resultNode.textContent = "退出码 " + result.exit_code + "\n" + (result.output || "没有输出");
    } catch (error) {
      resultNode.className = "test-result error";
      resultNode.textContent = error.message;
    }
    button.disabled = false;
  }

  async function deleteAgent(id, name) {
    if (!window.confirm("删除智能体“" + name + "”？如果它仍被小队或任务引用，系统会拒绝删除。")) return;
    try {
      await api("/api/agents/" + encodeURIComponent(id), { method: "DELETE" });
      closeModal();
      await refresh(true);
      toast("智能体已删除");
    } catch (error) { toast(error.message, true); }
  }

  async function submitSquadEdit(event) {
    event.preventDefault();
    var form = event.currentTarget;
    var values = new FormData(form);
    var button = form.querySelector(".form-submit");
    var errorNode = form.querySelector(".form-error");
    var memberRows = Array.from(form.querySelectorAll(".squad-member-edit"));
    var members = memberRows.map(function (row) {
      return { agent_id: row.querySelector('[name="member_agent"]').value, role: row.querySelector('[name="member_role"]').value };
    });
    var memberIds = members.map(function (member) { return member.agent_id; });
    button.disabled = true;
    errorNode.textContent = "";
    try {
      if (memberIds.includes(values.get("leader_id"))) throw new Error("Leader 不能同时作为成员");
      if (new Set(memberIds).size !== memberIds.length) throw new Error("同一成员不能重复添加");
      await api("/api/squads/" + encodeURIComponent(form.dataset.squad), {
        method: "PUT",
        body: { name: values.get("name"), leader_id: values.get("leader_id"), members: members }
      });
      closeModal();
      await refresh(true);
      toast("小队配置已更新");
    } catch (error) {
      errorNode.textContent = error.message;
      button.disabled = false;
    }
  }

  async function deleteSquad(id, name) {
    if (!window.confirm("删除小队“" + name + "”？如果仍有任务引用它，系统会拒绝删除。")) return;
    try {
      await api("/api/squads/" + encodeURIComponent(id), { method: "DELETE" });
      closeModal();
      await refresh(true);
      toast("小队已删除");
    } catch (error) { toast(error.message, true); }
  }

  async function submitIssueEdit(event) {
    event.preventDefault();
    var form = event.currentTarget;
    var values = new FormData(form);
    var button = form.querySelector(".form-submit");
    var errorNode = form.querySelector(".form-error");
    var assignee = values.get("assignee").split(":");
    button.disabled = true;
    errorNode.textContent = "";
    try {
      await api("/api/issues/" + encodeURIComponent(form.dataset.issue), {
        method: "PUT",
        body: {
          title: values.get("title"), description: values.get("description"),
          assignee_type: assignee[0], assignee_id: assignee.slice(1).join(":"), cwd: values.get("cwd"),
          review_policy: values.get("review_policy"), timeout_seconds: Number(values.get("timeout_seconds")),
          max_retries: Number(values.get("max_retries"))
        }
      });
      var id = form.dataset.issue;
      closeModal();
      await refresh(true);
      await openIssue(id, true);
      toast("任务已更新");
    } catch (error) {
      errorNode.textContent = error.message;
      button.disabled = false;
    }
  }

  async function duplicateIssue(issue) {
    try {
      var copy = await api("/api/issues", { method: "POST", body: {
        title: issue.title + " · 副本", description: issue.description,
        assignee_type: issue.assignee_type, assignee_id: issue.assignee_id, cwd: issue.cwd,
        review_policy: issue.review_policy || "auto", timeout_seconds: issue.timeout_seconds || 900,
        max_retries: issue.max_retries || 0
      } });
      closeDetail();
      await refresh(true);
      await openIssue(copy.id);
      toast("任务副本已创建");
    } catch (error) { toast(error.message, true); }
  }

  async function deleteIssue(issue) {
    if (!window.confirm("删除任务“" + issue.title + "”？相关评论和运行记录也会删除。")) return;
    try {
      await api("/api/issues/" + encodeURIComponent(issue.id), { method: "DELETE" });
      closeDetail();
      await refresh(true);
      toast("任务已删除");
    } catch (error) { toast(error.message, true); }
  }

  async function openIssue(id, silent) {
    activeIssue = id;
    if (!silent) detailBusy = false;
    try {
      var issue = await api("/api/issues/" + encodeURIComponent(id));
      renderDetail(issue);
    } catch (error) {
      if (!silent) toast(error.message, true);
    }
  }
  function runDuration(run) {
    if (!run.started_at || !run.finished_at) return "运行中";
    var seconds = Math.max(0, Math.round((new Date(run.finished_at) - new Date(run.started_at)) / 1000));
    return seconds < 60 ? seconds + " 秒" : Math.floor(seconds / 60) + " 分 " + (seconds % 60) + " 秒";
  }
  function renderRun(run) {
    var statusClass = run.status === "completed" ? "done" : (run.status === "running" ? "in_progress" : "failed");
    return '<details class="run-detail"><summary class="run-row"><span>' + esc(run.id) + '</span><span>@' + esc(run.agent_id) + ' · ' + esc(run.trigger) + ' · ' + esc(runDuration(run)) + '</span><span class="status-' + statusClass + '">' + esc(String(run.status || "unknown").toUpperCase()) + '</span></summary>' +
      '<div class="run-body"><div><small>退出码</small><b>' + esc(run.exit_code == null ? "—" : run.exit_code) + '</b></div><div><small>失败原因</small><b>' + esc(run.failure_reason || "—") + '</b></div>' +
      '<h4>输出</h4><pre>' + esc(run.output || "没有输出") + '</pre>' +
      '<details class="prompt-snapshot"><summary>查看本次提示快照</summary><pre>' + esc(run.prompt || "旧记录没有提示快照") + '</pre></details></div></details>';
  }
  function renderDetail(issue) {
    if (activeIssue !== issue.id) return;
    var panel = el("detail-panel");
    var comments = issue.comments || [];
    var runs = issue.runs || [];
    var busy = issue.status === "queued" || issue.status === "in_progress" || detailBusy;
    panel.innerHTML = '<button class="detail-close" aria-label="关闭">×</button>' +
      '<div class="detail-title"><p class="eyebrow">' + esc(issue.id.toUpperCase()) + '</p><h2>' + esc(issue.title) + '</h2><p>' + esc(issue.description) + '</p></div>' +
      '<div class="detail-meta"><div><small>负责人</small><span>' + esc(assigneeName(issue)) + '</span></div><div><small>状态</small><span class="status status-' + esc(issue.status) + '">' + esc(statusText[issue.status] || issue.status) + '</span></div><div><small>工作目录</small><span>' + esc(issue.cwd) + '</span></div><div><small>完成策略</small><span>' + esc(issue.review_policy === "manual" ? "人工验收" : "自动完成") + '</span></div><div><small>超时</small><span>' + esc(issue.timeout_seconds || 900) + ' 秒</span></div><div><small>失败重试</small><span>' + esc(issue.max_retries || 0) + ' 次</span></div></div>' +
      (issue.last_error ? '<div class="error-banner"><b>最近错误</b><span>' + esc(issue.last_error) + '</span></div>' : '') +
      '<div class="detail-actions"><button id="run-issue" class="' + (busy ? "running" : "") + '"' + (busy ? " disabled" : "") + '>▶ ' + (busy ? "任务运行中" : (issue.status === "failed" || issue.status === "cancelled" ? "重新运行" : "运行任务")) + '</button>' +
      (busy ? '<button id="cancel-issue" class="cancel-button">■ 取消</button>' : '') +
      '<button id="edit-issue" class="secondary-action">编辑</button><button id="copy-issue" class="secondary-action">复制</button><button id="delete-issue" class="danger-action">删除</button><select id="issue-status" aria-label="修改任务状态">' +
      Object.keys(statusText).map(function (status) { return '<option value="' + status + '"' + (status === issue.status ? " selected" : "") + '>' + esc(statusText[status]) + '</option>'; }).join("") + '</select></div>' +
      '<h3 class="subhead">评论时间线 · ' + comments.length + '</h3><div class="timeline">' +
      (comments.length ? comments.map(function (comment) { return '<article class="timeline-entry"><header><span>' + esc(comment.author_name || comment.author_id) + '</span><time>' + esc(formatTime(comment.created_at)) + '</time></header><pre>' + esc(comment.content) + '</pre></article>'; }).join("") : '<p class="node-role">尚无智能体回复。</p>') +
      '</div><form class="comment-form" id="comment-form"><textarea name="content" required placeholder="补充要求、反馈或返工说明…"></textarea><button type="submit">添加评论</button></form><h3 class="subhead">运行记录 · ' + runs.length + '</h3><div>' +
      (runs.length ? runs.map(renderRun).join("") : '<p class="node-role">尚无运行记录。</p>') + '</div>';
    panel.classList.add("open");
    panel.setAttribute("aria-hidden", "false");
    var scrim = document.querySelector(".scrim");
    scrim.classList.add("open");
    panel.querySelector(".detail-close").addEventListener("click", closeDetail);
    scrim.onclick = closeDetail;
    panel.querySelector("#run-issue").addEventListener("click", runActiveIssue);
    var cancelButton = panel.querySelector("#cancel-issue");
    if (cancelButton) cancelButton.addEventListener("click", cancelActiveIssue);
    panel.querySelector("#edit-issue").addEventListener("click", function () { openIssueEditor(issue); });
    panel.querySelector("#copy-issue").addEventListener("click", function () { duplicateIssue(issue); });
    panel.querySelector("#delete-issue").addEventListener("click", function () { deleteIssue(issue); });
    panel.querySelector("#comment-form").addEventListener("submit", addIssueComment);
    panel.querySelector("#issue-status").addEventListener("change", changeIssueStatus);
  }
  function closeDetail() {
    activeIssue = null;
    detailBusy = false;
    el("detail-panel").classList.remove("open");
    el("detail-panel").setAttribute("aria-hidden", "true");
    document.querySelector(".scrim").classList.remove("open");
  }
  async function runActiveIssue() {
    if (!activeIssue || detailBusy) return;
    detailBusy = true;
    await openIssue(activeIssue, true);
    var id = activeIssue;
    try {
      await api("/api/issues/" + encodeURIComponent(id) + "/run", { method: "POST", body: { max_runs: 16 } });
      toast("任务已进入本地运行队列");
    } catch (error) { toast(error.message, true); }
    detailBusy = false;
    await refresh(true);
    if (activeIssue === id) await openIssue(id, true);
  }
  async function cancelActiveIssue() {
    if (!activeIssue) return;
    var id = activeIssue;
    try {
      await api("/api/issues/" + encodeURIComponent(id) + "/cancel", { method: "POST" });
      toast("已请求取消，正在清理智能体进程");
      await openIssue(id, true);
    } catch (error) { toast(error.message, true); }
  }
  async function changeIssueStatus(event) {
    if (!activeIssue) return;
    var id = activeIssue;
    try {
      await api("/api/issues/" + encodeURIComponent(id) + "/status", { method: "POST", body: { status: event.target.value } });
      await refresh(true);
      await openIssue(id, true);
      toast("任务状态已更新");
    } catch (error) { toast(error.message, true); }
  }
  async function addIssueComment(event) {
    event.preventDefault();
    if (!activeIssue) return;
    var form = event.currentTarget;
    var content = form.content.value.trim();
    if (!content) return;
    try {
      await api("/api/issues/" + encodeURIComponent(activeIssue) + "/comments", { method: "POST", body: { content: content } });
      var id = activeIssue;
      await refresh(true);
      await openIssue(id, true);
      toast("评论已添加");
    } catch (error) { toast(error.message, true); }
  }

  async function importData() {
    var file = el("import-file").files[0];
    var errorNode = el("import-error");
    errorNode.textContent = "";
    if (!file) {
      errorNode.textContent = "请先选择一个 JSON 备份文件。";
      return;
    }
    if (!window.confirm("导入会替换当前工作区数据。服务器会先自动备份现有数据，是否继续？")) return;
    try {
      var bundle = JSON.parse(await file.text());
      await api("/api/import", { method: "POST", body: bundle });
      await refresh(true);
      toast("数据已导入，原数据已自动备份");
    } catch (error) { errorNode.textContent = error.message; }
  }

  document.querySelectorAll(".nav-item").forEach(function (button) {
    button.addEventListener("click", function () { switchView(button.dataset.view); });
  });
  document.querySelectorAll("#issue-filters button").forEach(function (button) {
    button.addEventListener("click", function () {
      filter = button.dataset.filter;
      document.querySelectorAll("#issue-filters button").forEach(function (item) { item.classList.toggle("active", item === button); });
      renderIssues();
    });
  });
  el("refresh").addEventListener("click", function () { refresh(false); });
  el("create-button").addEventListener("click", openCreateModal);
  el("sidebar-create").addEventListener("click", function () { switchView("issues"); openCreateModal(); });
  el("sidebar-search").addEventListener("click", function () { switchView("issues"); el("issue-search").focus(); });
  el("issue-search").addEventListener("input", function (event) {
    searchQuery = event.target.value.trim().toLowerCase();
    renderIssues();
  });
  el("issue-list").addEventListener("click", function (event) {
    var card = event.target.closest("[data-issue]");
    if (card) openIssue(card.dataset.issue);
  });
  el("issue-list").addEventListener("keydown", function (event) {
    var card = event.target.closest("[data-issue]");
    if (card && (event.key === "Enter" || event.key === " ")) {
      event.preventDefault();
      openIssue(card.dataset.issue);
    }
  });
  el("agent-list").addEventListener("click", function (event) {
    var card = event.target.closest("[data-agent]");
    if (card) openAgentEditor(card.dataset.agent);
  });
  el("agent-list").addEventListener("keydown", function (event) {
    var card = event.target.closest("[data-agent]");
    if (card && (event.key === "Enter" || event.key === " ")) {
      event.preventDefault();
      openAgentEditor(card.dataset.agent);
    }
  });
  el("squad-list").addEventListener("click", function (event) {
    var card = event.target.closest("[data-squad]");
    if (card) openSquadEditor(card.dataset.squad);
  });
  el("squad-list").addEventListener("keydown", function (event) {
    var card = event.target.closest("[data-squad]");
    if (card && (event.key === "Enter" || event.key === " ")) {
      event.preventDefault();
      openSquadEditor(card.dataset.squad);
    }
  });
  el("export-data").addEventListener("click", function () {
    var link = document.createElement("a");
    link.href = "/api/export";
    link.download = "multica-mini-backup.json";
    link.click();
  });
  el("import-data").addEventListener("click", importData);
  el("theme-toggle").addEventListener("click", function (event) {
    event.stopPropagation();
    var open = !el("theme-menu").classList.contains("open");
    el("theme-menu").classList.toggle("open", open);
    el("theme-toggle").setAttribute("aria-expanded", String(open));
  });
  document.querySelectorAll("[data-theme-mode]").forEach(function (button) {
    button.addEventListener("click", function () {
      applyTheme(button.dataset.themeMode, true);
      closeThemeMenu();
    });
  });
  document.addEventListener("click", closeThemeMenu);
  document.addEventListener("keydown", function (event) {
    if (event.key === "Escape") {
      closeThemeMenu();
      closeModal();
      closeDetail();
    }
  });
  var onSystemThemeChange = function () {
    if (themeMode === "system") document.documentElement.dataset.theme = resolvedTheme("system");
  };
  if (systemTheme.addEventListener) systemTheme.addEventListener("change", onSystemThemeChange);
  else if (systemTheme.addListener) systemTheme.addListener(onSystemThemeChange);

  applyTheme(themeMode, false);
  refresh(true);
  setInterval(function () {
    refresh(true);
    if (activeIssue && !detailBusy) openIssue(activeIssue, true);
  }, 5000);
})();
