#!/usr/bin/env bash
set -euo pipefail

dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bin=$(realpath -- "${1:-$dir/build/tanks}")
tmp=$(mktemp -d)
pid=''
n=0

cleanup() {
    if [[ -n "$pid" ]]; then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf -- "$tmp"
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

has() {
    grep -Fq -- "$2" "$1" || fail "в $1 не найдено: $2"
    n=$((n + 1))
}

lacks() {
    if grep -Eq -- "$2" "$1"; then
        fail "в $1 найдено запрещённое событие: $2"
    fi
    n=$((n + 1))
}

run() {
    local name=$1
    shift
    "$bin" --config "$dir/data/$name.txt" --delay 0 --log "$tmp/$name.stats" "$@" > "$tmp/$name.log"
    tail -n 7 "$tmp/$name.log" | cmp "$tmp/$name.stats" - || fail "итоги в журнале и на экране различаются: $name"
}

reject() {
    local rc=0
    "$bin" --delay 0 --log "$tmp/error.log" "$@" > "$tmp/error.out" 2> "$tmp/error.err" || rc=$?
    [[ "$rc" == 1 ]] || fail "для ошибочного ввода ожидался код 1, получен $rc"
    [[ -s "$tmp/error.err" ]] || fail 'нет сообщения об ошибке'
    n=$((n + 1))
}

run draw
has "$tmp/draw.log" 'Причина: Ничья'
has "$tmp/draw.log" 'Тактов: 2'
has "$tmp/draw.log" 'Такт 2: A1 уничтожен'
has "$tmp/draw.log" 'Такт 2: B1 уничтожен'
lacks "$tmp/draw.log" 'Такт 1: .* (попал|получил урон)'

run win_a
has "$tmp/win_a.log" 'Причина: Победа стороны A'
has "$tmp/win_a.log" 'A1: (0, 0), прочность 2'
run win_b
has "$tmp/win_b.log" 'Причина: Победа стороны B'

run conflict
has "$tmp/conflict.log" 'конфликт A1 и B1 за (1, 0)'
has "$tmp/conflict.log" 'Причина: Достигнут лимит тактов'
has "$tmp/conflict.log" 'Тактов: 3'
lacks "$tmp/conflict.log" 'переместился|стреляет'

run blocked
has "$tmp/blocked.log" 'B1: (2, 0), прочность 2'
lacks "$tmp/blocked.log" 'A1 переместился|в \(1, 0\)'

run escape
has "$tmp/escape.log" 'Такт 3: снаряд A1 достиг (5, 0), действующего танка там нет.'
has "$tmp/escape.log" 'Такт 4: B1 переместился из (2, 0) в (1, 0).'
lacks "$tmp/escape.log" 'получил урон'

run wreck
has "$tmp/wreck.log" 'Такт 2: B1 уничтожен, обломки остаются в (2, 0).'
has "$tmp/wreck.log" 'Такт 3: A1 намерен остаться в (1, 0).'
has "$tmp/wreck.log" 'Такт 3: снаряд B1 попал в A1'
lacks "$tmp/wreck.log" 'Такт [34]: B1 (наблюдает|намерен|стреляет|переместился)'

run multi_hit
has "$tmp/multi_hit.log" 'B1 получил урон 2, прочность 1 -> 0'
lacks "$tmp/multi_hit.log" 'прочность -|-> -'
run miss
has "$tmp/miss.log" 'промахнулся'
has "$tmp/miss.log" 'попадания 0, без урона 2, в полёте 1'
lacks "$tmp/miss.log" 'получил урон'
run empty
has "$tmp/empty.log" 'Тактов: 0'
has "$tmp/empty.log" 'Причина: Ничья'

# Одинаковые исходные данные и seed дают одинаковый журнал.
run demo --seed 37
cp "$tmp/demo.log" "$tmp/first.log"
run demo --seed 37
cmp "$tmp/demo.log" "$tmp/first.log" || fail 'бой не воспроизводится'
n=$((n + 1))

# Порядок строк с танками не должен давать преимущество стороне.
grep -v '^tank ' "$dir/data/demo.txt" > "$tmp/reversed.txt"
grep '^tank ' "$dir/data/demo.txt" | tac >> "$tmp/reversed.txt"
"$bin" --config "$tmp/reversed.txt" --seed 37 --delay 0 --log "$tmp/reversed.stats" > "$tmp/reversed.log"
cmp "$tmp/demo.log" "$tmp/reversed.log" || fail 'результат зависит от порядка танков'
n=$((n + 1))

"$bin" --delay 0 --log "$tmp/default.stats" > "$tmp/default.log"
run demo
cmp "$tmp/default.log" "$tmp/demo.log" || fail 'встроенный пример отличается от demo.txt'
n=$((n + 1))

# Пробелы в пути, CRLF и отсутствие завершающего перевода строки.
awk '{printf "%s\r\n", $0}' "$dir/data/draw.txt" > "$tmp/windows map.txt"
"$bin" --config "$tmp/windows map.txt" --delay 0 --log "$tmp/windows.stats" > "$tmp/windows.log"
cmp "$tmp/windows.log" "$tmp/draw.log" || fail 'ошибка чтения CRLF'
txt=$(cat "$dir/data/draw.txt")
printf '%s' "$txt" > "$tmp/no_newline.txt"
"$bin" --config "$tmp/no_newline.txt" --delay 0 --log "$tmp/no_newline.stats" > "$tmp/no_newline.log"
cmp "$tmp/no_newline.log" "$tmp/draw.log" || fail 'ошибка чтения последней строки'
n=$((n + 2))

run draw --limit 1
has "$tmp/draw.log" 'Тактов: 1'
has "$tmp/draw.log" 'Причина: Достигнут лимит тактов'

reject --config "$dir/data/bad_overlap.txt"
reject --config "$dir/data/bad_map.txt"
reject --config "$tmp/missing.txt"
reject --seed -1
reject --limit 999999999999999999999
reject --delay abc
reject --unknown 1
reject --seed
sed 's/accuracy 100/accuracy 101/' "$dir/data/draw.txt" > "$tmp/bad_acc.txt"
reject --config "$tmp/bad_acc.txt"
sed 's/flight 1/flight 0/' "$dir/data/draw.txt" > "$tmp/bad_flight.txt"
reject --config "$tmp/bad_flight.txt"
sed 's/tank B 1 2 0/tank B 1 3 0/' "$dir/data/draw.txt" > "$tmp/bad_pos.txt"
reject --config "$tmp/bad_pos.txt"
cp "$dir/data/draw.txt" "$tmp/same.txt"
reject --config "$tmp/same.txt" --log "$tmp/same.txt"
cmp "$dir/data/draw.txt" "$tmp/same.txt" || fail 'журнал затёр сценарий'
reject --log "$tmp/missing_dir/log.txt"

# Максимум танков и времени полёта: очередь снарядов проходит полное заполнение.
{
    printf 'field 40 20\nlimit 50\naccuracy 100\nflight 20\nteam A hold\nteam B hold\nmap\n'
    for ((i = 0; i < 20; ++i)); do printf '%s\n' '........................................'; done
    for ((i = 0; i < 16; ++i)); do
        printf 'tank A %d %d 0 100 60\n' "$((i + 1))" "$i"
        printf 'tank B %d %d 19 100 60\n' "$((i + 1))" "$i"
    done
} > "$tmp/max.txt"
"$bin" --config "$tmp/max.txt" --delay 0 --log "$tmp/max.stats" > "$tmp/max.log"
has "$tmp/max.log" 'Тактов: 50'
has "$tmp/max.log" 'Снарядов в полёте: 640'
lacks "$tmp/max.log" 'нарушены правила'

# Сигнал подаётся только после того, как программа действительно начала бой.
interrupt() {
    local sig=$1
    shift
    : > "$tmp/signal.out"
    "$bin" "$@" --delay 30 --log "$tmp/signal.log" > "$tmp/signal.out" &
    pid=$!
    ready=0
    for ((i = 0; i < 100; ++i)); do
        if grep -q 'Поле после такта 1:' "$tmp/signal.out"; then
            ready=1
            break
        fi
        sleep 0.02
    done
    [[ "$ready" == 1 ]] || fail 'программа не начала бой перед проверкой сигнала'
    kill -s "$sig" "$pid"
    rc=0
    wait "$pid" || rc=$?
    pid=''
    exp=130
    if [[ "$sig" == TERM ]]; then exp=143; fi
    [[ "$rc" == "$exp" ]] || fail "неверный код завершения по SIG$sig: $rc"
    has "$tmp/signal.log" 'Причина: Прервано пользователем'
    has "$tmp/signal.log" '=== Итоги ==='
    tail -n 7 "$tmp/signal.out" | cmp "$tmp/signal.log" - || fail 'итоги прерывания не попали в журнал'
}

# Бесконечный сценарий из файла и конечный встроенный пример.
interrupt INT --config "$dir/data/conflict.txt" --limit 0
interrupt TERM --config "$dir/data/conflict.txt" --limit 0
interrupt INT --limit 100
interrupt TERM --limit 100

echo "OK: $n проверок пройдено."
