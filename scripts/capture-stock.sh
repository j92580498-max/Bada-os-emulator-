#!/usr/bin/env bash
# Capture the actual terminal running the CLI. The CLI writes LCD pixels separately.
set -euo pipefail
cli=$1
firmware=$2
budget=$3
mkdir -p stock-images

capture() {
    local name=$1
    shift
    local record="$RUNNER_TEMP/$name" terminal window
    xterm -hold -title "$name" -geometry 100x27 -fa 'DejaVu Sans Mono' -fs 13 \
        -bg '#101820' -fg '#f1f5f9' -e bash -c '
            record=$1
            shift
            "$@" 2>&1 | tee "$record.log"
            echo "${PIPESTATUS[0]}" > "$record.status"
        ' bash "$record" "$@" &
    terminal=$!
    window=$(timeout 20 xdotool search --sync --onlyvisible --name "^$name$" | head -n 1)
    until [[ -s $record.status ]]; do
        kill -0 "$terminal" || return 1
        sleep 0.2
    done
    sleep 0.3
    import -window "$window" "stock-images/$name.png"
    cat "$record.log"
    kill "$terminal"
    wait "$terminal" || true
    return "$(cat "$record.status")"
}

capture 01-firmware-inventory "$cli" inspect "$firmware"
capture 02-stock-boot-trace "$cli" boot "$firmware" stock-images/03-guest-lcd.png "$budget"
