# 使用本地 Pi 完成第一个任务

本示例用两个 Pi 配置组成 `Leader → Worker → Leader` 小队。Pi 需要提前安装并配置好可用
模型；`--offline` 只禁止 Pi 的启动联网检查，是否访问模型服务取决于 Pi 的 provider 配置。

## 1. 找到 Pi

```sh
command -v pi
pi --version
```

记下第一条命令返回的绝对路径。下面用 `/absolute/path/to/pi` 表示该路径。

## 2. 在 Web 中登记 Worker

打开 Agent 页面，创建：

```text
ID: pi-worker
名称: Pi Worker
职责: 读取任务目录、执行任务并报告可验证结果
命令（每行一个参数）:
/absolute/path/to/pi
--print
--no-session
--no-skills
--no-context-files
--tools
read
--offline
{prompt}
```

保存后重新打开该 Agent，点击“测试连接”。成功退出并出现回复即可；回复不必逐字等于
`AGENT_READY`。若任务需要修改文件，把 `--tools` 后的 `read` 改成适合本地策略的工具
列表。

## 3. 登记 Leader

创建第二个 Agent：

```text
ID: pi-leader
名称: Pi Leader
职责: 拆分任务、派发成员并根据成员证据给出最终结论
命令（每行一个参数）:
/absolute/path/to/pi
--print
--no-session
--no-skills
--no-context-files
--no-tools
--offline
{prompt}
```

Leader 只需要分析和派发时可以禁用工具。它必须能原样输出如下 mention：

```text
[@Pi Worker](mention://agent/pi-worker)
```

## 4. 创建 Squad

在 Squad 页面创建 `pi-demo-team`：

- Leader 选择 `pi-leader`；
- 添加 `pi-worker`；
- 成员职责写明要读取或验证的内容。

点击 Squad 卡片可以再次编辑名称、Leader、成员和职责。

## 5. 创建并执行 Issue

准备一个工作目录和测试文件：

```sh
mkdir -p /tmp/multica-pi-demo
printf 'release=ready\nchecks=3\n' >/tmp/multica-pi-demo/release-check.txt
```

在 Tasks 页面创建：

```text
标题: 核对发布文件
说明: 请让成员读取 release-check.txt，报告 release 和 checks 的值，然后给出最终结论。
负责人: pi-demo-team
工作目录: /tmp/multica-pi-demo
完成策略: 自动完成
```

打开 Issue 并点击运行。正常状态会依次变为 `queued`、`in_progress`、`done`。评论时间线
应包含 Leader 的派发、Worker 的文件证据以及 Leader 的最终结论；Run 区域应有三次调用。

如果任务停在 `in_review`，编辑 Issue 并把完成策略改成“自动完成”。如果显示 `failed`，
展开最后一个 Run 查看命令、退出码、失败原因和合并输出。

## CLI 等价配置

```sh
PI_BIN=/absolute/path/to/pi
DATA_DIR=./data

./bin/multica-core agent add pi-worker --data-dir "$DATA_DIR" \
  --name 'Pi Worker' --role '读取任务目录并报告证据' \
  --exec "$PI_BIN" --arg --print --arg --no-session --arg --no-skills \
  --arg --no-context-files --arg --tools --arg read --arg --offline --arg '{prompt}'

./bin/multica-core agent add pi-leader --data-dir "$DATA_DIR" \
  --name 'Pi Leader' --role '拆分任务并给出最终结论' \
  --exec "$PI_BIN" --arg --print --arg --no-session --arg --no-skills \
  --arg --no-context-files --arg --no-tools --arg --offline --arg '{prompt}'

./bin/multica-core squad create pi-demo-team --data-dir "$DATA_DIR" \
  --name 'Pi 本地验证小队' --leader pi-leader
./bin/multica-core squad member-add pi-demo-team pi-worker --data-dir "$DATA_DIR" \
  --role '读取 release-check.txt，核对字段并报告证据'
```

若 Pi 的参数或 provider 不同，只修改 Agent 命令即可，Multica Mini 不绑定 Pi 的具体版本。
