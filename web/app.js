(function () {
  "use strict";

  var state = { agents: [], squads: [], issues: [], runs: [], version: "—" };
  var view = "issues";
  var filter = "all";
  var activeIssue = null;
  var detailBusy = false;
  var statusText = { todo: "待处理", in_progress: "运行中", in_review: "待复核", done: "已完成" };
  var titleText = { issues: "任务队列", agents: "智能体节点", squads: "小队" };

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
    return two(date.getMonth() + 1) + "/" + two(date.getDate()) + " " + two(date.getHours()) + ":" + two(date.getMinutes());
  }
  function truncate(value, size) {
    value = String(value || "").replace(/\s+/g, " ");
    return value.length > size ? value.slice(0, size - 1) + "…" : value;
  }
  function byId(kind, id) {
    return (state[kind] || []).find(function (item) { return item.id === id; });
  }
  function assigneeName(issue) {
    var item = byId(issue.assignee_type === "agent" ? "agents" : "squads", issue.assignee_id);
    return item ? (item.name || item.id) : issue.assignee_id;
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
    if (!node) {
      node = document.createElement("div");
      node.className = "toast";
      document.body.appendChild(node);
    }
    node.textContent = message;
    node.className = "toast" + (error ? " error" : "") + " show";
    clearTimeout(node.timer);
    node.timer = setTimeout(function () { node.classList.remove("show"); }, 3200);
  }

  async function refresh(silent) {
    try {
      state = await api("/api/state");
      render();
      el("sync-time").textContent = "SYNC " + new Date().toLocaleTimeString("zh-CN", { hour12: false });
      if (!silent) toast("本地状态已同步");
    } catch (error) {
      el("sync-time").textContent = "LINK ERROR";
      if (!silent) toast(error.message, true);
    }
  }

  function renderMetrics() {
    var open = state.issues.filter(function (issue) { return issue.status !== "done"; }).length;
    var running = state.runs.filter(function (run) { return run.status === "running"; }).length;
    el("metric-open").textContent = two(open);
    el("metric-runs").textContent = two(running);
    el("metric-agents").textContent = two(state.agents.length);
    el("version").textContent = "CORE " + state.version;
  }

  function renderIssues() {
    var issues = state.issues.slice().sort(function (a, b) { return String(b.id).localeCompare(String(a.id)); });
    if (filter !== "all") issues = issues.filter(function (issue) { return issue.status === filter; });
    el("issue-count").textContent = issues.length + " RECORDS";
    el("issue-empty").classList.toggle("hidden", issues.length > 0);
    el("issue-list").innerHTML = issues.map(function (issue) {
      return '<article class="issue-row" data-issue="' + esc(issue.id) + '" tabindex="0">' +
        '<span class="issue-id">' + esc(issue.id.toUpperCase()) + '</span>' +
        '<div class="issue-main"><strong>' + esc(issue.title) + '</strong><span>' + esc(truncate(issue.description, 92)) + '</span></div>' +
        '<div class="assignee">' + esc(assigneeName(issue)) + '<span>' + esc(issue.assignee_type.toUpperCase()) + '</span></div>' +
        '<time class="timestamp">UPDATED<br>' + esc(formatTime(issue.updated_at)) + '</time>' +
        '<span class="status status-' + esc(issue.status) + '">' + esc(statusText[issue.status] || issue.status) + '</span>' +
      '</article>';
    }).join("");
  }

  function renderAgents() {
    el("agent-count").textContent = state.agents.length + " NODES";
    el("agent-empty").classList.toggle("hidden", state.agents.length > 0);
    el("agent-list").innerHTML = state.agents.map(function (agent, index) {
      var skills = (agent.skills || []).map(function (skill) {
        var parts = skill.split("/");
        return parts[parts.length - 1] === "SKILL.md" ? parts[parts.length - 2] : parts[parts.length - 1];
      });
      return '<article class="node-card">' +
        '<span class="node-index">NODE / ' + two(index + 1) + '</span>' +
        '<h3>' + esc(agent.name || agent.id) + '</h3><span class="node-id">@' + esc(agent.id) + '</span>' +
        '<p class="node-role">' + esc(agent.role || "未设置职责") + '</p>' +
        '<div class="command-line">$ ' + esc((agent.command || []).join(" ")) + '</div>' +
        '<div class="tag-row">' + (skills.length ? skills.map(function (skill) { return '<span class="tag">' + esc(skill) + '</span>'; }).join("") : '<span class="tag">NO SKILLS</span>') + '</div>' +
      '</article>';
    }).join("");
  }

  function renderSquads() {
    el("squad-count").textContent = state.squads.length + " SQUADS";
    el("squad-empty").classList.toggle("hidden", state.squads.length > 0);
    el("squad-list").innerHTML = state.squads.map(function (squad, index) {
      var leader = byId("agents", squad.leader_id);
      var existing = new Set((squad.members || []).map(function (member) { return member.agent_id; }).concat([squad.leader_id]));
      var candidates = state.agents.filter(function (agent) { return !existing.has(agent.id); });
      var roster = '<div class="roster-line"><b>LEADER</b><span>' + esc(leader ? leader.name : squad.leader_id) + ' <small>@' + esc(squad.leader_id) + '</small></span></div>';
      roster += (squad.members || []).map(function (member) {
        var agent = byId("agents", member.agent_id);
        return '<div class="roster-line worker"><b>WORKER</b><span>' + esc(agent ? agent.name : member.agent_id) + ' · ' + esc(member.role || "未设置职责") + '</span></div>';
      }).join("");
      var add = candidates.length ? '<form class="member-form" data-squad="' + esc(squad.id) + '"><select name="agent_id" aria-label="选择新成员">' + candidates.map(function (agent) { return '<option value="' + esc(agent.id) + '">' + esc(agent.name || agent.id) + '</option>'; }).join("") + '</select><button class="mini-button" type="submit">＋ MEMBER</button></form>' : '';
      return '<article class="node-card">' +
        '<span class="node-index">SQUAD / ' + two(index + 1) + '</span>' +
        '<h3>' + esc(squad.name || squad.id) + '</h3><span class="node-id">#' + esc(squad.id) + '</span>' +
        '<div class="squad-roster">' + roster + '</div>' + add +
      '</article>';
    }).join("");
  }

  function render() {
    renderMetrics();
    renderIssues();
    renderAgents();
    renderSquads();
  }

  function switchView(next) {
    view = next;
    document.querySelectorAll(".nav-item").forEach(function (button) { button.classList.toggle("active", button.dataset.view === next); });
    document.querySelectorAll(".view").forEach(function (section) { section.classList.toggle("active", section.id === "view-" + next); });
    el("view-title").textContent = titleText[next];
    el("create-button").innerHTML = '<span>＋</span> ' + ({ issues: "新建任务", agents: "登记智能体", squads: "新建小队" }[next]);
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

  function optionsForAssignees() {
    var agents = state.agents.map(function (agent) { return '<option value="agent:' + esc(agent.id) + '">Agent · ' + esc(agent.name || agent.id) + '</option>'; });
    var squads = state.squads.map(function (squad) { return '<option value="squad:' + esc(squad.id) + '">Squad · ' + esc(squad.name || squad.id) + '</option>'; });
    return agents.concat(squads).join("");
  }

  function openCreateModal() {
    var layer = addOverlay("modal-layer open");
    var content = "";
    if (view === "issues") {
      content = '<p class="eyebrow">ISSUE / NEW RECORD</p><h2>建立任务</h2><form id="create-form" class="form-grid">' +
        '<div class="field full"><label>标题 *</label><input name="title" required maxlength="160" placeholder="例如：检查 RTL lint 并修复阻塞项"></div>' +
        '<div class="field full"><label>任务描述 *</label><textarea name="description" required placeholder="说明目标、边界和验收条件"></textarea></div>' +
        '<div class="field"><label>指派给 *</label><select name="assignee" required>' + optionsForAssignees() + '</select></div>' +
        '<div class="field"><label>工作目录 *</label><input name="cwd" required value="." placeholder="/path/to/project"></div>' +
        '<div class="field full"><span class="field-note">目录必须已存在于运行 multica-core 的机器上。</span><p class="form-error"></p><button class="form-submit">CREATE ISSUE</button></div></form>';
    } else if (view === "agents") {
      content = '<p class="eyebrow">AGENT / REGISTER NODE</p><h2>登记智能体</h2><form id="create-form" class="form-grid">' +
        '<div class="field"><label>ID *</label><input name="id" required pattern="[A-Za-z0-9._-]+" placeholder="rtl-reviewer"></div>' +
        '<div class="field"><label>显示名称</label><input name="name" placeholder="RTL Reviewer"></div>' +
        '<div class="field full"><label>职责</label><input name="role" placeholder="检查 RTL、定位 lint 问题并验证修复"></div>' +
        '<div class="field full"><label>可执行文件 *</label><input name="exec" required placeholder="/opt/agent/bin/agent-cli"></div>' +
        '<div class="field full"><label>参数 · 每行一个</label><textarea name="args" placeholder="run&#10;--prompt&#10;{prompt}">{prompt}</textarea><span class="field-note">支持 {prompt}、{cwd}、{issue_id}、{agent_id}。</span></div>' +
        '<div class="field full"><label>Skill 路径 · 每行一个</label><textarea name="skills" placeholder="/opt/skills/rtl-review/SKILL.md"></textarea><p class="form-error"></p><button class="form-submit">REGISTER NODE</button></div></form>';
    } else {
      var agentOptions = state.agents.map(function (agent) { return '<option value="' + esc(agent.id) + '">' + esc(agent.name || agent.id) + ' · @' + esc(agent.id) + '</option>'; }).join("");
      content = '<p class="eyebrow">SQUAD / ASSEMBLE</p><h2>新建小队</h2><form id="create-form" class="form-grid">' +
        '<div class="field"><label>ID *</label><input name="id" required pattern="[A-Za-z0-9._-]+" placeholder="verification-team"></div>' +
        '<div class="field"><label>编组名称</label><input name="name" placeholder="Verification Team"></div>' +
        '<div class="field"><label>Leader *</label><select name="leader_id" required>' + agentOptions + '</select></div>' +
        '<div class="field"><label>首位成员</label><select name="worker_id"><option value="">稍后添加</option>' + agentOptions + '</select></div>' +
        '<div class="field full"><label>成员职责</label><input name="worker_role" placeholder="实现与验证"></div>' +
        '<div class="field full"><span class="field-note">Leader 与成员必须是不同的智能体。</span><p class="form-error"></p><button class="form-submit">ASSEMBLE SQUAD</button></div></form>';
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
        body = { title: values.get("title"), description: values.get("description"), assignee_type: parts[0], assignee_id: parts.slice(1).join(":"), cwd: values.get("cwd") };
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
      toast("记录已写入本地存储");
    } catch (error) {
      errorNode.textContent = error.message;
      button.disabled = false;
    }
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

  function renderDetail(issue) {
    if (activeIssue !== issue.id) return;
    var panel = el("detail-panel");
    var comments = issue.comments || [];
    var runs = issue.runs || [];
    panel.innerHTML = '<button class="detail-close" aria-label="关闭">×</button>' +
      '<div class="detail-title"><p class="eyebrow">' + esc(issue.id.toUpperCase()) + '</p><h2>' + esc(issue.title) + '</h2><p>' + esc(issue.description) + '</p></div>' +
      '<div class="detail-meta"><div><small>ASSIGNEE</small><span>' + esc(assigneeName(issue)) + '</span></div><div><small>STATUS</small><span class="status status-' + esc(issue.status) + '">' + esc(statusText[issue.status] || issue.status) + '</span></div><div><small>WORK DIRECTORY</small><span>' + esc(issue.cwd) + '</span></div><div><small>UPDATED</small><span>' + esc(formatTime(issue.updated_at)) + '</span></div></div>' +
      '<div class="detail-actions"><button id="run-issue" class="' + (detailBusy ? 'running' : '') + '">' + (detailBusy ? 'AGENTS RUNNING…' : '▶ RUN ISSUE') + '</button><select id="issue-status" aria-label="修改任务状态">' + Object.keys(statusText).map(function (status) { return '<option value="' + status + '"' + (status === issue.status ? ' selected' : '') + '>' + esc(statusText[status]) + '</option>'; }).join("") + '</select></div>' +
      '<h3 class="subhead">COMMENT TIMELINE / ' + comments.length + '</h3>' +
      '<div class="timeline">' + (comments.length ? comments.map(function (comment) { return '<article class="timeline-entry"><header><span>' + esc(comment.author_name || comment.author_id) + '</span><time>' + esc(formatTime(comment.created_at)) + '</time></header><pre>' + esc(comment.content) + '</pre></article>'; }).join("") : '<p class="node-role">尚无智能体回复。</p>') + '</div>' +
      '<h3 class="subhead">RUN LOG / ' + runs.length + '</h3>' +
      '<div>' + (runs.length ? runs.map(function (run) { return '<div class="run-row"><span>' + esc(run.id) + '</span><span>@' + esc(run.agent_id) + ' · ' + esc(run.trigger) + '</span><span class="status-' + esc(run.status === 'completed' ? 'done' : run.status === 'running' ? 'in_progress' : 'todo') + '">' + esc(run.status.toUpperCase()) + '</span></div>'; }).join("") : '<p class="node-role">尚无运行记录。</p>') + '</div>';
    panel.classList.add("open");
    panel.setAttribute("aria-hidden", "false");
    var scrim = document.querySelector(".scrim") || addOverlay("scrim");
    scrim.classList.add("open");
    panel.querySelector(".detail-close").addEventListener("click", closeDetail);
    scrim.onclick = closeDetail;
    panel.querySelector("#run-issue").addEventListener("click", runActiveIssue);
    panel.querySelector("#issue-status").addEventListener("change", changeIssueStatus);
  }

  function closeDetail() {
    activeIssue = null;
    detailBusy = false;
    el("detail-panel").classList.remove("open");
    el("detail-panel").setAttribute("aria-hidden", "true");
    var scrim = document.querySelector(".scrim");
    if (scrim) scrim.classList.remove("open");
  }

  async function runActiveIssue() {
    if (!activeIssue || detailBusy) return;
    detailBusy = true;
    await openIssue(activeIssue, true);
    var id = activeIssue;
    try {
      var result = await api("/api/issues/" + encodeURIComponent(id) + "/run", { method: "POST", body: { max_runs: 16 } });
      toast(result.exit_code === 0 ? "协作运行结束，等待复核" : "运行结束，但 Agent 返回失败", result.exit_code !== 0);
    } catch (error) {
      toast(error.message, true);
    }
    detailBusy = false;
    await refresh(true);
    if (activeIssue === id) await openIssue(id, true);
  }

  async function changeIssueStatus(event) {
    if (!activeIssue) return;
    var id = activeIssue;
    try {
      await api("/api/issues/" + encodeURIComponent(id) + "/status", { method: "POST", body: { status: event.target.value } });
      await refresh(true);
      await openIssue(id, true);
      toast("任务状态已更新");
    } catch (error) {
      toast(error.message, true);
    }
  }

  document.querySelectorAll(".nav-item").forEach(function (button) {
    button.addEventListener("click", function () { switchView(button.dataset.view); });
  });
  document.querySelectorAll("#issue-filters button").forEach(function (button) {
    button.addEventListener("click", function () {
      filter = button.dataset.status;
      document.querySelectorAll("#issue-filters button").forEach(function (item) { item.classList.toggle("active", item === button); });
      renderIssues();
    });
  });
  el("refresh").addEventListener("click", function () { refresh(false); });
  el("create-button").addEventListener("click", openCreateModal);
  el("issue-list").addEventListener("click", function (event) {
    var row = event.target.closest("[data-issue]");
    if (row) openIssue(row.dataset.issue);
  });
  el("issue-list").addEventListener("keydown", function (event) {
    var row = event.target.closest("[data-issue]");
    if (row && (event.key === "Enter" || event.key === " ")) {
      event.preventDefault();
      openIssue(row.dataset.issue);
    }
  });
  el("squad-list").addEventListener("submit", async function (event) {
    var form = event.target.closest(".member-form");
    if (!form) return;
    event.preventDefault();
    try {
      await api("/api/squads/" + encodeURIComponent(form.dataset.squad) + "/members", { method: "POST", body: { agent_id: form.agent_id.value } });
      await refresh(true);
      toast("成员已加入小队");
    } catch (error) {
      toast(error.message, true);
    }
  });
  document.addEventListener("keydown", function (event) {
    if (event.key === "Escape") {
      closeModal();
      closeDetail();
    }
  });

  refresh(true);
  setInterval(function () {
    refresh(true);
    if (activeIssue && !detailBusy) openIssue(activeIssue, true);
  }, 5000);
})();
