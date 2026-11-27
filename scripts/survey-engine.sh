#!/usr/bin/env bash
# A separate diagnostic run logs and retains unknown MMIO register values.
set -euo pipefail
[[ ${WAVE723_PROBE_SECONDS} =~ ^[1-9][0-9]?$ ]]
if [[ ${WAVE723_SURVEY_SCOPE:-} == stock ]]; then
    exec gdb -nx -q -batch -return-child-result -x "$(dirname "$0")/stock-panel.gdb" \
        --args "$WAVE723_SURVEY_ENGINE" "$@"
fi
args=("$@")
for ((i=0; i+1<${#args[@]}; ++i)); do
    if [[ ${args[i]} == -append && ${args[i+1]} == capture:diagnostic:apps:* ]]; then
        mode=survey
        if [[ ${WAVE723_SURVEY_SCOPE:-both} == apps ]]; then mode=survey-apps; fi
        args[i+1]="diagnostic:$mode:${args[i+1]#capture:diagnostic:}"
        exec gdb -nx -q -batch -x "$(dirname "$0")/stock-panel.gdb" \
            --args "$WAVE723_SURVEY_ENGINE" "${args[@]}"
    fi
done
exit 2
