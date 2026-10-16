#!/usr/bin/env python3
"""User mode test: ELF64 programs from /Programs run in ring 3 from the
Terminal.  Checks argv, the libc (printf, malloc, stdin), file system calls
through the GUI proxy, CPU exceptions killing only the faulting program,
Ctrl+C on a program that never makes a system call, the desktop staying
responsive meanwhile, and that process memory is returned."""
import sys, os, re, time, shutil, subprocess
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

def ic(c, r): return (6 + c * 84 + 42, 6 + r * 86 + 22)
S = "build/test"
NFS = "build/tools/nfstool"
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
IMG = f"{S}/user.img"
shutil.copy("build/nimbus.img", IMG)

with Qemu([IMG], name="user") as q:
    q.wait_serial(r"Desktop ready", timeout=40)
    assert re.search(r"\[sys \] system calls on int 0x80", q.serial())
    q.mouse_home()
    q.double_click(*ic(0, 3))
    q.wait_serial(r'created: "Terminal"', 5)

    mark = [0]
    def run(cmd):
        mark[0] = len(q.serial())          # later waits only look at newer output
        q.type(cmd + "\n", delay=0.015)
    def wait_new(pat, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            m = re.search(pat, q.serial()[mark[0]:])
            if m: return m
            time.sleep(0.1)
        raise TimeoutError("%r not seen; new output:\n%s" % (pat, q.serial()[mark[0]:][-2500:]))
    def exited(name, code=None, timeout=15):
        m = wait_new(r"\[proc\] pid (\d+) '%s' exited with code (%s)" % (name, code if code is not None else r"-?\d+"), timeout)
        wait_new(r"\[term\] program pid %s finished" % m.group(1), 5)
        return m

    # 1. arguments and output
    run("hello one two")
    exited("hello", 0)
    log = q.serial()
    assert 'argv[2] = "two"' in log and "I was started with 3 arguments" in log, "hello output"

    # 2. malloc'ed sieve; run it several times to check memory is returned
    frees = []
    for i in range(4):
        run("primes 1000000")
        exited("primes", 0)
        time.sleep(0.6)
        frees.append([int(x) for x in re.findall(r"reaped, (\d+) frames free", q.serial())][-1])
    assert "78498 primes up to 1000000" in q.serial(), "primes result"
    assert frees[-1] >= frees[0] - 8, f"process memory leak: {frees}"

    # 3. interactive stdin: binary search in guess
    run("guess")
    lo, hi, seen = 1, 100, 0
    for _ in range(10):
        wait_new(r"Your guess: ", 5)
        g = (lo + hi) // 2
        before = len(re.findall(r"Higher!|Lower!|Correct!", q.serial()))
        run(str(g))
        t0 = time.time()
        while len(re.findall(r"Higher!|Lower!|Correct!", q.serial())) == before:
            assert time.time() - t0 < 5, "no answer from guess"
            time.sleep(0.1)
        ans = re.findall(r"Higher!|Lower!|Correct!", q.serial())[-1]
        if ans == "Correct!": break
        if ans == "Higher!": lo = g + 1
        else: hi = g - 1
    exited("guess", 0)
    assert re.search(r"Correct! You got it in \d+ tries", q.serial())

    # 4. files: todo writes /Documents/todo.txt, wc reads a file
    run('todo add buy oat milk')
    exited("todo", 0)
    run("todo add call the plumber")
    exited("todo", 0)
    run("todo")
    exited("todo", 0)
    assert " 2. call the plumber" in q.serial()
    run("wc /readme.txt")
    exited("wc", 0)
    run("wc /no/such/file")
    exited("wc", 1)
    assert "wc: /no/such/file: No such file or directory" in q.serial()
    run("sysinfo")
    exited("sysinfo", 0)
    assert "NimbusOS 0.2 x86_64" in q.serial()

    # 5. faults end only the faulting program
    for how, what in [("null", r"Page Fault at address 0 \(page not present, write\)"),
                      ("kernel", r"Page Fault at address 100000 \(protection violation, write\)"),
                      ("div", r"Divide Error at rip="),
                      ("cli", r"General Protection Fault at rip="),
                      ("stack", r"Page Fault at address 80[0-9a-f]+ \(page not present, write\)")]:
        run("crash " + how)
        m = wait_new(r"pid (\d+) 'crash' exited with code 139: " + what, 15)
        wait_new(r"\[term\] program pid %s finished" % m.group(1), 5)
    time.sleep(0.5)
    q.screenshot(f"{S}/user-terminal.png")

    # 6. Ctrl+C on a CPU-bound program; the desktop keeps working meanwhile
    run("clear")
    run("mandel 64 22")
    exited("mandel", 0, 30)
    time.sleep(0.5)
    q.screenshot(f"{S}/user-mandel.png")
    run("spin 30")
    wait_new(r"1 s: \d+ million iterations", 10)
    q.key("meta_l")                       # Start menu opens while spin runs
    q.wait_serial(r"start menu opened", 5)
    q.key("esc")
    q.key("ctrl-c")
    exited("spin", 130, 10)

    # 7. Ctrl+C while a program waits for input; unknown command still reported
    run("guess")
    wait_new(r"Your guess: ", 5)
    time.sleep(0.3)
    q.key("ctrl-c")
    exited("guess", 130, 10)
    # 8. opening a .elf file (as Explorer does on double-click) runs it in a new Terminal
    run("open /Programs/sysinfo.elf")
    exited("sysinfo", 0)
    assert len(re.findall(r'created: "Terminal"', q.serial())) == 2, "second terminal"
    time.sleep(0.5)
    q.screenshot(f"{S}/user-open.png")
    q.key("alt-f4")
    q.wait_serial(r'destroyed: "Terminal"', 5)
    run("nosuchprogram")
    time.sleep(1.0)
    q.screenshot(f"{S}/user-final.png")
    log = q.serial()
    assert "PANIC" not in log

def cat(p): return subprocess.check_output([NFS, "cat", IMG, p]).decode()
todo = cat("/Documents/todo.txt")
assert todo == "buy oat milk\ncall the plumber\n", repr(todo)
assert "0 problem(s)" in subprocess.check_output([NFS, "check", IMG]).decode()
print("user: PASS")
