#!/usr/bin/env bash
# ============================================================================
# push_github.sh —— 一条命令推到 GitHub
# ============================================================================
# 用法：
#   GH_TOKEN=ghp_xxx ./tools/push_github.sh https://github.com/<user>/<repo>.git
#   ./tools/push_github.sh git@github.com:<user>/<repo>.git      # 已配好 SSH key
#
# 说明：
#   · 仓库不存在时不会自动创建；先在网页上建空仓库（不要勾 README）
#   · token 只在本条命令内使用，不写进 .git/config
# ============================================================================
set -euo pipefail

REMOTE="${1:?用法: $0 <仓库地址>}"
BRANCH="${BRANCH:-main}"

cd "$(dirname "$0")/.."

if [ ! -d .git ]; then
  git init -q
  git add -A
  git -c user.name="${GIT_NAME:-YG}" -c user.email="${GIT_EMAIL:-yg@local}" \
      commit -q -m "Initial commit: ImGui overlay UI framework (cheat features removed)"
fi

git branch -M "$BRANCH"

if [ -n "${GH_TOKEN:-}" ] && [[ "$REMOTE" == https://* ]]; then
  AUTH_REMOTE="https://${GH_TOKEN}@${REMOTE#https://}"
  git remote remove origin 2>/dev/null || true
  git remote add origin "$REMOTE"
  git push "$AUTH_REMOTE" "$BRANCH" -u
else
  git remote remove origin 2>/dev/null || true
  git remote add origin "$REMOTE"
  git push -u origin "$BRANCH"
fi

echo "OK -> $REMOTE ($BRANCH)"
