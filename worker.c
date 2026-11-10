#include "proc.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[2], "A") && strcmp(argv[2], "B"))) {
        dprintf(STDERR_FILENO, "Использование: tanks-team /имя_IPC A|B\n");
        return 1;
    }
    return proc_worker(argv[1], argv[2][0] - 'A');
}
