import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


SERVER, CLIENT, WATCH, LOCAL = sys.argv[1:]
IP = os.environ.get("IZ4_TEST_IP", "127.0.0.1")


def wait(path, text):
    end = time.monotonic() + 5
    while time.monotonic() < end:
        if text in path.read_text():
            return
        time.sleep(0.02)
    raise AssertionError(f"{path.name}: ожидалось {text!r}\n{path.read_text()}")


def read(sock, n):
    buf = bytearray()
    while len(buf) < n:
        part = sock.recv(n - len(buf))
        if not part:
            raise AssertionError("Сокет закрыт до конца сообщения")
        buf.extend(part)
    return buf


def check(root, stop):
    with socket.socket() as sock:
        sock.bind((IP, 0))
        port = str(sock.getsockname()[1])
    procs = []

    def start(name, *args):
        path = root / name
        with path.open("w") as out:
            proc = subprocess.Popen(args, stdout=out, stderr=subprocess.STDOUT)
        procs.append(proc)
        return proc, path

    log = root / "battle.log"
    reason = "Прервано пользователем" if stop else "Достигнут лимит тактов"
    try:
        srv, sout = start("server", SERVER, IP, port, "--limit", "5", "--delay", "150", "--log", log)
        wait(sout, "Сервер готов")
        obs, oout = start("watch1", WATCH, IP, port)
        wait(oout, "Поле после такта 0")
        a, aout = start("team-a", CLIENT, IP, port, "A")
        wait(sout, "Сторона A подключилась")
        dup, dout = start("dupe-a", CLIENT, IP, port, "A")
        assert dup.wait(timeout=5) == 1
        assert "Сторона уже подключена" in dout.read_text()

        with socket.create_connection((IP, int(port)), timeout=5) as sock:
            # Проверяем чтение заголовка и роли по частям
            msg = struct.pack("!IIc", 1, 1, b"B")
            for byte in msg:
                sock.sendall(bytes([byte]))
                time.sleep(0.002)
            kind, size = struct.unpack("!II", read(sock, 8))
            assert kind == 2
            assert struct.unpack_from("!Q", read(sock, size))[0] == 1
            wait(sout, "получен план стороны A")
            if stop:
                srv.send_signal(signal.SIGINT)
                kind, size = struct.unpack("!II", read(sock, 8))
                assert kind == 6
                assert reason in read(sock, size).decode()
                assert sock.recv(1) == b""
            else:
                obs.send_signal(signal.SIGINT)
                assert obs.wait(timeout=5) == 130
                obs, oout = start("watch2", WATCH, IP, port)
                wait(oout, "Поле после такта 0")
                second, out2 = start("watch3", WATCH, IP, port)
                wait(out2, "Поле после такта 0")
                assert "Поле после такта 1" not in oout.read_text()

        if not stop:
            wait(sout, "Сторона B отключилась")
            b, bout = start("team-b", CLIENT, IP, port, "B")
            assert b.wait(timeout=8) == 0, bout.read_text()
            assert second.wait(timeout=5) == 0, out2.read_text()
            assert "Поле после такта 5" in out2.read_text()
        assert srv.wait(timeout=5) == (130 if stop else 0), sout.read_text()
        assert a.wait(timeout=5) == 0, aout.read_text()
        assert obs.wait(timeout=5) == 0, oout.read_text()
        assert reason in aout.read_text()
        assert reason in oout.read_text()
        assert reason in log.read_text()
        if stop:
            assert "Тактов: 0" in log.read_text()
        else:
            assert "Поле после такта 5" in oout.read_text()
            local = root / "local.log"
            subprocess.run([LOCAL, "--limit", "5", "--delay", "0", "--log", local],
                           stdout=subprocess.DEVNULL, check=True, timeout=5)
            assert local.read_bytes() == log.read_bytes()
    finally:
        for proc in procs:
            if proc.poll() is None:
                proc.kill()
            proc.wait(timeout=5)


with tempfile.TemporaryDirectory() as work:
    for stop in (False, True):
        root = Path(work) / str(stop)
        root.mkdir()
        check(root, stop)
