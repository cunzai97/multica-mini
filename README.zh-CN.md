# Multica Mini 离线核心版

这是从 [Multica](https://github.com/multica-ai/multica) 协作模型中裁剪出的单机产品。
它保留 Agent、Squad Leader、Issue、mention 派发、运行记录和本地 Web 工作台，并补齐
编辑、删除、连通性测试、失败恢复、备份和服务脚本。它不需要 PostgreSQL、Node.js、
Docker 或云端服务。

本项目面向 CentOS 7、RHEL 8 和其他 x86_64 Linux。发行二进制使用 musl 静态链接，
Web 资源全部随包提供。被配置的 Agent CLI 及其模型连接仍由使用者准备。

## 解压即用

```sh
tar -xzf multica-core-offline-0.2.0-linux-x86_64.tar.gz
cd multica-core-offline-0.2.0-linux-x86_64
./start.sh
```

打开 `http://127.0.0.1:30420`。服务只监听本机回环地址。`start.sh` 默认把数据写入
解压目录的 `data/`；也可以指定目录和端口：

```sh
./start.sh --data-dir /srv/multica-mini/data --port 30420
```

需要后台常驻时使用：

```sh
./service.sh start
./service.sh status
./service.sh stop
```

Web 工作台可以完成以下操作：

- 创建、查看、编辑、启用或停用、测试和安全删除 Agent；
- 创建和完整编辑 Squad，包括 Leader、成员及成员职责；
- 创建、搜索、编辑、复制、运行、取消和删除 Issue，并添加人工评论；
- 查看每次 Run 的状态、退出码、失败原因、输出、命令与提示快照；
- 为任务设置超时、失败重试和 `auto` / `manual` 完成策略；
- 导出完整数据，或在自动备份后导入数据；
- 切换亮色、暗色和跟随系统主题。主题只保存在浏览器，不进入 skill。

正常任务默认使用 `auto`，成功后直接进入 `done`。只有显式选择 `manual` 的任务才会
进入 `in_review` 等待人工验收。

## 配置 Agent

Agent 的命令是 argv 数组，不经过 shell。支持以下占位符：

| 占位符 | 替换内容 |
| --- | --- |
| `{prompt}` | 当前任务、评论、小队花名册、职责和 skill 内容组成的提示 |
| `{cwd}` | Issue 工作目录 |
| `{issue_id}` | Issue ID |
| `{agent_id}` | 当前 Agent ID |

没有 `{prompt}` 时，程序会把提示自动追加为最后一个参数。Web 中修改命令后需要先保存，
再点“测试连接”；测试会使用已保存配置，并要求 Agent 对最小提示返回结果。

以本地 Pi 为例，命令可按实际安装方式填写为：

```text
/path/to/pi
-p
{prompt}
```

具体参数以本机 `pi --help` 为准。完整示例见
[Pi 快速开始](docs/PI-QUICKSTART.zh-CN.md)。

## Squad 工作方式

Squad 的 Leader 先读取任务，然后用标准 mention 派发花名册中的 Worker：

```text
[@Worker](mention://agent/worker-id)
```

程序按 mention 顺序串行运行 Worker，把结果写回同一 Issue，再唤醒 Leader。Leader 不再
派发时，其最终回复成为小队结论。该过程不会要求用户逐轮 review。

## CLI 快速开始

```sh
./bin/multica-core init --data-dir ./data

./bin/multica-core agent add leader \
  --data-dir ./data --name Leader \
  --exec /path/to/agent-cli --arg run --arg '{prompt}'

./bin/multica-core agent add worker \
  --data-dir ./data --name Worker --role '负责实现和验证' \
  --exec /path/to/agent-cli --arg run --arg '{prompt}'

./bin/multica-core squad create team \
  --data-dir ./data --name Team --leader leader
./bin/multica-core squad member-add team worker \
  --data-dir ./data --role '负责实现和验证'

issue_id=$(./bin/multica-core issue create \
  --data-dir ./data \
  --title '检查工程并修复问题' \
  --description '完成修改并报告验证结果' \
  --assignee squad:team \
  --cwd /path/to/project)

./bin/multica-core run "$issue_id" --data-dir ./data
./bin/multica-core issue show "$issue_id" --data-dir ./data
```

查看所有命令：

```sh
./bin/multica-core --help
```

## 数据和安装

Web 的“数据”页面可以下载与导入 JSON 备份。CLI 对应命令为：

```sh
./bin/multica-core data export backup.json --data-dir ./data
./bin/multica-core data import backup.json --data-dir ./data
```

导入前会在 `<data-dir>/backups/` 自动保存当前数据。服务启动时会迁移旧 schema，并把
上次异常退出遗留的运行中任务标记为失败，避免界面永久卡在运行中。

安装到用户目录：

```sh
./install.sh --prefix "$HOME/.local" \
  --data-dir "$HOME/.local/share/multica-core"
"$HOME/.local/bin/multica-core-service" start \
  --data-dir "$HOME/.local/share/multica-core"
```

卸载默认保留数据：

```sh
"$HOME/.local/bin/multica-core-uninstall" \
  --prefix "$HOME/.local" \
  --data-dir "$HOME/.local/share/multica-core"
```

只有传入 `--remove-data` 才会同时删除数据目录。

## 系统依赖

运行 Multica Mini 本体需要：

- x86_64 Linux 内核提供进程、文件系统和 TCP socket；
- 一个能访问本机地址的现代浏览器；
- 可选的 `curl`，仅用于后台启动脚本的就绪检查，没有它也能启动。

它不依赖宿主机 glibc、Node.js、Python、Go、Docker、数据库或网络 CDN。实际执行任务
需要配置好的 Agent CLI；Agent CLI 自身的运行库、账号或网络要求不包含在本离线包中。

## 开发与验证

```sh
./scripts/build.sh
./tests/smoke.sh
./tests/web-smoke.sh
./tests/reliability-smoke.sh
./scripts/package.sh
```

产物位于 `dist/`，并附带 SHA-256 校验文件。详细架构和 API 见
[设计说明](docs/DESIGN.zh-CN.md)，常见错误见
[故障排查](docs/TROUBLESHOOTING.zh-CN.md)。发行包携带 Multica 的完整 `LICENSE`、
`NOTICE` 和第三方许可证。
