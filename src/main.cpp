#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "httplib.h"
#include "json.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr const char *kVersion = "0.1.0";
constexpr std::size_t kMaxCapturedOutput = 4U * 1024U * 1024U;
std::mutex g_write_mutex;

struct ProcessResult {
    int exit_code = 127;
    std::string output;
};

struct Dispatch {
    std::string agent_id;
    std::string reason;
};

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
                          const std::string &agent_id) {
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
    std::string output;
    char buffer[8192];
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

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) throw std::runtime_error("waitpid failed: " + std::string(std::strerror(errno)));
    }
    ProcessResult result;
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

        const ProcessResult result = run_process(command, cwd, home, issue.value("id", ""), agent_id);
        run["status"] = result.exit_code == 0 ? "completed" : "failed";
        run["exit_code"] = result.exit_code;
        run["output"] = result.output;
        run["finished_at"] = now_utc();
        save_entity(home, "runs", run_id, run);

        const std::string comment = result.output.empty()
                                        ? "Agent process exited with code " + std::to_string(result.exit_code) + " without output."
                                        : result.output;
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

int run_direct_issue(const fs::path &home, json &issue, const std::string &agent_id) {
    issue["status"] = "in_progress";
    issue["updated_at"] = now_utc();
    save_entity(home, "issues", issue.value("id", ""), issue);
    const ProcessResult result = execute_agent(home, issue, agent_id, std::nullopt, false, "issue assignment");
    issue["status"] = result.exit_code == 0 ? "in_review" : "todo";
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
    issue["updated_at"] = now_utc();
    save_entity(home, "issues", issue.value("id", ""), issue);

    std::queue<Dispatch> pending;
    pending.push({leader_id, "squad assignment"});
    bool leader_delegated = false;

    for (int iteration = 0; iteration < max_runs && !pending.empty(); ++iteration) {
        const Dispatch dispatch = pending.front();
        pending.pop();
        const bool is_leader = dispatch.agent_id == leader_id;
        const ProcessResult result = execute_agent(home, issue, dispatch.agent_id, squad, is_leader, dispatch.reason);
        if (result.exit_code != 0) {
            issue["status"] = "todo";
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
                    issue["status"] = "todo";
                    issue["updated_at"] = now_utc();
                    save_entity(home, "issues", issue.value("id", ""), issue);
                    std::cerr << "leader did not delegate to a squad member\n";
                    return 1;
                }
                issue["status"] = "in_review";
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

    issue["status"] = "todo";
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
        "  multica-core issue create --title TEXT --description TEXT --assignee agent:ID|squad:ID [--cwd DIR]\n"
        "  multica-core issue list\n"
        "  multica-core issue show ID\n"
        "  multica-core issue status ID STATUS\n"
        "  multica-core run ISSUE_ID [--max-runs N]\n"
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
        static const std::set<std::string> allowed{"todo", "in_progress", "in_review", "done"};
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
        load_entity(home, type == "agent" ? "agents" : "squads", id);
        const std::string issue_id = next_id(home, "issue", "issue-");
        fs::path cwd = option_value(args, "--cwd", 3).value_or(fs::current_path().string());
        cwd = fs::absolute(cwd);
        if (!fs::is_directory(cwd)) throw std::runtime_error("cwd is not a directory: " + cwd.string());
        const json issue{
            {"id", issue_id},
            {"title", required_option(args, "--title", 3)},
            {"description", required_option(args, "--description", 3)},
            {"status", "todo"},
            {"assignee_type", type},
            {"assignee_id", id},
            {"cwd", cwd.string()},
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
    if (type == "agent") return run_direct_issue(home, issue, issue.value("assignee_id", ""));
    if (type == "squad") return run_squad_issue(home, issue, issue.value("assignee_id", ""), max_runs);
    throw std::runtime_error("issue has invalid assignee_type");
}

int serve_web(const fs::path &home, const std::vector<std::string> &args) {
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
                {"agents", load_entities(home, "agents")},
                {"squads", load_entities(home, "squads")},
                {"issues", load_entities(home, "issues")},
                {"runs", load_entities(home, "runs")},
            });
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
                {"created_at", now_utc()},
            };
            save_entity(home, "agents", id, agent);
            json_response(response, agent, 201);
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
            load_entity(home, type == "agent" ? "agents" : "squads", assignee_id);
            const std::string issue_id = next_id(home, "issue", "issue-");
            const json issue{
                {"id", issue_id},
                {"title", title},
                {"description", description},
                {"status", "todo"},
                {"assignee_type", type},
                {"assignee_id", assignee_id},
                {"cwd", cwd.string()},
                {"comments", json::array()},
                {"created_at", now_utc()},
                {"updated_at", now_utc()},
            };
            save_entity(home, "issues", issue_id, issue);
            json_response(response, issue, 201);
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
            static const std::set<std::string> allowed{"todo", "in_progress", "in_review", "done"};
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
            std::lock_guard<std::mutex> lock(g_write_mutex);
            const std::string issue_id = request.matches[1].str();
            json issue = load_entity(home, "issues", issue_id);
            const int exit_code = run_issue(home, issue, max_runs);
            json_response(response, json{{"exit_code", exit_code},
                                         {"issue", load_entity(home, "issues", issue_id)}});
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
    if (args[0] == "init") {
        std::cout << home << "\n";
        return 0;
    }
    if (args[0] == "serve") return serve_web(home, args);
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
