# Multica Core Offline

这是从 Multica 协作模型裁剪出的本地核心运行时，面向 CentOS 7、RHEL 8 和其他
x86_64 Linux。它保留 Issue、Agent、Squad Leader、mention 派发、运行记录、Skill
注入和本地 Web 管理界面，不包含 PostgreSQL、Next.js、Node.js 或云端服务。

本项目基于 Multica（https://github.com/multica-ai/multica）。分发包携带 Multica 的
完整许可证与 NOTICE。

架构、调度语义和 API 说明见 [设计与使用说明](docs/DESIGN.zh-CN.md)。

## 直接运行 Web

离线包解压后即可启动：

```sh
tar -xzf multica-core-offline-0.1.0-linux-x86_64.tar.gz
cd multica-core-offline-0.1.0-linux-x86_64
./start.sh
```

然后打开 `http://127.0.0.1:30420`。服务固定监听本机回环地址，不接受远程连接。
页面可以登记、查看和编辑 Agent，建立 Squad、添加 Worker、创建和执行 Issue，并查看
Agent 评论与 Run 记录。界面保持 Multica 原作的浅色看板风格，并支持亮色、暗色和跟随系统三种主题。
主题选择只保存在浏览器本地，不会写入 skill。页面、API 和协作运行时由同一个二进制提供。`start.sh` 默认将数据保存在
解压目录的 `data/`；可以传入 `--data-dir DIR` 和 `--port PORT` 覆盖。

也可以安装到指定目录：

```sh
./install.sh --prefix "$HOME/.local" --data-dir "$HOME/.local/share/multica-core"
"$HOME/.local/bin/multica-core" serve
```

## CLI 快速开始

```sh
./multica-core init --data-dir ./data

./multica-core agent add leader \
  --name Leader \
  --exec /path/to/agent-cli \
  --arg run --arg '{prompt}'

./multica-core agent add worker \
  --name Worker \
  --exec /path/to/agent-cli \
  --arg run --arg '{prompt}' \
  --role '负责实现和验证'

./multica-core squad create team --name Team --leader leader
./multica-core squad member-add team worker --role '负责实现和验证'

issue_id=$(./multica-core issue create \
  --data-dir ./data \
  --title '检查工程并修复问题' \
  --description '完成修改并报告验证结果' \
  --assignee squad:team \
  --cwd /path/to/project)

./multica-core run "$issue_id" --data-dir ./data
./multica-core issue show "$issue_id" --data-dir ./data
```

Agent 命令以 argv 直接执行，不经过 shell。支持 `{prompt}`、`{cwd}`、`{issue_id}` 和
`{agent_id}` 占位符。没有 `{prompt}` 时，提示会自动追加到 argv 末尾。

Leader 必须用下面的格式派发成员：

```text
[@Worker](mention://agent/worker)
```

## 运行依赖

`multica-core` 是静态 x86_64 Linux 二进制，Web 页面也是本地静态资源，不依赖
glibc、Docker、Node.js、Python、Go 或 PostgreSQL。实际执行任务仍需要配置好的
Agent CLI；Agent CLI 自身的系统依赖不由本程序提供。

目标系统为 CentOS 7、RHEL 8 及兼容的 x86_64 Linux。运行需要 Linux 内核提供常规的
进程、文件系统和 TCP socket 功能，并需要一个浏览器访问本地页面。离线包本身不执行
在线请求，也不使用 CDN。

## 本地 API

Web 使用以下 JSON API：

```text
GET  /api/health
GET  /api/state
POST /api/agents
POST /api/squads
POST /api/squads/:id/members
POST /api/issues
GET  /api/issues/:id
POST /api/issues/:id/run
POST /api/issues/:id/status
```

首版 API 只适合单机使用。`run` 请求会保持连接，直到本轮 Agent 协作完成；页面在此
期间仍会刷新运行状态。JSON 数据使用临时文件加原子替换写入
`--data-dir`，服务内的写操作串行执行。

## 构建、测试与打包

仓库根目录已经带有 musl C++ 工具链及所需头文件：

```sh
./scripts/build.sh
./tests/smoke.sh
./tests/web-smoke.sh
./scripts/package.sh
```

产物位于 `dist/`，并附有 SHA-256 校验文件。最终包包含完整的 Multica
`LICENSE`、`NOTICE` 和第三方许可证。
