#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "httplib.h"
#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char *kVersion = "0.2.0";
constexpr std::size_t kMaxCapturedOutput = 4U * 1024U * 1024U;
std::mutex g_write_mutex;
std::mutex g_process_mutex;
std::map<std::string, pid_t> g_active_processes;
std::set<std::string> g_cancelled_issues;
std::set<std::string> g_active_issues;

struct ProcessResult {
    int exit_code = 127;
    std::string output;
    bool cancelled = false;
    bool timed_out = false;
};

struct Dispatch {
    std::string agent_id;
    std::string reason;
};

bool cancellation_requested(const std::string &issue_id) {
    std::lock_guard<std::mutex> lock(g_process_mutex);
    return g_cancelled_issues.count(issue_id) > 0;
}

bool request_cancellation(const std::string &issue_id) {
    std::lock_guard<std::mutex> lock(g_process_mutex);
    const bool active_issue = g_active_issues.count(issue_id) > 0;
    g_cancelled_issues.insert(issue_id);
    const auto active = g_active_processes.find(issue_id);
    if (active != g_active_processes.end()) ::kill(-active->second, SIGTERM);
    return active_issue;
}

bool begin_issue_run(const std::string &issue_id) {
    std::lock_guard<std::mutex> lock(g_process_mutex);
    if (g_active_issues.count(issue_id)) return false;
    g_active_issues.insert(issue_id);
    g_cancelled_issues.erase(issue_id);
    return true;
}

void finish_issue_run(const std::string &issue_id) {
    std::lock_guard<std::mutex> lock(g_process_mutex);
    g_active_issues.erase(issue_id);
    g_active_processes.erase(issue_id);
    g_cancelled_issues.erase(issue_id);
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string now_utc() {
    const std::time_t value = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&value, &tm);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}

std::string read_text(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read " + path.string());
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

json read_json(const fs::path &path) {
    try {
        return json::parse(read_text(path));
    } catch (const json::exception &error) {
        throw std::runtime_error("invalid JSON in " + path.string() + ": " + error.what());
    }
}

void atomic_write(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    const fs::path temporary = path.string() + ".tmp." + std::to_string(static_cast<long long>(::getpid()));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot write " + temporary.string());
        output << content;
        output.flush();
        if (!output) throw std::runtime_error("failed while writing " + temporary.string());
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(path, error);
        error.clear();
        fs::rename(temporary, path, error);
    }
    if (error) {
        fs::remove(temporary);
        throw std::runtime_error("cannot replace " + path.string() + ": " + error.message());
    }
}

void write_json(const fs::path &path, const json &value) {
    atomic_write(path, value.dump(2) + "\n");
}

fs::path default_home() {
    if (const char *configured = std::getenv("MULTICA_CORE_HOME")) {
        if (*configured) return fs::path(configured);
    }
    if (const char *xdg = std::getenv("XDG_DATA_HOME")) {
        if (*xdg) return fs::path(xdg) / "multica-core";
    }
    if (const char *home = std::getenv("HOME")) {
        if (*home) return fs::path(home) / ".local" / "share" / "multica-core";
    }
    return fs::current_path() / ".multica-core";
}

void ensure_home(const fs::path &home) {
    fs::create_directories(home / "agents");
    fs::create_directories(home / "squads");
    fs::create_directories(home / "issues");
    fs::create_directories(home / "runs");
    const fs::path sequence = home / "sequence.json";
    if (!fs::exists(sequence)) {
        write_json(sequence, json{{"issue", 0}, {"run", 0}, {"comment", 0}});
    }
}

json load_entities(const fs::path &home, const std::string &kind) {
    json values = json::array();
    std::vector<fs::path> paths;
    for (const auto &entry : fs::directory_iterator(home / kind)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    for (const auto &path : paths) values.push_back(read_json(path));
    return values;
}

std::string next_id(const fs::path &home, const std::string &key, const std::string &prefix) {
    const fs::path path = home / "sequence.json";
    json sequence = read_json(path);
    const long long value = sequence.value(key, 0LL) + 1;
    sequence[key] = value;
    write_json(path, sequence);
    std::ostringstream id;
    id << prefix;
    id.width(6);
    id.fill('0');
    id << value;
    return id.str();
}

fs::path entity_path(const fs::path &home, const std::string &kind, const std::string &id) {
    if (id.empty() || id == "." || id == ".." || id.find('/') != std::string::npos) {
        throw std::runtime_error("invalid " + kind + " id: " + id);
    }
    return home / kind / (id + ".json");
}

json load_entity(const fs::path &home, const std::string &kind, const std::string &id) {
    const fs::path path = entity_path(home, kind, id);
    if (!fs::exists(path)) throw std::runtime_error(kind + " not found: " + id);
    return read_json(path);
}

void save_entity(const fs::path &home, const std::string &kind, const std::string &id, const json &value) {
    write_json(entity_path(home, kind, id), value);
}

void validate_assignee_ready(const fs::path &home, const std::string &type, const std::string &id) {
    if (type == "agent") {
        const json agent = load_entity(home, "agents", id);
        if (!agent.value("enabled", true)) throw std::runtime_error("agent is disabled: " + id);
        return;
    }
    const json squad = load_entity(home, "squads", id);
    const auto members = squad.value("members", json::array());
    if (members.empty()) throw std::runtime_error("squad has no worker members: " + id);
    const std::string leader_id = squad.value("leader_id", "");
    const json leader = load_entity(home, "agents", leader_id);
    if (!leader.value("enabled", true)) throw std::runtime_error("squad leader is disabled: " + leader_id);
    for (const auto &member : members) {
        const std::string member_id = member.value("agent_id", "");
        const json agent = load_entity(home, "agents", member_id);
        if (!agent.value("enabled", true)) throw std::runtime_error("squad member is disabled: " + member_id);
    }
}

std::string replace_all(std::string value, const std::string &needle, const std::string &replacement) {
    if (needle.empty()) return value;
    std::size_t position = 0;
    while ((position = value.find(needle, position)) != std::string::npos) {
        value.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
    return value;
}

std::vector<std::string> agent_command(const json &agent, const json &issue, const std::string &prompt) {
    if (!agent.contains("command") || !agent["command"].is_array() || agent["command"].empty()) {
        throw std::runtime_error("agent " + agent.value("id", "") + " has no command");
    }
    const std::string cwd = issue.value("cwd", fs::current_path().string());
    const std::string issue_id = issue.value("id", "");
    const std::string agent_id = agent.value("id", "");
    bool has_prompt = false;
    std::vector<std::string> command;
    for (const auto &part_value : agent["command"]) {
        if (!part_value.is_string()) throw std::runtime_error("agent command entries must be strings");
        std::string part = part_value.get<std::string>();
        if (part.find("{prompt}") != std::string::npos) has_prompt = true;
        part = replace_all(std::move(part), "{prompt}", prompt);
        part = replace_all(std::move(part), "{cwd}", cwd);
        part = replace_all(std::move(part), "{issue_id}", issue_id);
        part = replace_all(std::move(part), "{agent_id}", agent_id);
        command.push_back(std::move(part));
    }
    if (!has_prompt) command.push_back(prompt);
    return command;
}

ProcessResult run_process(const std::vector<std::string> &command,
                          const fs::path &cwd,
                          const fs::path &home,
                          const std::string &issue_id,
                          const std::string &agent_id,
                          int timeout_seconds) {
    if (command.empty()) throw std::runtime_error("empty command");
    int output_pipe[2];
    if (::pipe(output_pipe) != 0) throw std::runtime_error("pipe failed: " + std::string(std::strerror(errno)));

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        throw std::runtime_error("fork failed: " + std::string(std::strerror(errno)));
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (::getppid() == 1) _exit(125);
        ::close(output_pipe[0]);
        ::dup2(output_pipe[1], STDOUT_FILENO);
        ::dup2(output_pipe[1], STDERR_FILENO);
        ::close(output_pipe[1]);
        if (::chdir(cwd.c_str()) != 0) {
            std::cerr << "cannot chdir to " << cwd << ": " << std::strerror(errno) << "\n";
            _exit(126);
        }
        ::setenv("MULTICA_CORE_HOME", home.c_str(), 1);
        ::setenv("MULTICA_CORE_ISSUE_ID", issue_id.c_str(), 1);
        ::setenv("MULTICA_CORE_AGENT_ID", agent_id.c_str(), 1);

        std::vector<char *> argv;
        argv.reserve(command.size() + 1);
        for (const auto &part : command) argv.push_back(const_cast<char *>(part.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        std::cerr << "cannot execute " << command[0] << ": " << std::strerror(errno) << "\n";
        _exit(127);
    }

    ::close(output_pipe[1]);
    ::setpgid(pid, pid);
    const int flags = ::fcntl(output_pipe[0], F_GETFL, 0);
    if (flags >= 0) ::fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);
    {
        std::lock_guard<std::mutex> lock(g_process_mutex);
        g_active_processes[issue_id] = pid;
    }

    std::string output;
    char buffer[8192];
    int status = 0;
    bool child_done = false;
    bool sent_term = false;
    ProcessResult result;
    const auto started = std::chrono::steady_clock::now();
    auto term_sent_at = started;

    while (!child_done) {
        struct pollfd descriptor { output_pipe[0], POLLIN | POLLHUP, 0 };
        const int poll_result = ::poll(&descriptor, 1, 100);
        if (poll_result < 0 && errno != EINTR) {
            ::kill(-pid, SIGKILL);
            throw std::runtime_error("poll failed: " + std::string(std::strerror(errno)));
        }
        if (poll_result > 0 && (descriptor.revents & (POLLIN | POLLHUP))) {
            while (true) {
                const ssize_t size = ::read(output_pipe[0], buffer, sizeof(buffer));
                if (size > 0) {
                    const std::size_t remaining = kMaxCapturedOutput > output.size()
                                                      ? kMaxCapturedOutput - output.size()
                                                      : 0;
                    output.append(buffer, std::min<std::size_t>(remaining, static_cast<std::size_t>(size)));
                    continue;
                }
                if (size < 0 && errno == EINTR) continue;
                break;
            }
        }

        const pid_t waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) child_done = true;
        else if (waited < 0 && errno != EINTR) throw std::runtime_error("waitpid failed: " + std::string(std::strerror(errno)));

        const auto current = std::chrono::steady_clock::now();
        const bool cancelled = cancellation_requested(issue_id);
        if (!sent_term && cancelled) {
            result.cancelled = true;
            ::kill(-pid, SIGTERM);
            sent_term = true;
            term_sent_at = current;
        } else if (!sent_term && current - started >= std::chrono::seconds(timeout_seconds)) {
            result.timed_out = true;
            ::kill(-pid, SIGTERM);
            sent_term = true;
            term_sent_at = current;
        } else if (sent_term && !child_done && current - term_sent_at >= std::chrono::seconds(2)) {
            ::kill(-pid, SIGKILL);
        }
    }

    while (true) {
        const ssize_t size = ::read(output_pipe[0], buffer, sizeof(buffer));
        if (size > 0) {
            const std::size_t remaining = kMaxCapturedOutput > output.size()
                                              ? kMaxCapturedOutput - output.size()
                                              : 0;
            output.append(buffer, std::min<std::size_t>(remaining, static_cast<std::size_t>(size)));
            continue;
        }
        if (size < 0 && errno == EINTR) continue;
        break;
    }
    ::close(output_pipe[0]);
    {
        std::lock_guard<std::mutex> lock(g_process_mutex);
        g_active_processes.erase(issue_id);
    }
    result.output = trim(output);
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exit_code = 128 + WTERMSIG(status);
    return result;
}

std::vector<std::string> skill_labels(const json &agent) {
    std::vector<std::string> labels;
    if (!agent.contains("skills") || !agent["skills"].is_array()) return labels;
    for (const auto &entry : agent["skills"]) {
        if (!entry.is_string()) continue;
        const fs::path path(entry.get<std::string>());
        if (fs::exists(path)) {
            labels.push_back(path.filename() == "SKILL.md" ? path.parent_path().filename().string()
                                                            : path.filename().string());
        } else {
            labels.push_back(path.string());
        }
    }
    return labels;
}

std::string load_skills(const json &agent) {
    std::ostringstream result;
    if (!agent.contains("skills") || !agent["skills"].is_array()) return "";
    for (const auto &entry : agent["skills"]) {
        if (!entry.is_string()) continue;
        fs::path path(entry.get<std::string>());
        if (fs::is_directory(path)) path /= "SKILL.md";
        if (!fs::is_regular_file(path)) continue;
        result << "\n\n## Skill: " << path.parent_path().filename().string() << "\n\n";
        result << read_text(path);
    }
    return result.str();
}

std::string render_issue(const json &issue) {
    std::ostringstream prompt;
    prompt << "Issue " << issue.value("id", "") << "\n";
    prompt << "Title: " << issue.value("title", "") << "\n";
    prompt << "Description:\n" << issue.value("description", "") << "\n";
    prompt << "Status: " << issue.value("status", "todo") << "\n";
    if (issue.contains("comments") && issue["comments"].is_array() && !issue["comments"].empty()) {
        prompt << "\nComments, oldest first:\n";
        for (const auto &comment : issue["comments"]) {
            prompt << "\n[" << comment.value("author_name", comment.value("author_id", "unknown")) << "]\n";
            prompt << comment.value("content", "") << "\n";
        }
    }
    return prompt.str();
}

std::string render_roster(const fs::path &home, const json &squad) {
    std::ostringstream roster;
    roster << "\nSquad roster:\n";
    if (!squad.contains("members") || !squad["members"].is_array()) return roster.str();
    for (const auto &member : squad["members"]) {
        const std::string id = member.value("agent_id", "");
        if (id.empty()) continue;
        const json agent = load_entity(home, "agents", id);
        roster << "- " << agent.value("name", id) << " — role: "
               << member.value("role", agent.value("role", "unspecified"));
        const auto labels = skill_labels(agent);
        if (!labels.empty()) {
            roster << " — skills: ";
            for (std::size_t i = 0; i < labels.size(); ++i) {
                if (i) roster << ", ";
                roster << labels[i];
            }
        }
        roster << " — [@" << agent.value("name", id) << "](mention://agent/" << id << ")\n";
    }
    return roster.str();
}

std::string build_prompt(const fs::path &home,
                         const json &agent,
                         const json &issue,
                         const std::optional<json> &squad,
                         bool is_leader,
                         const std::string &reason) {
    std::ostringstream prompt;
    prompt << "You are " << agent.value("name", agent.value("id", "agent")) << ".\n";
    if (!agent.value("role", "").empty()) prompt << "Role: " << agent.value("role", "") << "\n";
    prompt << "Trigger: " << reason << "\n\n";
    if (is_leader && squad.has_value()) {
        prompt << "You are the squad leader. Coordinate the issue; do not perform member work yourself. "
                  "Delegate by writing one or more exact member mentions from the roster. Keep delegation concise. "
                  "When the overall issue is complete, return a concise final assessment without a member mention.\n";
        prompt << render_roster(home, *squad) << "\n";
    } else {
        prompt << "Work on the assigned issue and return a concise result with verification evidence.\n\n";
    }
    prompt << render_issue(issue);
    prompt << load_skills(agent);
    return prompt.str();
}

std::set<std::string> mentioned_agents(const std::string &content) {
    static const std::regex mention(R"(\[@?[^\]]+\]\(mention://agent/([A-Za-z0-9._-]+)\))");
    std::set<std::string> ids;
    for (std::sregex_iterator it(content.begin(), content.end(), mention), end; it != end; ++it) {
        ids.insert((*it)[1].str());
    }
    return ids;
}

void append_comment(const fs::path &home, json &issue, const json &agent, const std::string &content) {
    if (!issue.contains("comments") || !issue["comments"].is_array()) issue["comments"] = json::array();
    issue["comments"].push_back(json{
        {"id", next_id(home, "comment", "comment-")},
        {"author_type", "agent"},
        {"author_id", agent.value("id", "")},
        {"author_name", agent.value("name", agent.value("id", "agent"))},
        {"content", content},
        {"created_at", now_utc()},
    });
    issue["updated_at"] = now_utc();
}

ProcessResult execute_agent(const fs::path &home,
                            json &issue,
                            const std::string &agent_id,
                            const std::optional<json> &squad,
                            bool is_leader,
                            const std::string &reason) {
    const json agent = load_entity(home, "agents", agent_id);
    if (!agent.value("enabled", true)) throw std::runtime_error("agent is disabled: " + agent_id);
    const std::string run_id = next_id(home, "run", "run-");
    json run{
        {"id", run_id},
        {"issue_id", issue.value("id", "")},
        {"agent_id", agent_id},
        {"status", "running"},
        {"leader", is_leader},
        {"trigger", reason},
        {"started_at", now_utc()},
    };
    save_entity(home, "runs", run_id, run);

    try {
        const std::string prompt = build_prompt(home, agent, issue, squad, is_leader, reason);
        const std::vector<std::string> command = agent_command(agent, issue, prompt);
        fs::path cwd = issue.value("cwd", fs::current_path().string());
        if (!fs::is_directory(cwd)) throw std::runtime_error("issue cwd is not a directory: " + cwd.string());
        run["prompt"] = prompt;
        run["command"] = agent.value("command", json::array());
        save_entity(home, "runs", run_id, run);

        const ProcessResult result = run_process(command, cwd, home, issue.value("id", ""), agent_id,
                                                 issue.value("timeout_seconds", 900));
        run["status"] = result.cancelled ? "cancelled" : (result.exit_code == 0 ? "completed" : "failed");
        run["exit_code"] = result.exit_code;
        run["output"] = result.output;
        if (result.timed_out) run["failure_reason"] = "timeout";
        if (result.cancelled) run["failure_reason"] = "cancelled";
        run["finished_at"] = now_utc();
        save_entity(home, "runs", run_id, run);

        std::string comment = result.output;
        if (result.cancelled) comment = "Agent run was cancelled." + (comment.empty() ? "" : "\n\n" + comment);
        else if (result.timed_out) comment = "Agent run timed out after " + std::to_string(issue.value("timeout_seconds", 900)) +
                                             " seconds." + (comment.empty() ? "" : "\n\n" + comment);
        else if (comment.empty()) comment = "Agent process exited with code " + std::to_string(result.exit_code) + " without output.";
        append_comment(home, issue, agent, comment);
        save_entity(home, "issues", issue.value("id", ""), issue);
        std::cout << "[" << agent.value("name", agent_id) << "] " << comment << "\n";
        return result;
    } catch (const std::exception &error) {
        const std::string message = std::string("Agent execution failed: ") + error.what();
        run["status"] = "failed";
        run["exit_code"] = 1;
        run["output"] = message;
        run["finished_at"] = now_utc();
        save_entity(home, "runs", run_id, run);
        append_comment(home, issue, agent, message);
        save_entity(home, "issues", issue.value("id", ""), issue);
        std::cerr << "[" << agent.value("name", agent_id) << "] " << message << "\n";
        return ProcessResult{1, message};
    }
}

ProcessResult execute_with_retries(const fs::path &home,
                                   json &issue,
                                   const std::string &agent_id,
                                   const std::optional<json> &squad,
                                   bool is_leader,
                                   const std::string &reason) {
    const int max_retries = issue.value("max_retries", 0);
    ProcessResult result;
    for (int attempt = 0; attempt <= max_retries; ++attempt) {
        const std::string trigger = attempt == 0
                                        ? reason
                                        : "retry " + std::to_string(attempt) + " after failed run";
        result = execute_agent(home, issue, agent_id, squad, is_leader, trigger);
        if (result.exit_code == 0 || result.cancelled) return result;
    }
    return result;
}

bool pending_contains(std::queue<Dispatch> pending, const std::string &agent_id) {
    while (!pending.empty()) {
        if (pending.front().agent_id == agent_id) return true;
        pending.pop();
    }
    return false;
}

void enqueue_unique(std::queue<Dispatch> &pending, const Dispatch &dispatch) {
    if (!pending_contains(pending, dispatch.agent_id)) pending.push(dispatch);
}

std::string review_policy(const json &value, const std::string &fallback = "auto");

int run_direct_issue(const fs::path &home, json &issue, const std::string &agent_id) {
    issue["status"] = "in_progress";
    issue.erase("last_error");
    issue["updated_at"] = now_utc();
    save_entity(home, "issues", issue.value("id", ""), issue);
    const ProcessResult result = execute_with_retries(home, issue, agent_id, std::nullopt, false, "issue assignment");
    issue["status"] = result.cancelled
                          ? "cancelled"
                          : (result.exit_code == 0
                                 ? (review_policy(issue) == "manual" ? "in_review" : "done")
                                 : "failed");
    if (result.cancelled) issue["last_error"] = "run cancelled by user";
    issue["updated_at"] = now_utc();
    save_entity(home, "issues", issue.value("id", ""), issue);
    return result.exit_code == 0 ? 0 : 1;
}

int run_squad_issue(const fs::path &home, json &issue, const std::string &squad_id, int max_runs) {
    const json squad = load_entity(home, "squads", squad_id);
    const std::string leader_id = squad.value("leader_id", "");
    if (leader_id.empty()) throw std::runtime_error("squad has no leader: " + squad_id);

    std::set<std::string> members;
    if (squad.contains("members") && squad["members"].is_array()) {
        for (const auto &member : squad["members"]) members.insert(member.value("agent_id", ""));
    }
    members.erase("");
    if (members.empty()) throw std::runtime_error("squad has no worker members: " + squad_id);

    issue["status"] = "in_progress";
    issue.erase("last_error");
    issue["updated_at"] = now_utc();
    save_entity(home, "issues", issue.value("id", ""), issue);

    std::queue<Dispatch> pending;
    pending.push({leader_id, "squad assignment"});
    bool leader_delegated = false;

    for (int iteration = 0; iteration < max_runs && !pending.empty(); ++iteration) {
        const Dispatch dispatch = pending.front();
        pending.pop();
        const bool is_leader = dispatch.agent_id == leader_id;
        const ProcessResult result = execute_with_retries(home, issue, dispatch.agent_id, squad, is_leader, dispatch.reason);
        if (result.exit_code != 0) {
            issue["status"] = result.cancelled ? "cancelled" : "failed";
            issue["last_error"] = result.cancelled ? "run cancelled by user" : "agent run failed: " + dispatch.agent_id;
            issue["updated_at"] = now_utc();
            save_entity(home, "issues", issue.value("id", ""), issue);
            return 1;
        }

        const auto mentions = mentioned_agents(result.output);
        if (is_leader) {
            int accepted = 0;
            for (const auto &id : mentions) {
                if (!members.count(id)) continue;
                enqueue_unique(pending, {id, "delegated by squad leader"});
                ++accepted;
            }
            if (accepted == 0) {
                if (!leader_delegated) {
                    issue["status"] = "failed";
                    issue["last_error"] = "leader did not delegate to a squad member";
                    issue["updated_at"] = now_utc();
                    save_entity(home, "issues", issue.value("id", ""), issue);
                    std::cerr << "leader did not delegate to a squad member\n";
                    return 1;
                }
                issue["status"] = review_policy(issue) == "manual" ? "in_review" : "done";
                issue["updated_at"] = now_utc();
                save_entity(home, "issues", issue.value("id", ""), issue);
                return 0;
            }
            leader_delegated = true;
        } else {
            for (const auto &id : mentions) {
                if (members.count(id) && id != dispatch.agent_id) {
                    enqueue_unique(pending, {id, "mentioned by squad member"});
                }
            }
            enqueue_unique(pending, {leader_id, "squad member reported a result"});
        }
    }

    issue["status"] = "failed";
    issue["last_error"] = "squad run limit reached before completion";
    issue["updated_at"] = now_utc();
    save_entity(home, "issues", issue.value("id", ""), issue);
    std::cerr << "squad run limit reached before review\n";
    return 1;
}

std::optional<std::string> option_value(const std::vector<std::string> &args,
                                        const std::string &name,
                                        std::size_t start = 0) {
    for (std::size_t i = start; i < args.size(); ++i) {
        if (args[i] == name) {
            if (i + 1 >= args.size()) throw std::runtime_error(name + " requires a value");
            return args[i + 1];
        }
    }
    return std::nullopt;
}

std::vector<std::string> repeated_option(const std::vector<std::string> &args,
                                         const std::string &name,
                                         std::size_t start = 0) {
    std::vector<std::string> values;
    for (std::size_t i = start; i < args.size(); ++i) {
        if (args[i] == name) {
            if (i + 1 >= args.size()) throw std::runtime_error(name + " requires a value");
            values.push_back(args[++i]);
        }
    }
    return values;
}

std::string required_option(const std::vector<std::string> &args,
                            const std::string &name,
                            std::size_t start = 0) {
    const auto value = option_value(args, name, start);
    if (!value || value->empty()) throw std::runtime_error("missing required option " + name);
    return *value;
}

void print_help() {
    std::cout <<
        "multica-core " << kVersion << "\n\n"
        "Usage:\n"
        "  multica-core init [--data-dir DIR]\n"
        "  multica-core agent add ID --name NAME --exec PATH [--arg ARG]... [--role TEXT] [--skill PATH]...\n"
        "  multica-core agent list\n"
        "  multica-core squad create ID --name NAME --leader AGENT_ID\n"
        "  multica-core squad member-add SQUAD_ID AGENT_ID [--role TEXT]\n"
        "  multica-core squad show ID\n"
        "  multica-core issue create --title TEXT --description TEXT --assignee agent:ID|squad:ID [--cwd DIR] [--review-policy auto|manual] [--timeout-seconds N] [--max-retries N]\n"
        "  multica-core issue list\n"
        "  multica-core issue show ID\n"
        "  multica-core issue status ID STATUS\n"
        "  multica-core run ISSUE_ID [--max-runs N]\n"
        "  multica-core data export FILE\n"
        "  multica-core data import FILE\n"
        "  multica-core serve [--port 30420] [--web-root DIR]\n"
        "  multica-core version\n\n"
        "Global option --data-dir DIR may appear anywhere.\n";
}

void list_entities(const fs::path &home, const std::string &kind) {
    std::vector<fs::path> paths;
    for (const auto &entry : fs::directory_iterator(home / kind)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    for (const auto &path : paths) {
        const json value = read_json(path);
        std::cout << value.value("id", path.stem().string()) << "\t" << value.value("name", value.value("title", ""));
        if (value.contains("status")) std::cout << "\t" << value.value("status", "");
        std::cout << "\n";
    }
}

int command_agent(const fs::path &home, const std::vector<std::string> &args) {
    if (args.size() >= 3 && args[1] == "agent" && args[2] == "list") {
        list_entities(home, "agents");
        return 0;
    }
    if (args.size() < 4 || args[1] != "agent" || args[2] != "add") return -1;
    const std::string id = args[3];
    const fs::path path = entity_path(home, "agents", id);
    if (fs::exists(path)) throw std::runtime_error("agent already exists: " + id);
    const std::string executable = required_option(args, "--exec", 4);
    json command = json::array({executable});
    for (const auto &part : repeated_option(args, "--arg", 4)) command.push_back(part);
    json skills = json::array();
    for (const auto &skill : repeated_option(args, "--skill", 4)) skills.push_back(skill);
    const json agent{
        {"id", id},
        {"name", option_value(args, "--name", 4).value_or(id)},
        {"role", option_value(args, "--role", 4).value_or("")},
        {"command", command},
        {"skills", skills},
        {"enabled", true},
        {"created_at", now_utc()},
    };
    save_entity(home, "agents", id, agent);
    std::cout << id << "\n";
    return 0;
}

int command_squad(const fs::path &home, const std::vector<std::string> &args) {
    if (args.size() >= 4 && args[1] == "squad" && args[2] == "show") {
        std::cout << load_entity(home, "squads", args[3]).dump(2) << "\n";
        return 0;
    }
    if (args.size() >= 4 && args[1] == "squad" && args[2] == "create") {
        const std::string id = args[3];
        if (fs::exists(entity_path(home, "squads", id))) throw std::runtime_error("squad already exists: " + id);
        const std::string leader = required_option(args, "--leader", 4);
        load_entity(home, "agents", leader);
        const json squad{
            {"id", id},
            {"name", option_value(args, "--name", 4).value_or(id)},
            {"leader_id", leader},
            {"members", json::array()},
            {"created_at", now_utc()},
        };
        save_entity(home, "squads", id, squad);
        std::cout << id << "\n";
        return 0;
    }
    if (args.size() >= 5 && args[1] == "squad" && args[2] == "member-add") {
        json squad = load_entity(home, "squads", args[3]);
        const std::string agent_id = args[4];
        const json agent = load_entity(home, "agents", agent_id);
        for (const auto &member : squad["members"]) {
            if (member.value("agent_id", "") == agent_id) throw std::runtime_error("agent is already a squad member");
        }
        squad["members"].push_back(json{
            {"agent_id", agent_id},
            {"role", option_value(args, "--role", 5).value_or(agent.value("role", ""))},
        });
        save_entity(home, "squads", args[3], squad);
        std::cout << agent_id << "\n";
        return 0;
    }
    return -1;
}

int command_issue(const fs::path &home, const std::vector<std::string> &args) {
    if (args.size() >= 3 && args[1] == "issue" && args[2] == "list") {
        list_entities(home, "issues");
        return 0;
    }
    if (args.size() >= 4 && args[1] == "issue" && args[2] == "show") {
        std::cout << load_entity(home, "issues", args[3]).dump(2) << "\n";
        return 0;
    }
    if (args.size() >= 5 && args[1] == "issue" && args[2] == "status") {
        static const std::set<std::string> allowed{"todo", "queued", "in_progress", "in_review", "done", "failed", "cancelled"};
        if (!allowed.count(args[4])) throw std::runtime_error("invalid issue status: " + args[4]);
        json issue = load_entity(home, "issues", args[3]);
        issue["status"] = args[4];
        issue["updated_at"] = now_utc();
        save_entity(home, "issues", args[3], issue);
        return 0;
    }
    if (args.size() >= 3 && args[1] == "issue" && args[2] == "create") {
        const std::string assignee = required_option(args, "--assignee", 3);
        const auto separator = assignee.find(':');
        if (separator == std::string::npos) throw std::runtime_error("assignee must be agent:ID or squad:ID");
        const std::string type = assignee.substr(0, separator);
        const std::string id = assignee.substr(separator + 1);
        if (type != "agent" && type != "squad") throw std::runtime_error("assignee type must be agent or squad");
        validate_assignee_ready(home, type, id);
        const std::string issue_id = next_id(home, "issue", "issue-");
        fs::path cwd = option_value(args, "--cwd", 3).value_or(fs::current_path().string());
        cwd = fs::absolute(cwd);
        if (!fs::is_directory(cwd)) throw std::runtime_error("cwd is not a directory: " + cwd.string());
        const int timeout_seconds = std::stoi(option_value(args, "--timeout-seconds", 3).value_or("900"));
        const int max_retries = std::stoi(option_value(args, "--max-retries", 3).value_or("0"));
        if (timeout_seconds < 1 || timeout_seconds > 86400) throw std::runtime_error("--timeout-seconds must be between 1 and 86400");
        if (max_retries < 0 || max_retries > 10) throw std::runtime_error("--max-retries must be between 0 and 10");
        const json issue{
            {"id", issue_id},
            {"title", required_option(args, "--title", 3)},
            {"description", required_option(args, "--description", 3)},
            {"status", "todo"},
            {"assignee_type", type},
            {"assignee_id", id},
            {"cwd", cwd.string()},
            {"review_policy", review_policy(json{{"review_policy", option_value(args, "--review-policy", 3).value_or("auto")}})},
            {"timeout_seconds", timeout_seconds},
            {"max_retries", max_retries},
            {"comments", json::array()},
            {"created_at", now_utc()},
            {"updated_at", now_utc()},
        };
        save_entity(home, "issues", issue_id, issue);
        std::cout << issue_id << "\n";
        return 0;
    }
    return -1;
}

json parse_request_json(const httplib::Request &request) {
    if (request.body.empty()) return json::object();
    try {
        json value = json::parse(request.body);
        if (!value.is_object()) throw std::runtime_error("request body must be a JSON object");
        return value;
    } catch (const json::exception &error) {
        throw std::runtime_error(std::string("invalid request JSON: ") + error.what());
    }
}

std::string required_string(const json &value, const std::string &key) {
    if (!value.contains(key) || !value[key].is_string() || trim(value[key].get<std::string>()).empty()) {
        throw std::runtime_error(key + " is required");
    }
    return trim(value[key].get<std::string>());
}

std::string optional_string(const json &value, const std::string &key, const std::string &fallback = "") {
    if (!value.contains(key) || value[key].is_null()) return fallback;
    if (!value[key].is_string()) throw std::runtime_error(key + " must be a string");
    return trim(value[key].get<std::string>());
}

json string_array(const json &value, const std::string &key) {
    if (!value.contains(key) || value[key].is_null()) return json::array();
    if (!value[key].is_array()) throw std::runtime_error(key + " must be an array");
    json result = json::array();
    for (const auto &entry : value[key]) {
        if (!entry.is_string()) throw std::runtime_error(key + " entries must be strings");
        result.push_back(entry.get<std::string>());
    }
    return result;
}

bool optional_bool(const json &value, const std::string &key, bool fallback) {
    if (!value.contains(key) || value[key].is_null()) return fallback;
    if (!value[key].is_boolean()) throw std::runtime_error(key + " must be a boolean");
    return value[key].get<bool>();
}

int optional_integer(const json &value, const std::string &key, int fallback, int minimum, int maximum) {
    if (!value.contains(key) || value[key].is_null()) return fallback;
    if (!value[key].is_number_integer()) throw std::runtime_error(key + " must be an integer");
    const int result = value[key].get<int>();
    if (result < minimum || result > maximum) {
        throw std::runtime_error(key + " must be between " + std::to_string(minimum) + " and " + std::to_string(maximum));
    }
    return result;
}

std::string review_policy(const json &value, const std::string &fallback) {
    const std::string policy = optional_string(value, "review_policy", fallback);
    if (policy != "auto" && policy != "manual") {
        throw std::runtime_error("review_policy must be auto or manual");
    }
    return policy;
}

void migrate_data(const fs::path &home) {
    const fs::path metadata_path = home / "meta.json";
    int schema_version = 0;
    if (fs::exists(metadata_path)) schema_version = read_json(metadata_path).value("schema_version", 0);
    if (schema_version >= 2) return;

    for (auto agent : load_entities(home, "agents")) {
        if (!agent.contains("enabled")) agent["enabled"] = true;
        save_entity(home, "agents", agent.value("id", ""), agent);
    }
    for (auto issue : load_entities(home, "issues")) {
        if (!issue.contains("review_policy")) issue["review_policy"] = "auto";
        if (!issue.contains("timeout_seconds")) issue["timeout_seconds"] = 900;
        if (!issue.contains("max_retries")) issue["max_retries"] = 0;
        save_entity(home, "issues", issue.value("id", ""), issue);
    }
    write_json(metadata_path, json{{"schema_version", 2}, {"updated_at", now_utc()}});
}

void recover_interrupted_runs(const fs::path &home) {
    for (auto run : load_entities(home, "runs")) {
        if (run.value("status", "") != "running") continue;
        run["status"] = "failed";
        run["failure_reason"] = "service_restarted";
        run["finished_at"] = now_utc();
        save_entity(home, "runs", run.value("id", ""), run);
    }
    for (auto issue : load_entities(home, "issues")) {
        const std::string status = issue.value("status", "todo");
        if (status != "queued" && status != "in_progress") continue;
        issue["status"] = "failed";
        issue["last_error"] = "service restarted while the task was running";
        issue["updated_at"] = now_utc();
        save_entity(home, "issues", issue.value("id", ""), issue);
    }
}

json export_bundle(const fs::path &home) {
    return json{
        {"format", "multica-mini-backup"},
        {"format_version", 1},
        {"exported_at", now_utc()},
        {"agents", load_entities(home, "agents")},
        {"squads", load_entities(home, "squads")},
        {"issues", load_entities(home, "issues")},
        {"runs", load_entities(home, "runs")},
        {"sequence", read_json(home / "sequence.json")},
    };
}

void import_bundle(const fs::path &home, const json &bundle) {
    if (bundle.value("format", "") != "multica-mini-backup" || bundle.value("format_version", 0) != 1) {
        throw std::runtime_error("unsupported backup format");
    }
    static const std::vector<std::string> kinds{"agents", "squads", "issues", "runs"};
    for (const auto &kind : kinds) {
        if (!bundle.contains(kind) || !bundle[kind].is_array()) throw std::runtime_error(kind + " must be an array");
        for (const auto &entry : bundle[kind]) {
            if (!entry.is_object()) throw std::runtime_error(kind + " entries must be objects");
            entity_path(home, kind, required_string(entry, "id"));
        }
    }
    if (!bundle.contains("sequence") || !bundle["sequence"].is_object()) {
        throw std::runtime_error("sequence must be an object");
    }

    const fs::path backup_dir = home / "backups";
    fs::create_directories(backup_dir);
    std::string backup_name = now_utc();
    std::replace(backup_name.begin(), backup_name.end(), ':', '-');
    write_json(backup_dir / ("before-import-" + backup_name + ".json"), export_bundle(home));

    for (const auto &kind : kinds) {
        for (const auto &entry : fs::directory_iterator(home / kind)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") fs::remove(entry.path());
        }
        for (const auto &value : bundle[kind]) {
            save_entity(home, kind, value.value("id", ""), value);
        }
    }
    write_json(home / "sequence.json", bundle["sequence"]);
    write_json(home / "meta.json", json{{"schema_version", 1}, {"imported_at", now_utc()}});
    migrate_data(home);
}

void json_response(httplib::Response &response, const json &value, int status = 200) {
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_content(value.dump(), "application/json; charset=utf-8");
}

template <typename Function>
void api_guard(httplib::Response &response, Function function) {
    try {
        function();
    } catch (const std::exception &error) {
        json_response(response, json{{"error", error.what()}}, 400);
    }
}

fs::path executable_path() {
    std::vector<char> buffer(4096);
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length < 0) return {};
    buffer[static_cast<std::size_t>(length)] = '\0';
    return fs::path(buffer.data());
}

fs::path find_web_root(const std::vector<std::string> &args) {
    std::vector<fs::path> candidates;
    if (const auto configured = option_value(args, "--web-root", 1)) candidates.emplace_back(*configured);
    if (const char *configured = std::getenv("MULTICA_CORE_WEB_ROOT")) {
        if (*configured) candidates.emplace_back(configured);
    }
    const fs::path executable = executable_path();
    if (!executable.empty()) {
        candidates.push_back(executable.parent_path().parent_path() / "web");
        candidates.push_back(executable.parent_path().parent_path() / "share" / "multica-core" / "web");
    }
    candidates.push_back(fs::current_path() / "web");
    candidates.push_back(fs::current_path() / "multica-core-offline" / "web");
    for (const auto &candidate : candidates) {
        std::error_code error;
        const fs::path absolute = fs::absolute(candidate, error);
        if (!error && fs::is_regular_file(absolute / "index.html")) return absolute;
    }
    throw std::runtime_error("Web resources not found; use --web-root DIR or MULTICA_CORE_WEB_ROOT");
}

int run_issue(const fs::path &home, json &issue, int max_runs) {
    const std::string type = issue.value("assignee_type", "");
    validate_assignee_ready(home, type, issue.value("assignee_id", ""));
    if (type == "agent") return run_direct_issue(home, issue, issue.value("assignee_id", ""));
    if (type == "squad") return run_squad_issue(home, issue, issue.value("assignee_id", ""), max_runs);
    throw std::runtime_error("issue has invalid assignee_type");
}

int serve_web(const fs::path &home, const std::vector<std::string> &args) {
    recover_interrupted_runs(home);
    const std::string port_text = option_value(args, "--port", 1).value_or("30420");
    std::size_t parsed = 0;
    const int port = std::stoi(port_text, &parsed);
    if (parsed != port_text.size() || port < 1 || port > 65535) {
        throw std::runtime_error("--port must be between 1 and 65535");
    }
    const fs::path web_root = find_web_root(args);
    httplib::Server server;

    server.Get("/api/health", [](const httplib::Request &, httplib::Response &response) {
        json_response(response, json{{"ok", true}, {"version", kVersion}});
    });

    server.Get("/api/state", [home](const httplib::Request &, httplib::Response &response) {
        api_guard(response, [&] {
            json_response(response, json{
                {"version", kVersion},
                {"data_dir", fs::absolute(home).string()},
                {"agents", load_entities(home, "agents")},
                {"squads", load_entities(home, "squads")},
                {"issues", load_entities(home, "issues")},
                {"runs", load_entities(home, "runs")},
            });
        });
    });

    server.Get("/api/export", [home](const httplib::Request &, httplib::Response &response) {
        api_guard(response, [&] {
            response.status = 200;
            response.set_header("Cache-Control", "no-store");
            response.set_header("Content-Disposition", "attachment; filename=multica-mini-backup.json");
            response.set_content(export_bundle(home).dump(2) + "\n", "application/json; charset=utf-8");
        });
    });

    server.Post("/api/import", [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            {
                std::lock_guard<std::mutex> process_lock(g_process_mutex);
                if (!g_active_issues.empty()) throw std::runtime_error("cannot import data while tasks are running");
            }
            const json bundle = parse_request_json(request);
            std::lock_guard<std::mutex> lock(g_write_mutex);
            import_bundle(home, bundle);
            json_response(response, json{{"imported", true}, {"schema_version", 2}});
        });
    });

    server.Get(R"(/api/agents/([A-Za-z0-9._-]+))",
               [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            json_response(response, load_entity(home, "agents", request.matches[1].str()));
        });
    });

    server.Post("/api/agents", [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            const json body = parse_request_json(request);
            const std::string id = required_string(body, "id");
            std::string name = optional_string(body, "name");
            if (name.empty()) name = id;
            std::lock_guard<std::mutex> lock(g_write_mutex);
            if (fs::exists(entity_path(home, "agents", id))) throw std::runtime_error("agent already exists: " + id);
            const json command = string_array(body, "command");
            if (command.empty() || !command[0].is_string() || trim(command[0].get<std::string>()).empty()) {
                throw std::runtime_error("command must contain an executable");
            }
            const json agent{
                {"id", id},
                {"name", name},
                {"role", optional_string(body, "role")},
                {"command", command},
                {"skills", string_array(body, "skills")},
                {"enabled", optional_bool(body, "enabled", true)},
                {"created_at", now_utc()},
            };
            save_entity(home, "agents", id, agent);
            json_response(response, agent, 201);
        });
    });

    server.Put(R"(/api/agents/([A-Za-z0-9._-]+))",
               [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            const std::string id = request.matches[1].str();
            const json body = parse_request_json(request);
            std::string name = optional_string(body, "name");
            if (name.empty()) name = id;
            const json command = string_array(body, "command");
            if (command.empty() || !command[0].is_string() || trim(command[0].get<std::string>()).empty()) {
                throw std::runtime_error("command must contain an executable");
            }
            std::lock_guard<std::mutex> lock(g_write_mutex);
            json agent = load_entity(home, "agents", id);
            agent["name"] = name;
            agent["role"] = optional_string(body, "role");
            agent["command"] = command;
            agent["skills"] = string_array(body, "skills");
            agent["enabled"] = optional_bool(body, "enabled", agent.value("enabled", true));
            agent["updated_at"] = now_utc();
            save_entity(home, "agents", id, agent);
            json_response(response, agent);
        });
    });

    server.Post(R"(/api/agents/([A-Za-z0-9._-]+)/test)",
                [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            const std::string id = request.matches[1].str();
            const json body = parse_request_json(request);
            const json agent = load_entity(home, "agents", id);
            fs::path cwd = fs::absolute(optional_string(body, "cwd", fs::current_path().string()));
            if (!fs::is_directory(cwd)) throw std::runtime_error("cwd is not a directory: " + cwd.string());
            const std::string test_id = "agent-test-" + id;
            if (!begin_issue_run(test_id)) throw std::runtime_error("agent test is already running: " + id);
            ProcessResult result;
            try {
                const json issue{{"id", test_id}, {"cwd", cwd.string()}};
                const std::string prompt = "Connection test for Multica Mini. Reply exactly: AGENT_READY";
                const auto command = agent_command(agent, issue, prompt);
                result = run_process(command, cwd, home, test_id, id, 60);
            } catch (...) {
                finish_issue_run(test_id);
                throw;
            }
            finish_issue_run(test_id);
            json_response(response, json{{"ok", result.exit_code == 0},
                                         {"exit_code", result.exit_code},
                                         {"output", result.output},
                                         {"timed_out", result.timed_out}});
        });
    });

    server.Delete(R"(/api/agents/([A-Za-z0-9._-]+))",
                  [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            const std::string id = request.matches[1].str();
            std::lock_guard<std::mutex> lock(g_write_mutex);
            load_entity(home, "agents", id);
            for (const auto &squad : load_entities(home, "squads")) {
                if (squad.value("leader_id", "") == id) {
                    throw std::runtime_error("agent is the leader of squad: " + squad.value("id", ""));
                }
                for (const auto &member : squad.value("members", json::array())) {
                    if (member.value("agent_id", "") == id) {
                        throw std::runtime_error("agent is a member of squad: " + squad.value("id", ""));
                    }
                }
            }
            for (const auto &issue : load_entities(home, "issues")) {
                if (issue.value("assignee_type", "") == "agent" && issue.value("assignee_id", "") == id) {
                    throw std::runtime_error("agent is assigned to issue: " + issue.value("id", ""));
                }
            }
            fs::remove(entity_path(home, "agents", id));
            json_response(response, json{{"deleted", id}});
        });
    });

    server.Get(R"(/api/squads/([A-Za-z0-9._-]+))",
               [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            json_response(response, load_entity(home, "squads", request.matches[1].str()));
        });
    });

    server.Post("/api/squads", [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            const json body = parse_request_json(request);
            const std::string id = required_string(body, "id");
            const std::string leader_id = required_string(body, "leader_id");
            std::string name = optional_string(body, "name");
            if (name.empty()) name = id;
            std::lock_guard<std::mutex> lock(g_write_mutex);
            if (fs::exists(entity_path(home, "squads", id))) throw std::runtime_error("squad already exists: " + id);
            load_entity(home, "agents", leader_id);
            json members = json::array();
            if (body.contains("members")) {
                if (!body["members"].is_array()) throw std::runtime_error("members must be an array");
                std::set<std::string> seen;
                for (const auto &entry : body["members"]) {
                    if (!entry.is_object()) throw std::runtime_error("member entries must be objects");
                    const std::string agent_id = required_string(entry, "agent_id");
                    if (agent_id == leader_id) throw std::runtime_error("leader cannot also be a worker member");
                    if (!seen.insert(agent_id).second) throw std::runtime_error("duplicate squad member: " + agent_id);
                    const json agent = load_entity(home, "agents", agent_id);
                    members.push_back(json{{"agent_id", agent_id},
                                           {"role", optional_string(entry, "role", agent.value("role", ""))}});
                }
            }
            const json squad{
                {"id", id},
                {"name", name},
                {"leader_id", leader_id},
                {"members", members},
                {"created_at", now_utc()},
            };
            save_entity(home, "squads", id, squad);
            json_response(response, squad, 201);
        });
    });

    server.Put(R"(/api/squads/([A-Za-z0-9._-]+))",
               [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            const std::string id = request.matches[1].str();
            const json body = parse_request_json(request);
            const std::string leader_id = required_string(body, "leader_id");
            std::string name = optional_string(body, "name");
            if (name.empty()) name = id;
            if (!body.contains("members") || !body["members"].is_array()) {
                throw std::runtime_error("members must be an array");
            }
            std::lock_guard<std::mutex> lock(g_write_mutex);
            json squad = load_entity(home, "squads", id);
            load_entity(home, "agents", leader_id);
            json members = json::array();
            std::set<std::string> seen;
            for (const auto &entry : body["members"]) {
                if (!entry.is_object()) throw std::runtime_error("member entries must be objects");
                const std::string agent_id = required_string(entry, "agent_id");
                if (agent_id == leader_id) throw std::runtime_error("leader cannot also be a worker member");
                if (!seen.insert(agent_id).second) throw std::runtime_error("duplicate squad member: " + agent_id);
                const json agent = load_entity(home, "agents", agent_id);
                members.push_back(json{{"agent_id", agent_id},
                                       {"role", optional_string(entry, "role", agent.value("role", ""))}});
            }
            squad["name"] = name;
            squad["leader_id"] = leader_id;
            squad["members"] = members;
            squad["updated_at"] = now_utc();
            save_entity(home, "squads", id, squad);
            json_response(response, squad);
        });
    });

    server.Delete(R"(/api/squads/([A-Za-z0-9._-]+))",
                  [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            const std::string id = request.matches[1].str();
            std::lock_guard<std::mutex> lock(g_write_mutex);
            load_entity(home, "squads", id);
            for (const auto &issue : load_entities(home, "issues")) {
                if (issue.value("assignee_type", "") == "squad" && issue.value("assignee_id", "") == id) {
                    throw std::runtime_error("squad is assigned to issue: " + issue.value("id", ""));
                }
            }
            fs::remove(entity_path(home, "squads", id));
            json_response(response, json{{"deleted", id}});
        });
    });

    server.Post(R"(/api/squads/([A-Za-z0-9._-]+)/members)",
                [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            const std::string squad_id = request.matches[1].str();
            const json body = parse_request_json(request);
            const std::string agent_id = required_string(body, "agent_id");
            std::lock_guard<std::mutex> lock(g_write_mutex);
            json squad = load_entity(home, "squads", squad_id);
            if (squad.value("leader_id", "") == agent_id) throw std::runtime_error("leader cannot also be a worker member");
            const json agent = load_entity(home, "agents", agent_id);
            for (const auto &member : squad["members"]) {
                if (member.value("agent_id", "") == agent_id) throw std::runtime_error("agent is already a squad member");
            }
            squad["members"].push_back(json{{"agent_id", agent_id},
                                             {"role", optional_string(body, "role", agent.value("role", ""))}});
            save_entity(home, "squads", squad_id, squad);
            json_response(response, squad);
        });
    });

    server.Post("/api/issues", [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            const json body = parse_request_json(request);
            const std::string type = required_string(body, "assignee_type");
            const std::string assignee_id = required_string(body, "assignee_id");
            const std::string title = required_string(body, "title");
            const std::string description = required_string(body, "description");
            if (type != "agent" && type != "squad") throw std::runtime_error("assignee_type must be agent or squad");
            fs::path cwd = fs::absolute(optional_string(body, "cwd", fs::current_path().string()));
            if (!fs::is_directory(cwd)) throw std::runtime_error("cwd is not a directory: " + cwd.string());
            std::lock_guard<std::mutex> lock(g_write_mutex);
            validate_assignee_ready(home, type, assignee_id);
            const std::string issue_id = next_id(home, "issue", "issue-");
            const json issue{
                {"id", issue_id},
                {"title", title},
                {"description", description},
                {"status", "todo"},
                {"assignee_type", type},
                {"assignee_id", assignee_id},
                {"cwd", cwd.string()},
                {"review_policy", review_policy(body)},
                {"timeout_seconds", optional_integer(body, "timeout_seconds", 900, 1, 86400)},
                {"max_retries", optional_integer(body, "max_retries", 0, 0, 10)},
                {"comments", json::array()},
                {"created_at", now_utc()},
                {"updated_at", now_utc()},
            };
            save_entity(home, "issues", issue_id, issue);
            json_response(response, issue, 201);
        });
    });

    server.Put(R"(/api/issues/([A-Za-z0-9._-]+))",
               [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            const std::string id = request.matches[1].str();
            const json body = parse_request_json(request);
            const std::string type = required_string(body, "assignee_type");
            const std::string assignee_id = required_string(body, "assignee_id");
            const std::string title = required_string(body, "title");
            const std::string description = required_string(body, "description");
            if (type != "agent" && type != "squad") throw std::runtime_error("assignee_type must be agent or squad");
            fs::path cwd = fs::absolute(optional_string(body, "cwd", fs::current_path().string()));
            if (!fs::is_directory(cwd)) throw std::runtime_error("cwd is not a directory: " + cwd.string());
            std::lock_guard<std::mutex> lock(g_write_mutex);
            json issue = load_entity(home, "issues", id);
            validate_assignee_ready(home, type, assignee_id);
            issue["title"] = title;
            issue["description"] = description;
            issue["assignee_type"] = type;
            issue["assignee_id"] = assignee_id;
            issue["cwd"] = cwd.string();
            issue["review_policy"] = review_policy(body);
            issue["timeout_seconds"] = optional_integer(body, "timeout_seconds", issue.value("timeout_seconds", 900), 1, 86400);
            issue["max_retries"] = optional_integer(body, "max_retries", issue.value("max_retries", 0), 0, 10);
            issue["updated_at"] = now_utc();
            save_entity(home, "issues", id, issue);
            json_response(response, issue);
        });
    });

    server.Delete(R"(/api/issues/([A-Za-z0-9._-]+))",
                  [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            const std::string id = request.matches[1].str();
            std::lock_guard<std::mutex> lock(g_write_mutex);
            load_entity(home, "issues", id);
            for (const auto &entry : fs::directory_iterator(home / "runs")) {
                if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
                const json run = read_json(entry.path());
                if (run.value("issue_id", "") == id) fs::remove(entry.path());
            }
            fs::remove(entity_path(home, "issues", id));
            json_response(response, json{{"deleted", id}});
        });
    });

    server.Get(R"(/api/issues/([A-Za-z0-9._-]+))",
               [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            const std::string issue_id = request.matches[1].str();
            json issue = load_entity(home, "issues", issue_id);
            json runs = json::array();
            for (const auto &run : load_entities(home, "runs")) {
                if (run.value("issue_id", "") == issue_id) runs.push_back(run);
            }
            issue["runs"] = runs;
            json_response(response, issue);
        });
    });

    server.Post(R"(/api/issues/([A-Za-z0-9._-]+)/status)",
                [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            static const std::set<std::string> allowed{"todo", "queued", "in_progress", "in_review", "done", "failed", "cancelled"};
            const json body = parse_request_json(request);
            const std::string status = required_string(body, "status");
            if (!allowed.count(status)) throw std::runtime_error("invalid issue status: " + status);
            std::lock_guard<std::mutex> lock(g_write_mutex);
            const std::string issue_id = request.matches[1].str();
            json issue = load_entity(home, "issues", issue_id);
            issue["status"] = status;
            issue["updated_at"] = now_utc();
            save_entity(home, "issues", issue_id, issue);
            json_response(response, issue);
        });
    });

    server.Post(R"(/api/issues/([A-Za-z0-9._-]+)/comments)",
                [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            const std::string issue_id = request.matches[1].str();
            const json body = parse_request_json(request);
            const std::string content = required_string(body, "content");
            std::lock_guard<std::mutex> lock(g_write_mutex);
            json issue = load_entity(home, "issues", issue_id);
            issue["comments"].push_back(json{
                {"id", next_id(home, "comment", "comment-")},
                {"author_type", "human"},
                {"author_id", "local-user"},
                {"author_name", "你"},
                {"content", content},
                {"created_at", now_utc()},
            });
            issue["updated_at"] = now_utc();
            save_entity(home, "issues", issue_id, issue);
            json_response(response, issue, 201);
        });
    });

    server.Post(R"(/api/issues/([A-Za-z0-9._-]+)/run)",
                [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            const json body = parse_request_json(request);
            int max_runs = 16;
            if (body.contains("max_runs")) {
                if (!body["max_runs"].is_number_integer()) throw std::runtime_error("max_runs must be an integer");
                max_runs = body["max_runs"].get<int>();
            }
            if (max_runs < 1 || max_runs > 256) throw std::runtime_error("max_runs must be between 1 and 256");
            const std::string issue_id = request.matches[1].str();
            if (!begin_issue_run(issue_id)) throw std::runtime_error("issue is already running: " + issue_id);
            try {
                std::lock_guard<std::mutex> lock(g_write_mutex);
                json issue = load_entity(home, "issues", issue_id);
                issue["status"] = "queued";
                issue["updated_at"] = now_utc();
                save_entity(home, "issues", issue_id, issue);
            } catch (...) {
                finish_issue_run(issue_id);
                throw;
            }
            std::thread([home, issue_id, max_runs] {
                try {
                    std::lock_guard<std::mutex> lock(g_write_mutex);
                    json issue = load_entity(home, "issues", issue_id);
                    if (cancellation_requested(issue_id)) {
                        issue["status"] = "cancelled";
                        issue["last_error"] = "run cancelled by user";
                        issue["updated_at"] = now_utc();
                        save_entity(home, "issues", issue_id, issue);
                    } else {
                        run_issue(home, issue, max_runs);
                    }
                } catch (const std::exception &error) {
                    try {
                        json issue = load_entity(home, "issues", issue_id);
                        issue["status"] = "failed";
                        issue["last_error"] = error.what();
                        issue["updated_at"] = now_utc();
                        save_entity(home, "issues", issue_id, issue);
                    } catch (...) {
                    }
                }
                finish_issue_run(issue_id);
            }).detach();
            json_response(response, json{{"accepted", true}, {"issue_id", issue_id}, {"status", "queued"}}, 202);
        });
    });

    server.Post(R"(/api/issues/([A-Za-z0-9._-]+)/cancel)",
                [home](const httplib::Request &request, httplib::Response &response) {
        api_guard(response, [&] {
            const std::string issue_id = request.matches[1].str();
            load_entity(home, "issues", issue_id);
            if (!request_cancellation(issue_id)) throw std::runtime_error("issue is not running: " + issue_id);
            json_response(response, json{{"cancel_requested", true}, {"issue_id", issue_id}});
        });
    });

    server.set_file_extension_and_mimetype_mapping("css", "text/css; charset=utf-8");
    server.set_file_extension_and_mimetype_mapping("js", "application/javascript; charset=utf-8");
    if (!server.set_mount_point("/", web_root.string())) {
        throw std::runtime_error("cannot serve Web resources from " + web_root.string());
    }

    std::cout << "Multica Core Offline " << kVersion << "\n"
              << "Web:  http://127.0.0.1:" << port << "\n"
              << "Data: " << home << "\n";
    if (!server.listen("127.0.0.1", port)) throw std::runtime_error("cannot listen on port " + std::to_string(port));
    return 0;
}

int run_command(const fs::path &home, const std::vector<std::string> &args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        print_help();
        return 0;
    }
    if (args[0] == "version" || args[0] == "--version") {
        std::cout << kVersion << "\n";
        return 0;
    }
    ensure_home(home);
    migrate_data(home);
    if (args[0] == "init") {
        std::cout << home << "\n";
        return 0;
    }
    if (args[0] == "serve") return serve_web(home, args);
    if (args[0] == "data") {
        if (args.size() != 3 || (args[1] != "export" && args[1] != "import")) {
            throw std::runtime_error("usage: multica-core data export|import FILE");
        }
        const fs::path path = fs::absolute(args[2]);
        if (args[1] == "export") {
            write_json(path, export_bundle(home));
            std::cout << path << "\n";
            return 0;
        }
        import_bundle(home, read_json(path));
        std::cout << path << "\n";
        return 0;
    }
    if (args[0] == "run") {
        if (args.size() < 2) throw std::runtime_error("run requires ISSUE_ID");
        json issue = load_entity(home, "issues", args[1]);
        const int max_runs = std::stoi(option_value(args, "--max-runs", 2).value_or("16"));
        if (max_runs < 1 || max_runs > 256) throw std::runtime_error("--max-runs must be between 1 and 256");
        return run_issue(home, issue, max_runs);
    }
    std::vector<std::string> routed;
    routed.reserve(args.size() + 1);
    routed.emplace_back();
    routed.insert(routed.end(), args.begin(), args.end());
    if (const int result = command_agent(home, routed); result >= 0) return result;
    if (const int result = command_squad(home, routed); result >= 0) return result;
    if (const int result = command_issue(home, routed); result >= 0) return result;
    throw std::runtime_error("unknown command; run multica-core --help");
}

} // namespace

int main(int argc, char **argv) {
    try {
        fs::path home = default_home();
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) {
            const std::string value = argv[i];
            if (value == "--data-dir") {
                if (i + 1 >= argc) throw std::runtime_error("--data-dir requires a value");
                home = fs::absolute(argv[++i]);
                continue;
            }
            args.push_back(value);
        }
        return run_command(home, args);
    } catch (const std::exception &error) {
        std::cerr << "multica-core: " << error.what() << "\n";
        return 1;
    }
}
