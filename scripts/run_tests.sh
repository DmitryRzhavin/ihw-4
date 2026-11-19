#!/usr/bin/env bash
set -uo pipefail
cd "$(dirname "$0")/.."
build=${BUILD_DIR:-build}
tmp=$(mktemp -d)
port=$((20000 + RANDOM % 20000))
passed=0
server=0
server_status=0
last_status=0
declare -A cpid opid
all_pids=()
fail() {
    echo "FAIL: $*" >&2
    local f
    for f in "$tmp"/*.log "$tmp"/*.out; do
        [[ -f $f ]] && { echo "--- $f" >&2; tail -n 12 "$f" >&2; }
    done
    exit 1
}
cleanup() {
    local p
    for p in "${all_pids[@]:-}"; do [[ -n $p ]] && kill -KILL "$p" 2>/dev/null; done
    rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
pass() { passed=$((passed+1)); echo "PASS $*"; }
field() { awk -v key="$2" '/ SUMMARY / {for(i=1;i<=NF;i++){split($i,p,"=");if(p[1]==key)v=p[2]}} END{print v}' "$1"; }
count_of() { awk -F= -v key="$2" '$1 ~ "^[[:space:]]*"key"[[:space:]]*$" {gsub(/[[:space:]]/,"",$2); v=$2} END{print v}' "configs/$1.conf"; }
wait_for() {
    local i
    for ((i=0; i<${2:-30}*50; i++)); do
        grep -q -- "$1" "$tmp/server.log" 2>/dev/null && return 0
        sleep 0.02
    done
    return 1
}
start_server() {
    local cfg=$1 unit=$2 i
    shift 2
    port=$((port+1))
    rm -f "$tmp"/*.out "$tmp"/*.log
    "$build/kitchen_server" --port "$port" --config "configs/$cfg.conf" --unit-ms "$unit" \
        --timeout 60 --no-log "$@" > "$tmp/server.log" 2>&1 &
    server=$!
    all_pids+=("$server")
    for ((i=0; i<150; i++)); do
        grep -q 'SERVER listening' "$tmp/server.log" 2>/dev/null && return 0
        kill -0 "$server" 2>/dev/null || break
        sleep 0.02
    done
    fail "server did not start ($cfg)"
}
start_client() {
    "$build/kitchen_client" --port "$port" --role "$1" --id "$2" > "$tmp/$1$2.out" 2>&1 &
    cpid["$1$2"]=$!
    all_pids+=("$!")
}
start_observer() {
    "$build/kitchen_observer" --port "$port" --name "$1" > "$tmp/obs_$1.out" 2>&1 &
    opid["$1"]=$!
    all_pids+=("$!")
}
start_all() {
    local i
    for ((i=1; i<=$(count_of "$1" visitors); i++)); do start_client visitor "$i"; done
    for ((i=1; i<=$(count_of "$1" waiters); i++)); do start_client waiter "$i"; done
    for ((i=1; i<=$(count_of "$1" cooks); i++)); do start_client cook "$i"; done
}
stop_client() {
    kill "-$2" "${cpid[$1]}" 2>/dev/null
    wait "${cpid[$1]}"
    last_status=$?
    unset "cpid[$1]"
}
stop_observer() {
    kill "-$2" "${opid[$1]}" 2>/dev/null
    wait "${opid[$1]}"
    last_status=$?
    unset "opid[$1]"
}
finish_all() {
    local k st bad=0
    wait "$server"
    server_status=$?
    for k in "${!cpid[@]}"; do
        wait "${cpid[$k]}"; st=$?
        [[ $st == 0 ]] || { echo "client $k exited with $st" >&2; bad=1; }
    done
    for k in "${!opid[@]}"; do
        wait "${opid[$k]}"; st=$?
        [[ $st == 0 ]] || { echo "observer $k exited with $st" >&2; bad=1; }
    done
    cpid=(); opid=()
    return $bad
}
expect_normal() {
    finish_all || fail "$1: a participant exited abnormally"
    [[ $server_status == 0 ]] || fail "$1: server status $server_status"
    bash scripts/check_log.sh "$tmp/server.log" || fail "$1: log check"
    [[ $(field "$tmp/server.log" termination) == normal ]] || fail "$1: not normal"
}
expect_status() {
    local want=$1 got
    shift
    "$@" > "$tmp/cmd.out" 2>&1
    got=$?
    [[ $got == "$want" ]] || fail "$* -> $got (expected $want)"
}
scenario() {
    local cfg=$1 delivered=$2 canceled=$3 operations=$4
    start_server "$cfg" "${5:-2}"
    start_observer watcher
    start_all "$cfg"
    expect_normal "$cfg"
    [[ $(field "$tmp/server.log" delivered) == "$delivered" ]] || fail "$cfg: delivered"
    [[ $(field "$tmp/server.log" canceled) == "$canceled" ]] || fail "$cfg: canceled"
    [[ $(field "$tmp/server.log" completed) == "$operations" ]] || fail "$cfg: operations"
    grep -q '== kitchen closed: normal' "$tmp/obs_watcher.out" || fail "$cfg: observer was not closed"
    grep -q ' SUMMARY ' "$tmp/obs_watcher.out" || fail "$cfg: observer missed the summary"
    if [[ $cfg == limited_queues || $cfg == stress ]]; then
        [[ $(field "$tmp/server.log" max_kitchen) -le 1 && $(field "$tmp/server.log" max_ready) -le 1 ]] || fail "$cfg: capacity"
    fi
    pass "scenario/$cfg"
}
"./$build/model_test" > "$tmp/model.out" 2>&1 || { cat "$tmp/model.out"; fail 'model_test'; }
pass 'model/invariants'
scenario demo 4 0 16
scenario limited_queues 4 0 16
scenario ready_full 4 0 16
scenario lifo 4 0 16
scenario random_orders 4 0 24
scenario impossible 2 2 8
scenario stress 24 0 288 1
start_server demo 200
start_observer watcher
start_all demo
wait_for 'START cook=1 ' || fail 'cook 1 did not start'
stop_client cook1 INT
[[ $last_status == 130 ]] || fail "cook interrupted status $last_status"
wait_for 'ABORT cook=1' 5 || fail 'abort was not recorded'
start_client cook 1
expect_normal cook-reconnect
grep -q 'RECONNECT role=2 id=1' "$tmp/server.log" || fail 'cook reconnect missing'
[[ $(field "$tmp/server.log" delivered) == 4 && $(field "$tmp/server.log" aborted) -ge 1 ]] || fail 'cook counters'
pass 'dynamic/cook-disconnect-reconnect'
start_server demo 150
for name in o1 o2 o3; do start_observer "$name"; done
wait_for 'observers=3' 5 || fail 'observers did not join'
start_all demo
wait_for 'START cook=' || fail 'cooking did not start'
stop_observer o2 INT
[[ $last_status == 130 ]] || fail "observer status $last_status"
wait_for 'OBSERVER_LEAVE name=o2' 5 || fail 'observer leave missing'
start_observer o4
wait_for 'OBSERVER_JOIN name=o4' 5 || fail 'observer rejoin missing'
expect_normal observers
for name in o1 o3 o4; do grep -q '== kitchen closed: normal' "$tmp/obs_$name.out" || fail "observer $name"; done
grep -q 'SERVER listening' "$tmp/obs_o4.out" || fail 'rejoined observer got no history'
pass 'dynamic/observers-join-leave'
start_server demo 200
start_all demo
wait_for 'ACCEPT waiter=1 ' || fail 'waiter 1 accepted nothing'
stop_client waiter1 INT
[[ $last_status == 130 ]] || fail "waiter interrupted status $last_status"
wait_for 'ORPHAN order=' 5 || fail 'orphan order missing'
start_client waiter 1
expect_normal waiter-reconnect
grep -q 'ADOPT waiter=' "$tmp/server.log" || fail 'no waiter took over the order'
[[ $(field "$tmp/server.log" delivered) == 4 ]] || fail 'waiter counters'
pass 'dynamic/waiter-disconnect'
start_server demo 200
start_all demo
wait_for 'ARRIVAL visitor=1' || fail 'visitor 1 did not arrive'
stop_client visitor1 INT
[[ $last_status == 130 ]] || fail "visitor interrupted status $last_status"
wait_for 'ASSEMBLED order=1 ' 20 || fail 'order 1 was not assembled'
sleep 0.5
grep -q 'DELIVER .* order=1 ' "$tmp/server.log" && fail 'order delivered to an absent visitor'
start_client visitor 1
expect_normal visitor-reconnect
reconnect_line=$(grep -n 'RECONNECT role=0 id=1' "$tmp/server.log" | head -n 1 | cut -d: -f1)
deliver_line=$(grep -n 'DELIVER .* order=1 ' "$tmp/server.log" | head -n 1 | cut -d: -f1)
[[ -n $reconnect_line && -n $deliver_line && $reconnect_line -lt $deliver_line ]] || fail 'delivery before visitor returned'
pass 'dynamic/visitor-disconnect'
for sig in INT TERM; do
    start_server demo 300
    start_observer watcher
    start_all demo
    wait_for 'START cook=' || fail 'cooking did not start'
    kill "-$sig" "$server"
    finish_all || fail "participants after server SIG$sig"
    expected=130
    [[ $sig == TERM ]] && expected=143
    [[ $server_status == "$expected" ]] || fail "server SIG$sig status $server_status"
    [[ $(field "$tmp/server.log" termination) == interrupted ]] || fail 'server summary'
    grep -q '== kitchen closed: interrupted' "$tmp/obs_watcher.out" || fail 'observer was not notified'
    bash scripts/check_log.sh "$tmp/server.log" || fail 'log after signal'
    pass "server/SIG$sig-closes-clients"
done
start_server demo 300
start_observer watcher
start_all demo
wait_for 'START cook=' || fail 'cooking did not start'
kill -KILL "$server"
{ wait "$server"; } 2>/dev/null
for k in "${!cpid[@]}"; do
    wait "${cpid[$k]}"; st=$?
    [[ $st == 3 ]] || fail "client $k status $st after server crash"
done
wait "${opid[watcher]}"; st=$?
[[ $st == 3 ]] || fail "observer status $st after server crash"
cpid=(); opid=()
pass 'server/crash-detected-by-clients'
start_server demo 2
exec 3<>"/dev/tcp/127.0.0.1/$port" || fail 'tcp connection'
printf 'GARBAGE\n' >&3
IFS= read -r -t 3 answer <&3
[[ $answer == 'ERR protocol' ]] || fail "garbage answer: $answer"
exec 3<&- 3>&-
exec 3<>"/dev/tcp/127.0.0.1/$port" || fail 'tcp connection'
printf 'HELLO chef 1 5\n' >&3
IFS= read -r -t 3 answer <&3
[[ $answer == 'ERR role' ]] || fail "role answer: $answer"
exec 3<&- 3>&-
exec 3<>"/dev/tcp/127.0.0.1/$port" || fail 'tcp connection'
head -c 3000 /dev/zero | tr '\0' 'a' >&3
sleep 0.2
exec 3<&- 3>&-
kill -0 "$server" 2>/dev/null || fail 'server stopped after malformed input'
start_client cook 1
sleep 0.3
"$build/kitchen_client" --port "$port" --role cook --id 1 > "$tmp/duplicate.out" 2>&1
[[ $? == 1 ]] || fail 'duplicate cook id was accepted'
"$build/kitchen_client" --port "$port" --role cook --id 9 > "$tmp/range.out" 2>&1
[[ $? == 1 ]] || fail 'cook id out of range was accepted'
start_observer watcher
for i in 1 2 3 4; do start_client visitor "$i"; done
for i in 1 2; do start_client waiter "$i"; done
for i in 2 3; do start_client cook "$i"; done
expect_normal protocol
[[ $(field "$tmp/server.log" delivered) == 4 ]] || fail 'run after malformed input'
pass 'protocol/malformed-input-and-duplicates'
start_server demo 50
expect_status 1 "$build/kitchen_server" --port "$port" --no-log --timeout 3
kill -TERM "$server"; wait "$server" 2>/dev/null
pass 'errors/port-busy'
expect_status 2 "$build/kitchen_server" --port 0 --no-log
expect_status 2 "$build/kitchen_server" --port abc --no-log
expect_status 2 "$build/kitchen_server" --config tests/invalid.conf --no-log
expect_status 2 "$build/kitchen_server" --config configs/missing.conf --no-log
expect_status 2 "$build/kitchen_client" --role chef --id 1
expect_status 2 "$build/kitchen_client" --role cook --id 0
expect_status 2 "$build/kitchen_observer" --name "bad name"
pass 'errors/arguments-and-configuration'
expect_status 3 "$build/kitchen_client" --port $((port+500)) --role cook --id 1 --connect-timeout 300
expect_status 3 "$build/kitchen_observer" --port $((port+500)) --connect-timeout 300
pass 'errors/no-server'
echo "All $passed tests passed."
