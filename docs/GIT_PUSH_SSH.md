# Git 推送约定：统一走 SSH

> 本文件记录本项目的 Git 远程推送约定。属于开发工作流约定，不写进 README（README 只介绍项目）。

## 约定
- 本项目 `origin` 远程**统一使用 SSH 协议**推送/拉取，**不再使用 HTTPS**。
- 公钥已配置在 GitHub 账户上，本机 `~/.ssh/` 下有可用的私钥（`id_ed25519` / `id_rsa`）。

## 当前配置（已落地）
```
origin  git@github.com:Tesla1983/ST32_H743Phone.git (fetch)
origin  git@github.com:Tesla1983/ST32_H743Phone.git (push)
```
日常推送即可直接用：
```bash
git push origin main
```
无需再带完整 SSH URL。

## 为什么改用 SSH（历史原因）
早期 `origin` 是 HTTPS（`https://github.com/Tesla1983/ST32_H743Phone.git`）。
从该环境向 GitHub 推送时，HTTPS 路径连续多次失败：
```
error: RPC failed; HTTP 408 curl 22 The requested URL returned error: 408
send-pack: unexpected disconnect while reading sideband packet
fatal: the remote end hung up unexpectedly
```
表现为 GitHub 边缘节点（曾见 `x-github-edge-region: japaneast`）在接收 pack 时
请求超时断连，疑似上传速率慢触发代理请求超时。增大 `http.postBuffer`、
放宽 `http.lowSpeedLimit/LowSpeedTime` 均无效。

改用 SSH 协议（`git@github.com:Tesla1983/ST32_H743Phone.git`）后一次性推送成功。
SSH 对慢速/较大对象更鲁棒，故固定为默认。

## 重新配置 / 迁移命令
若换机器或远程被改回 HTTPS，用下面命令切回 SSH：
```bash
git remote set-url origin git@github.com:Tesla1983/ST32_H743Phone.git
```
验证连通性（不推送）：
```bash
ssh -T git@github.com          # 应返回 "Hi Tesla1983! ..."
git push origin main --dry-run # 应返回 "Everything up-to-date"
```

## 备注
- fetch 也走 SSH，无需单独区分。
- 若日后 SSH 也出现超时，再排查网络或考虑拆分提交减小单次 pack 体积。
