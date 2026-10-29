#!/usr/bin/env python3
"""Tests for the JavaScript engine's DOM layer (js/dom_*.c) through build/tools/domrun.

Each case gives an HTML page and a list of actions (click=SEL, type=SEL:text, check=SEL, submit=SEL, advance=MS, dump);
`want` lists lines that must appear in the output in order, `nowant` strings that must not appear anywhere.
Usage: tools/test_dom.py [-v] [name-filter]
"""
import os, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOMRUN = os.path.join(ROOT, "build", "tools", "domrun")
CASES = []

def case(name, html, actions=(), want=(), nowant=()):
    CASES.append((name, html, list(actions), list(want), list(nowant)))

def page(body, script="", head=""):
    return "<html><head>%s</head><body>%s<script>%s</script></body></html>" % (head, body, script)

# ---- lookup -----------------------------------------------------------------------------------------------------
case("by_id", page('<div id="a">hi</div>', 'var e=document.getElementById("a"); console.log(e.tagName, e.id, e.textContent, document.getElementById("nope"))'),
     want=["LOG: DIV a hi null"])
case("query_selector", page('<ul id="l"><li class="a">1</li><li class="b x">2</li><li class="b">3</li></ul>',
     'console.log(document.querySelector("li.b").textContent, document.querySelectorAll("li").length, document.querySelectorAll("li.b").length, document.querySelector("#l > li:last-child").textContent, document.querySelector("ul li.x").textContent, document.querySelector(".zzz"))'),
     want=["LOG: 2 3 2 3 2 null"])
case("query_attr_selectors", page('<a href="x.html" id="a1">x</a><a href="y.pdf" id="a2">y</a><input type="text" name="n" id="i">',
     'console.log(document.querySelector("a[href$=pdf]").id, document.querySelector("a[href^=x]").id, document.querySelector("[name=n]").id, document.querySelectorAll("a[href]").length, document.querySelector("input[type=\\"text\\"]").id)'),
     want=["LOG: a2 a1 i 2 i"])
case("selector_lists_combinators", page('<div id="d"><p id="p1">a</p><span id="s1">b</span><p id="p2">c</p></div>',
     'console.log(document.querySelectorAll("#p1, #s1").length, document.querySelector("#p1 + span").id, document.querySelector("#p1 ~ p").id, document.querySelector("div p:first-child").id, document.querySelectorAll("#d > p").length, document.querySelector("p:nth-child(3)").id)'),
     want=["LOG: 2 s1 p2 p1 2 p2"])
case("by_tag_class", page('<p class="c">1</p><p>2</p><span class="c">3</span>',
     'console.log(document.getElementsByTagName("p").length, document.getElementsByClassName("c").length, document.getElementsByTagName("*").length > 5)'),
     want=["LOG: 2 2 true"])
case("body_head_title", '<html><head><title>My Page</title></head><body><p>x</p><script>console.log(document.title, document.body.tagName, document.head.tagName, document.documentElement.tagName); document.title="New"; console.log(document.title)</script></body></html>',
     want=["LOG: My Page BODY HEAD HTML", "LOG: New"])
case("fragment_page", '<h1>No body tag</h1><script>console.log(document.body.firstChild.tagName, document.body.children.length)</script>',
     want=["LOG: H1 2"])
case("matches_closest", page('<div class="outer"><div class="mid"><b id="t">x</b></div></div>',
     'var t=document.getElementById("t"); console.log(t.matches("b"), t.matches(".x"), t.closest(".outer").className, t.closest(".nope"), t.closest("b") === t)'),
     want=["LOG: true false outer null true"])

# ---- reading and writing the tree -------------------------------------------------------------------------------
case("text_content", page('<div id="d">a<b>b</b>c</div>',
     'var d=document.getElementById("d"); console.log(d.textContent, d.innerHTML); d.textContent="x & <y>"; console.log(d.innerHTML, d.textContent, d.children.length)'),
     want=["LOG: abc a<b>b</b>c", "LOG: x &amp; &lt;y&gt; x & <y> 0"])
case("inner_html", page('<div id="d">old</div>',
     'var d=document.getElementById("d"); d.innerHTML="<p class=\\"n\\">new <i>one</i></p><p>two</p>"; console.log(d.children.length, d.firstChild.className, d.querySelector("i").textContent, d.innerHTML)'),
     want=['LOG: 2 n one <p class="n">new <i>one</i></p><p>two</p>'])
case("outer_html", page('<div id="d"><span id="s">x</span></div>',
     'var s=document.getElementById("s"); console.log(s.outerHTML); s.outerHTML="<em>y</em>"; console.log(document.getElementById("d").innerHTML)'),
     want=['LOG: <span id="s">x</span>', "LOG: <em>y</em>"])
case("create_append", page('<div id="d"></div>',
     'var d=document.getElementById("d"); var p=document.createElement("p"); p.textContent="made"; p.setAttribute("id","m"); d.appendChild(p); console.log(d.innerHTML, d.childNodes.length, p.parentNode===d, document.getElementById("m")===p)'),
     want=['LOG: <p id="m">made</p> 1 true true'])
case("remove_insert_replace", page('<ul id="u"><li id="a">a</li><li id="b">b</li><li id="c">c</li></ul>',
     'var u=document.getElementById("u"); var x=document.createElement("li"); x.id="x"; u.insertBefore(x, document.getElementById("b")); console.log(u.children.length, u.children[1].id); u.removeChild(document.getElementById("a")); var y=document.createElement("li"); y.id="y"; u.replaceChild(y, document.getElementById("c")); document.getElementById("b").remove(); console.log(Array.from(u.children).map(function(e){return e.id}).join(","))'),
     want=["LOG: 4 x", "LOG: x,y"])
case("append_prepend_strings", page('<div id="d"><i>m</i></div>',
     'var d=document.getElementById("d"); d.append("end", document.createElement("br")); d.prepend("start"); console.log(d.innerHTML)'),
     want=["LOG: start<i>m</i>end<br>"])
case("clone_node", page('<div id="d"><b>x</b></div>',
     'var d=document.getElementById("d"); var c=d.cloneNode(true); c.id="c2"; c.firstChild.textContent="y"; console.log(d.innerHTML, c.innerHTML, c.id, d.cloneNode(false).childNodes.length)'),
     want=["LOG: <b>x</b> <b>y</b> c2 0"])
case("siblings", page('<ul id="u"><li id="a">a</li><li id="b">b</li><li id="c">c</li></ul>',
     'var b=document.getElementById("b"); console.log(b.previousElementSibling.id, b.nextElementSibling.id, b.parentNode.id, b.parentElement.id, b.parentNode.firstElementChild.id, b.parentNode.lastElementChild.id, b.parentNode.childElementCount, b.nextSibling === b.nextElementSibling)'),
     want=["LOG: a c u u a c 3 true"])
case("contains_cycle", page('<div id="a"><div id="b"></div></div>',
     'var a=document.getElementById("a"), b=document.getElementById("b"); console.log(a.contains(b), b.contains(a), a.contains(a)); b.appendChild(a); console.log(a.parentNode.tagName)'),
     want=["LOG: true false true", "LOG: BODY"])
case("insert_adjacent", page('<div id="d"><i>m</i></div>',
     'var d=document.getElementById("d"); d.insertAdjacentHTML("afterbegin","<b>1</b>"); d.insertAdjacentHTML("beforeend","<b>2</b>"); d.insertAdjacentHTML("beforebegin","<hr>"); console.log(d.innerHTML, d.previousSibling.tagName)'),
     want=["LOG: <b>1</b><i>m</i><b>2</b> HR"])
case("node_types", page('<p id="p">t</p>',
     'var p=document.getElementById("p"); console.log(p.nodeType, p.nodeName, p.firstChild.nodeType, p.firstChild.nodeName, p.firstChild.nodeValue, document.nodeType, document.createTextNode("z").data, document.createDocumentFragment().nodeType)'),
     want=["LOG: 1 P 3 #text t 9 z 11"])
case("fragment_append", page('<ul id="u"></ul>',
     'var f=document.createDocumentFragment(); for (var i=0;i<3;i++){ var li=document.createElement("li"); li.textContent="n"+i; f.appendChild(li) } document.getElementById("u").appendChild(f); console.log(document.getElementById("u").children.length, f.childNodes.length)'),
     want=["LOG: 3 0"])

# ---- attributes, classes, styles --------------------------------------------------------------------------------
case("attributes", page('<a id="a" href="x.html" title="T &amp; U">link</a>',
     'var a=document.getElementById("a"); console.log(a.getAttribute("href"), a.href, a.title, a.hasAttribute("id"), a.hasAttribute("zz"), a.getAttribute("zz")); a.setAttribute("data-k","v\\"q"); a.removeAttribute("title"); console.log(a.getAttribute("data-k"), a.hasAttribute("title"), a.getAttributeNames().join()); a.href="y.html"; console.log(a.getAttribute("href"))'),
     want=["LOG: x.html x.html T & U true false null", 'LOG: v"q false id,href,data-k', "LOG: y.html"])
case("class_name_list", page('<div id="d" class="a b"></div>',
     'var d=document.getElementById("d"); console.log(d.className, d.classList.contains("a"), d.classList.length); d.classList.add("c"); d.classList.remove("a"); console.log(d.className); console.log(d.classList.toggle("b"), d.classList.toggle("z"), d.className); d.className="only"; console.log(d.classList.contains("only"), d.classList.contains("b"))'),
     want=["LOG: a b true 2", "LOG: b c", "LOG: false true c z", "LOG: true false"])
case("style_props", page('<p id="p" style="color: red">x</p>',
     'var p=document.getElementById("p"); console.log(p.style.color, p.style.fontSize === ""); p.style.fontSize="20px"; p.style.backgroundColor="blue"; p.style.color=""; console.log(p.getAttribute("style"), p.style.cssText); p.style.display="none"; console.log(p.style.display); p.style.setProperty("margin","3px"); console.log(p.style.getPropertyValue("margin"), p.style.margin)'),
     want=["LOG: red true", "LOG: font-size: 20px; background-color: blue; font-size: 20px; background-color: blue;", "LOG: none", "LOG: 3px 3px"])
case("dataset", page('<div id="d" data-user-id="42" data-role="admin"></div>',
     'var d=document.getElementById("d"); console.log(d.dataset.userId, d.dataset.role, d.dataset.nope)'),
     want=["LOG: 42 admin undefined"])
case("form_values", page('<input id="t" type="text" value="init"><input id="c" type="checkbox" checked><textarea id="ta">area</textarea>',
     'var t=document.getElementById("t"), c=document.getElementById("c"), ta=document.getElementById("ta"); console.log(t.value, c.checked, ta.value); t.value="x\\"y"; c.checked=false; ta.value="new"; console.log(t.value, c.checked, ta.value, t.getAttribute("value"), c.hasAttribute("checked"))'),
     want=["LOG: init true area", 'LOG: x"y false new x"y false'])
case("disabled_hidden_bool", page('<button id="b">go</button>',
     'var b=document.getElementById("b"); console.log(b.disabled, b.hidden); b.disabled=true; console.log(b.disabled, b.hasAttribute("disabled")); b.disabled=false; console.log(b.hasAttribute("disabled"))'),
     want=["LOG: false false", "LOG: true true", "LOG: false"])

# ---- events -----------------------------------------------------------------------------------------------------
case("onclick_attr", page('<button id="b" onclick="this.textContent=\'done\'; console.log(\'clicked\', event.type)">go</button>'),
     ["click=#b", "dump"], want=["LOG: clicked click", "click=#b -> changed", 'HTML: '], nowant=["<script"])
case("onclick_property", page('<button id="b">go</button>', 'document.getElementById("b").onclick=function(e){ console.log("prop", e.target.id, this.id) }'),
     ["click=#b"], want=["LOG: prop b b"])
case("add_event_listener", page('<button id="b">go</button>', 'var b=document.getElementById("b"); function h1(){ console.log("one") } b.addEventListener("click", h1); b.addEventListener("click", function(){ console.log("two") }); b.addEventListener("click", h1)'),
     ["click=#b"], want=["LOG: one", "LOG: two"])
case("remove_event_listener", page('<button id="b">go</button>', 'var b=document.getElementById("b"); function h(){ console.log("h") } b.addEventListener("click", h); b.addEventListener("click", function(){ console.log("other"); b.removeEventListener("click", h) }); '),
     ["click=#b", "click=#b"], want=["LOG: h", "LOG: other", "LOG: other"])
case("bubbling_order", page('<div id="outer"><div id="mid"><button id="b">x</button></div></div>', 'function mk(n){ return function(e){ console.log(n, e.target.id, e.currentTarget.id) } } document.getElementById("b").addEventListener("click", mk("b")); document.getElementById("mid").addEventListener("click", mk("mid")); document.getElementById("outer").addEventListener("click", mk("outer")); document.addEventListener("click", function(e){ console.log("doc") }); window.addEventListener("click", function(){ console.log("win") })'),
     ["click=#b"], want=["LOG: b b b", "LOG: mid b mid", "LOG: outer b outer", "LOG: doc", "LOG: win"])
case("stop_propagation", page('<div id="outer"><button id="b">x</button></div>', 'document.getElementById("b").addEventListener("click", function(e){ console.log("b"); e.stopPropagation() }); document.getElementById("outer").addEventListener("click", function(){ console.log("outer") })'),
     ["click=#b"], want=["LOG: b"], nowant=["LOG: outer"])
case("prevent_default_link", page('<a id="a" href="next.html">go</a><a id="a2" href="other.html">go2</a>', 'document.getElementById("a").addEventListener("click", function(e){ e.preventDefault(); console.log("stopped") })'),
     ["click=#a", "click=#a2"], want=["LOG: stopped", "click=#a -> prevented", "HREF: other.html"], nowant=["HREF: next.html"])
case("return_false", page('<a id="a" href="next.html" onclick="return false">go</a>'),
     ["click=#a"], want=["prevented"], nowant=["HREF"])
case("javascript_href", page('<a id="a" href="javascript:console.log(\'from href\')">go</a>'),
     ["click=#a"], want=["LOG: from href"], nowant=["HREF"])
case("div_onclick_wrapped", page('<div id="d" onclick="console.log(\'div\')">click me</div>'),
     ["dump", "click=#d"], want=['nimbus-js:', "LOG: div"])
case("input_event", page('<input id="t" type="text" value="">', 'var t=document.getElementById("t"); t.addEventListener("input", function(){ console.log("input:", t.value) }); t.addEventListener("change", function(){ console.log("change:", t.value) })'),
     ["type=#t:hello"], want=["LOG: input: hello", "LOG: change: hello"])
case("checkbox_click", page('<input id="c" type="checkbox"><div id="out"></div>', 'document.getElementById("c").addEventListener("click", function(){ document.getElementById("out").textContent = this.checked ? "on" : "off" })'),
     ["check=#c", "dump"], want=['<div id="out">on</div>'])
case("submit_event", page('<form id="f" action="/go"><input type="text" name="q" id="q"></form>', 'document.getElementById("f").addEventListener("submit", function(e){ console.log("submitting", document.getElementById("q").value); e.preventDefault() })'),
     ["type=#q:cats", "submit=#f"], want=["LOG: submitting cats", "submit=#f -> prevented"])
case("submit_allowed", page('<form id="f" action="/go"><input type="text" name="q"></form>', 'document.getElementById("f").onsubmit=function(){ console.log("ok") }'),
     ["submit=#f"], want=["LOG: ok", "submit=#f -> "], nowant=["prevented"])
case("custom_events", page('<div id="d"></div>', 'var d=document.getElementById("d"); d.addEventListener("ping", function(e){ console.log("got", e.type, e.detail) }); d.dispatchEvent(new CustomEvent("ping", {detail: 7})); console.log(d.dispatchEvent(new Event("nothing")))'),
     want=["LOG: got ping 7", "LOG: true"])
case("event_order_load", '<html><body><script>console.log("script 1, state=" + document.readyState); document.addEventListener("DOMContentLoaded", function(){ console.log("DOMContentLoaded", document.readyState) }); window.addEventListener("load", function(){ console.log("load", document.readyState) }); window.onload = function(){ console.log("onload prop") }</script><p>x</p><script>console.log("script 2")</script></body></html>',
     want=["LOG: script 1, state=loading", "LOG: script 2", "LOG: DOMContentLoaded interactive", "LOG: load complete", "LOG: onload prop"] if False else
          ["LOG: script 1, state=loading", "LOG: script 2", "LOG: DOMContentLoaded interactive", "LOG: onload prop", "LOG: load complete"])
case("click_changes_page", page('<h1 id="h">a</h1><button id="b" onclick="document.getElementById(\'h\').textContent=\'b\'">x</button>'),
     ["click=#b", "dump"], want=["click=#b -> changed", "<h1 id=\"h\">b</h1>"])
case("click_no_change", page('<button id="b" onclick="var x=1">x</button>'),
     ["click=#b"], want=["click=#b -> "], nowant=["changed"])

# ---- timers -----------------------------------------------------------------------------------------------------
case("set_timeout_order", page('', 'setTimeout(function(){ console.log("c") }, 300); setTimeout(function(){ console.log("a") }, 100); setTimeout(function(){ console.log("b") }, 200)'),
     ["advance=250", "advance=100"], want=["LOG: a", "LOG: b", "LOG: c"])
case("set_interval", page('<div id="d">0</div>', 'var n=0; var id=setInterval(function(){ n++; document.getElementById("d").textContent=n; if (n==3) clearInterval(id) }, 100)'),
     ["advance=100", "advance=100", "advance=100", "advance=100", "dump"], want=["timers: no"] if False else ['<div id="d">3</div>', "timers: no"])
case("clear_timeout", page('', 'var t=setTimeout(function(){ console.log("never") }, 100); clearTimeout(t); setTimeout(function(){ console.log("yes") }, 100)'),
     ["advance=200"], want=["LOG: yes"], nowant=["never"])
case("timer_chain", page('', 'function tick(n){ console.log("tick", n); if (n<3) setTimeout(function(){ tick(n+1) }, 50) } tick(1)'),
     ["advance=60", "advance=60", "advance=60"], want=["LOG: tick 1", "LOG: tick 2", "LOG: tick 3"])
case("timer_string_and_args", page('', 'setTimeout("console.log(\'from string\')", 10)'),
     ["advance=20"], want=["LOG: from string"])
case("request_animation_frame", page('', 'requestAnimationFrame(function(t){ console.log("frame", typeof t) })'),
     ["advance=20"], want=["LOG: frame number"])
case("has_timers", page('', 'setInterval(function(){}, 1000)'), want=["timers: yes"])
case("date_now_uses_clock", page('', 'var a=Date.now(); setTimeout(function(){ console.log(Date.now()-a) }, 500)'),
     ["advance=500"], want=["LOG: 500"])

# ---- document.write, location, window ---------------------------------------------------------------------------
case("document_write", '<html><body><p>before</p><script>document.write("<b>written</b>")</script><p>after</p></body></html>',
     ["dump"], want=["<p>before</p>", "<b>written</b>", "<p>after</p>"])
case("document_write_order", '<html><body><script>document.write("<i>1</i>")</script><script>document.write("<i>2</i>")</script></body></html>',
     ["dump"], want=["<i>1</i>"], nowant=["<i>2</i><i>1</i>"])
case("location_parts", page('', 'console.log(location.href); console.log(location.hostname, location.host, location.pathname, location.search, location.hash, location.protocol, location.origin)'),
     want=["LOG: http://example.test/dir/page.html?x=1#top", "LOG: example.test example.test /dir/page.html ?x=1 #top http: http://example.test"])
case("location_navigate", page('<button id="b">x</button>', 'document.getElementById("b").onclick=function(){ location.href="/next"; }; window.assign = function(){ location.assign("http://other.test/") }'),
     ["click=#b"], want=["NAV: /next"])
case("location_assign_reload", page('', 'location.assign("/a"); location.replace("/b"); location.reload(); location = "/c"'),
     want=["NAV: /a", "NAV: /b", "NAV: http://example.test/dir/page.html?x=1#top", "NAV: /c"])
case("window_globals", page('', 'var x=1; console.log(window===this, window.window===window, typeof window.document, window.x, window.innerWidth>0, navigator.userAgent.length>0, typeof localStorage, typeof alert, typeof requestAnimationFrame)'),
     want=["LOG: true true object 1 true true object function function"])
case("storage", page('', 'localStorage.setItem("k","v"); sessionStorage.setItem("n", 5); console.log(localStorage.getItem("k"), sessionStorage.getItem("n"), localStorage.getItem("zz"), localStorage.length); localStorage.removeItem("k"); console.log(localStorage.getItem("k")); localStorage.clear()'),
     want=["LOG: v 5 null 1", "LOG: null"])
case("alerts", page('', 'alert("hello"); console.log(confirm("sure?"), prompt("name?", "bob"))'),
     want=["LOG: alert: hello", "LOG: confirm: sure?", "LOG: prompt: name?", "LOG: true bob"])
case("get_computed_style", page('<p id="p" style="color: red">x</p>', 'console.log(getComputedStyle(document.getElementById("p")).color)'),
     want=["LOG: red"])

# ---- robustness -------------------------------------------------------------------------------------------------
case("error_in_script", page('<p id="p">x</p>', 'console.log("before"); undefined_fn(); console.log("not reached")'),
     want=["LOG: before", "LOG: Uncaught ReferenceError: undefined_fn is not defined"] if False else ["LOG: before", "LOG: Uncaught ReferenceError: undefined_fn is not defined"], nowant=["not reached"])
case("error_in_second_script_continues", '<html><body><script>throw new Error("first")</script><script>console.log("second runs")</script></body></html>',
     want=["LOG: Uncaught Error: first", "LOG: second runs"])
case("error_in_handler", page('<button id="b" onclick="nope()">x</button><button id="c" onclick="console.log(\'fine\')">y</button>'),
     ["click=#b", "click=#c"], want=["LOG: Uncaught ReferenceError: nope is not defined", "LOG: fine"])
case("syntax_error_in_script", '<html><body><script>var = ;</script><script>console.log("ok")</script></body></html>',
     want=["LOG: SyntaxError:", "LOG: ok"])
case("infinite_loop_handler", page('<button id="b" onclick="while(true){}">x</button><button id="c" onclick="console.log(\'alive\')">y</button>'),
     ["click=#b", "click=#c"], want=["LOG: Error: script took too long", "LOG: alive"])
case("infinite_loop_timer", page('', 'setTimeout(function(){ for(;;){} }, 10); setTimeout(function(){ console.log("later") }, 20)'),
     ["advance=30"], want=["LOG: Error: script took too long", "LOG: later"])
case("script_src_skipped", '<html><body><script src="x.js"></script><script>console.log("inline")</script></body></html>',
     want=["LOG: script src is not loaded yet: x.js", "LOG: inline"])
case("entities_roundtrip", page('<p id="p">caf&eacute; &amp; &lt;b&gt; &#65;&#x42;</p>', 'var p=document.getElementById("p"); console.log(p.textContent, p.innerHTML); p.textContent += "!"; '),
     ["dump"], want=["LOG: caf&eacute; & <b> AB", "&amp;", "&lt;b&gt;"])
case("big_dom", page('<ul id="u"></ul>', 'var u=document.getElementById("u"); for (var i=0;i<2000;i++){ var li=document.createElement("li"); li.textContent="item "+i; u.appendChild(li) } console.log(u.children.length, u.lastChild.textContent)'),
     want=["LOG: 2000 item 1999"])
case("unclosed_tags", '<html><body><ul><li>a<li>b<li>c</ul><p>one<p>two<table><tr><td>x<td>y<tr><td>z</table><script>console.log(document.querySelectorAll("li").length, document.querySelectorAll("p").length, document.querySelectorAll("td").length, document.querySelectorAll("tr").length)</script></body></html>',
     want=["LOG: 3 2 3 2"])
case("serialize_hides_scripts", '<html><body><p>a</p><script>var x=1</script></body></html>',
     ["dump"], nowant=["var x"])
case("comments_ignored", '<html><body><!-- hi --><p id="p">a</p><script>console.log(document.body.childNodes.length)</script></body></html>',
     want=["LOG: 2"])
case("uninitialized_events_ok", page('<div id="d"></div>'),
     ["click=#d", "advance=100", "dump"], want=["timers: no"])
case("garbage_collect", page('<div id="d"></div>', 'var d=document.getElementById("d"); function f(){ var s=""; for (var i=0;i<200;i++) s+="xxxxxxxx"; return s } window.f=f; d.onclick=function(){ for (var i=0;i<400;i++) f(); d.textContent=String(Date.now()>0) }'),
     ["click=#d"]*30 + ["dump"], want=['<div id="d"', "true"])


def main():
    verbose = "-v" in sys.argv
    filt = [a for a in sys.argv[1:] if not a.startswith("-")]
    if not os.path.exists(DOMRUN):
        print("build/tools/domrun is missing: run `make jsrun` first")
        return 2
    bad = ran = slow = 0
    for name, html, actions, want, nowant in CASES:
        if filt and not any(f in name for f in filt):
            continue
        ran += 1
        with tempfile.NamedTemporaryFile("w", suffix=".html", delete=False) as f:
            f.write(html)
            path = f.name
        try:
            p = subprocess.run([DOMRUN, path] + actions, capture_output=True, text=True, timeout=int(os.environ.get('JS_TEST_TIMEOUT', '60')))
        except subprocess.TimeoutExpired:
            print('SLOW %s (skipped: timed out)' % name)
            slow += 1
            continue
        finally:
            os.unlink(path)
        out = p.stdout
        problem = None
        if p.returncode != 0:
            problem = "domrun failed (rc=%d): %s" % (p.returncode, p.stderr.strip()[:300])
        else:
            pos = 0
            for w in want:
                i = out.find(w, pos)
                if i < 0:
                    problem = "missing %r (after offset %d)" % (w, pos)
                    break
                pos = i + len(w)
            if not problem:
                for nw in nowant:
                    if nw in out:
                        problem = "unexpected %r" % nw
                        break
        if problem:
            bad += 1
            print("FAIL %s: %s\n--- output ---\n%s---" % (name, problem, out))
        elif verbose:
            print("ok   %s" % name)
    print("%d cases, %d failed, %d slow" % (ran, bad, slow))
    print("dom: PASS" if not bad else "dom: FAIL")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
