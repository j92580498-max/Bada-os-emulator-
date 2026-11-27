#!/usr/bin/env bash
# A separate diagnostic run logs and retains unknown MMIO register values.
set -euo pipefail
args=("$@")
for ((i=0; i+1<${#args[@]}; ++i)); do
    if [[ ${args[i]} == -append && ${args[i+1]} == capture:diagnostic:apps:* ]]; then
        args[i+1]="diagnostic:survey:${args[i+1]#capture:diagnostic:}"
        exec "$WAVE723_SURVEY_ENGINE" "${args[@]}"
    fi
done
exit 2
