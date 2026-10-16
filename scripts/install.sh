#!/usr/bin/env bash
set -euo pipefail
# Cloud setup; use the existing isolated checkout instead of creating a worktree.
mkdir -p /workspace/.tmp
if [[ ! -x /workspace/.dotnet/dotnet ]] || ! /workspace/.dotnet/dotnet --list-sdks | rg -q '^8\.0\.426 ' ; then
  curl --fail --silent --show-error --location https://dot.net/v1/dotnet-install.sh -o /workspace/.tmp/dotnet-install.sh
  bash /workspace/.tmp/dotnet-install.sh --version 8.0.426 --install-dir /workspace/.dotnet
fi
source /workspace/gateway/scripts/dotnet-env.sh
cd /workspace/gateway
dotnet restore Gateway.sln --locked-mode
dotnet build Gateway.sln --no-restore --nologo
