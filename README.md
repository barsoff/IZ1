# Танковые бои

Консольная модель боя двух сторон на C для Linux.

Условие, устройство программы, правила и результаты проверок описаны
в [отчёте PDF](report.pdf). 

## Сборка и запуск

Команды выполняются из каталога `IZ1`:

```bash
cmake -S . -B build
cmake --build build
./build/tanks
```

Без параметров запускается встроенный пример. Запуск с данными из файла:

```bash
./build/tanks --config data/demo.txt --seed 24 --delay 200 --log battle.log
./build/tanks --help
```

События боя выводятся на экран, итоги сохраняются в `battle.log`.
Для прерывания нажмите Ctrl+C.

## Проверка

```bash
bash tests/run_tests.sh ./build/tanks
```
