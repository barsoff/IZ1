# Танковые бои

Многопроцессная модель боя на C для Linux, вариант 24
Процесс стороны рассчитывает планы семейства танков, координатор применяет их после ответа обеих сторон

Отчет доступен в [PDF](report.pdf)

## Сборка и запуск

Команды выполняются из каталога `IZ1`:

```bash
cmake -S . -B build
cmake --build build
./build/tanks --config data/demo.txt --seed 24 --delay 200 --log battle.log
./build/tanks-unnamed
./build/tanks --help
```

`tanks` - дочерние процессы с именованными POSIX-семафорами, `tanks-unnamed` - с неименованными в разделяемой памяти
Обмен в обеих версиях - через POSIX shared memory, без параметров запускается встроенный пример

Независимые процессы с именованными семафорами и POSIX shared memory запускаются в трёх терминалах, стороны - после сообщения «IPC готов»:

```bash
./build/tanks-ipc --ipc /battle24 --config data/demo.txt
./build/tanks-team /battle24 A
./build/tanks-team /battle24 B
```

Ожидание сторон - 30 секунд, ответа в бою - 5 секунд
События выводятся на экран, итоги сохраняются в `battle.log`
Ctrl+C завершает бой, координатор удаляет семафоры и память

## Проверка

```bash
ctest --test-dir build --output-on-failure
```
