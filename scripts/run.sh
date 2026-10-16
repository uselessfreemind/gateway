#!/usr/bin/env bash
set -euo pipefail
source /workspace/gateway/scripts/dotnet-env.sh
cd /workspace/gateway/src/Gateway
exec dotnet bin/Debug/net8.0/Gateway.dll
