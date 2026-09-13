# 故障排查

先运行服务状态和健康检查：

```sh
./service.sh status
curl -fsS http://127.0.0.1:30420/api/health
```

后台服务日志默认在 `<data-dir>/service.log`。任务问题应先打开 Issue，再展开最后一个 Run
查看失败原因、退出码、命令摘要、提示快照和合并输出。

| 现象 | 常见原因 | 处理方法 |
| --- | --- | --- |
| `cannot listen on port` | 端口已被其他进程占用 | 停掉旧服务，或用 `--port` 换端口 |
| `command not found` / 退出码 127 | Agent 可执行文件路径错误 | 在终端运行该绝对路径；编辑 Agent 后先保存再测试 |
| `Permission denied` / 退出码 126 | Agent 文件没有执行权限 | 检查文件权限、挂载参数和父目录权限 |
| `cwd is not a directory` | Issue 工作目录不存在 | 创建目录或编辑 Issue 为现有绝对路径 |
| `skill file is not readable` | skill 路径错误或权限不足 | 修正 Agent 的 skill 路径；不需要时移除该项 |
| Agent 测试返回非零 | CLI 参数、provider 或登录状态异常 | 复制 Agent 命令到终端验证，检查模型配置和凭据 |
| `timed out` | 模型响应慢、CLI 卡死或超时过短 | 增大 Issue 超时；确认 Agent CLI 能独立退出 |
| `cancelled by user` | 用户取消了运行 | 确认没有残留子进程后重新运行 Issue |
| `agent is disabled` | 负责人或 Squad 成员被停用 | 在 Agent 编辑器中启用并保存 |
| `squad has no members` | Squad 只有 Leader | 编辑 Squad，加入至少一个启用的 Worker |
| mention 没有触发 Worker | 格式或 ID 不匹配 | 使用 `[@名称](mention://agent/精确ID)`，并确认 ID 在 Squad 花名册中 |
| Issue 一直要求 review | 完成策略设为 `manual` | 编辑为“自动完成”，或在验收后把状态改为 Done |
| 服务重启后任务为 Failed | 服务退出时任务仍在运行 | 查看 `last_error`，确认环境后重新运行；这是恢复机制的预期行为 |
| 导入被拒绝 | 有任务正在运行，或文件不是有效备份 | 等任务结束后重试；先用“导出”生成格式样例 |

## 服务无法启动

直接前台运行可以立即看到错误：

```sh
./bin/multica-core serve --data-dir ./data --port 30420
```

发行二进制本身是静态 x86_64 ELF。如果出现 `Exec format error`，检查机器架构：

```sh
uname -m
file ./bin/multica-core
```

本包要求 Linux x86_64。CentOS 7 和 RHEL 8 不需要额外安装 glibc、Node.js、Python、Go
或数据库。浏览器可以从同一台机器访问 `127.0.0.1`；若服务器没有桌面，可使用 SSH
端口转发：

```sh
ssh -L 30420:127.0.0.1:30420 user@server
```

然后在本机浏览器访问 `http://127.0.0.1:30420`。

## 数据恢复

Web 导入前的自动备份位于：

```text
<data-dir>/backups/
```

也可以先手工导出：

```sh
./bin/multica-core data export rescue.json --data-dir ./data
```

不要同时启动两个服务写同一个数据目录。卸载脚本默认保留数据，只有明确传入
`--remove-data` 才会删除数据目录。
