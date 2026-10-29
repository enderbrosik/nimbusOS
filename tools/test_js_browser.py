#!/usr/bin/env python3
"""Browser JavaScript test: a page with inline scripts, a timer, an onclick
handler and a form submit listener is loaded in the Web Browser.  Scripts must
run on load, the timer must fire, and the page must survive a click."""
import sys, os, subprocess, time, shutil
sys.path.insert(0, os.path.dirname(__file__))
from qemu_harness import Qemu

S = "build/test"
IMG = f"{S}/jsb.img"
subprocess.check_call(["make", "-s", "build"], stdout=subprocess.DEVNULL)
os.makedirs(S, exist_ok=True)
shutil.copy("build/nimbus.img", IMG)
web = subprocess.Popen([sys.executable, "-m", "http.server", "8080", "--bind", "127.0.0.1",
                        "--directory", os.path.join(os.path.dirname(__file__), "testweb")],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(0.5)
try:
    with Qemu([IMG], name="jsb") as q:
        q.wait_serial(r"Desktop ready", timeout=40)
        q.wait_serial(r"DHCP: address 10\.0\.2\.15", 15)
        q.mouse_home()
        q.key("meta_l")
        for _ in range(11): q.key("down")
        q.key("ret")
        q.wait_serial(r'created: "Web Browser"', 5)
        q.key("ctrl-l")
        time.sleep(0.3)
        q.type("http://10.0.2.2:8080/js.html\n")
        q.wait_serial(r"page script ran, title=JS test page", 15)
        q.wait_serial(r"timer fired", 10)
        time.sleep(0.5)
        q.screenshot(f"{S}/js-page.png")
        q.click(160, 213)                               # "Count" (onclick on a button)
        q.wait_serial(r"click 1", 5)
        q.click(160, 321)                               # the clickable div
        q.wait_serial(r"div clicked", 5)
        time.sleep(0.5)
        q.screenshot(f"{S}/js-page2.png")
        q.click(230, 285)                               # the form's text box, then submit via Enter
        q.key("ret")
        q.wait_serial(r"submit q=hi", 5)
        assert "PANIC" not in q.serial()
finally:
    web.terminate()
print("js-browser: PASS (check build/test/js-page.png)")
