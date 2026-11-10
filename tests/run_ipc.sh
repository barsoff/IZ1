#!/usr/bin/env bash
set -euo pipefail

dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bin=$(realpath -- "${1:-$dir/build/tanks-ipc}")
team=$(dirname -- "$bin")/tanks-team
base=$(dirname -- "$bin")/tanks
tmp=$(mktemp -d)
name=/tanks_test_$$
pid=''
a=''
b=''
n=0
extra=()

cleanup() {
    for p in "$pid" "$a" "$b" "${extra[@]}"; do
        if [[ -n "$p" ]]; then kill -TERM "$p" 2>/dev/null || true; fi
    done
    wait 2>/dev/null || true
    rm -rf -- "$tmp"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

start() {
    : > "$tmp/err"
    "$bin" --ipc "$name" --log "$tmp/stats" "$@" > "$tmp/out" 2> "$tmp/err" &
    pid=$!
    local ready=0
    for ((i = 0; i < 200; ++i)); do
        if grep -q 'IPC готов' "$tmp/err"; then ready=1; break; fi
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.01
    done
    [[ "$ready" == 1 ]] || fail 'координатор не создал сеанс'
}

join() {
    "$team" "$name" A > "$tmp/a.out" 2> "$tmp/a.err" &
    a=$!
    "$team" "$name" B > "$tmp/b.out" 2> "$tmp/b.err" &
    b=$!
}

finish() {
    local expected=$1 rc=0
    wait "$pid" || rc=$?
    pid=''
    [[ "$rc" == "$expected" ]] || fail "код координатора $rc, ожидался $expected"
    for p in "$a" "$b"; do
        if [[ -n "$p" ]]; then wait "$p" || fail 'ошибка стороны'; fi
    done
    a=''
    b=''
    [[ ! -e /dev/shm/${name#/} ]] || fail 'осталась память'
    local sems=(/dev/shm/sem."${name#/}"_s*)
    [[ ! -e "${sems[0]}" ]] || fail 'остались семафоры'
    n=$((n + 1))
}

# Все корректные сценарии дают тот же результат в трёх реализациях
for cfg in demo draw win_a win_b conflict blocked escape wreck multi_hit miss empty; do
    "$base" --config "$dir/data/$cfg.txt" --delay 0 --log "$tmp/ref.stats" > "$tmp/ref.out"
    if [[ "$cfg" == empty ]]; then
        "$bin" --ipc "$name" --config "$dir/data/$cfg.txt" --delay 0 --log "$tmp/stats" > "$tmp/out"
    else
        start --config "$dir/data/$cfg.txt" --delay 0
        join
        finish 0
    fi
    cmp "$tmp/ref.out" "$tmp/out" || fail "вывод отличается: $cfg"
    cmp "$tmp/ref.stats" "$tmp/stats" || fail "итоги отличаются: $cfg"
done

for sig in INT TERM; do
    start --config "$dir/data/conflict.txt" --limit 0 --delay 30
    join
    for ((i = 0; i < 200; ++i)); do
        if grep -q 'Поле после такта 1:' "$tmp/out"; then break; fi
        sleep 0.01
    done
    grep -q 'Поле после такта 1:' "$tmp/out" || fail 'бой не начался'
    kill -s "$sig" "$pid"
    rc=130
    if [[ "$sig" == TERM ]]; then rc=143; fi
    finish "$rc"
    grep -q 'Прервано пользователем' "$tmp/stats" || fail 'не записано прерывание'
done

# Остановка во время ожидания ещё не подключившихся сторон
start --delay 0
kill -INT "$pid"
finish 130

# Прерывание при подключённой только одной стороне
for side in A B; do
    start --delay 0
    "$team" "$name" "$side" > "$tmp/one.out" 2> "$tmp/one.err" &
    a=$!
    sleep 0.1
    kill -INT "$pid"
    finish 130
done

# Повторное имя не должно повредить уже работающий сеанс
start --config "$dir/data/conflict.txt" --limit 0 --delay 30
rc=0
"$bin" --ipc "$name" --delay 0 --log "$tmp/dup.stats" > "$tmp/dup.out" 2> "$tmp/dup.err" || rc=$?
[[ "$rc" == 1 ]] || fail 'повторное имя принято'
join
for ((i = 0; i < 200; ++i)); do
    if grep -q 'Поле после такта 1:' "$tmp/out"; then break; fi
    sleep 0.01
done
rc=0
"$team" "$name" A > "$tmp/dup.out" 2> "$tmp/dup.err" || rc=$?
[[ "$rc" == 1 ]] || fail 'повторная сторона принята'
kill -INT "$pid"
finish 130

# Потеря стороны обнаруживается по таймауту без зависания координатора
start --config "$dir/data/conflict.txt" --limit 0 --delay 30
join
for ((i = 0; i < 200; ++i)); do
    if grep -q 'Поле после такта 1:' "$tmp/out"; then break; fi
    sleep 0.01
done
kill -KILL "$a"
wait "$a" 2>/dev/null || true
a=''
finish 1
grep -q 'Ошибка выполнения' "$tmp/stats" || fail 'потеря стороны не отражена в итогах'

# Прерывание рабочего процесса также останавливает координатор
start --config "$dir/data/conflict.txt" --limit 0 --delay 30
join
for ((i = 0; i < 200; ++i)); do
    if grep -q 'Поле после такта 1:' "$tmp/out"; then break; fi
    sleep 0.01
done
kill -INT "$a"
finish 130

# Два сеанса с разными именами не используют чужие данные
start --config "$dir/data/draw.txt" --delay 0
"$bin" --ipc "${name}_other" --config "$dir/data/win_a.txt" --delay 0 --log "$tmp/other.stats" > "$tmp/other.out" 2> "$tmp/other.err" &
other=$!
extra+=("$other")
for ((i = 0; i < 200; ++i)); do
    if grep -q 'IPC готов' "$tmp/other.err"; then break; fi
    sleep 0.01
done
grep -q 'IPC готов' "$tmp/other.err" || fail 'второй сеанс не запустился'
for side in A B; do
    "$team" "${name}_other" "$side" &
    extra+=("$!")
done
join
finish 0
for p in "${extra[@]}"; do wait "$p" || fail 'ошибка второго сеанса'; done
extra=()
grep -q 'Победа стороны A' "$tmp/other.stats" || fail 'второй сеанс получил чужие данные'
grep -q 'Ничья' "$tmp/stats" || fail 'первый сеанс получил чужие данные'
[[ ! -e /dev/shm/${name#/}_other ]] || fail 'осталась память второго сеанса'

# Независимый процесс не создаёт отсутствующий сеанс
rc=0
"$team" "$name" A > "$tmp/missing.out" 2> "$tmp/missing.err" || rc=$?
[[ "$rc" == 1 ]] || fail 'несуществующий сеанс принят'
echo "OK: $n проверок IPC пройдено"
