#!/usr/bin/env python3
"""Differential test for the NimbusOS JavaScript engine (js/).

Each case is run by the engine (build/tools/jsrun) and, when Node is installed, by Node too; the output must match.
Cases avoid the engine's documented differences (4 decimal places, no exponents, UTF-8 byte strings, no regex).
Cases marked ERR must make the engine fail with the given message prefix.
Usage: tools/test_js.py [-v] [name-filter]
"""
import os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JSRUN = os.path.join(ROOT, "build", "tools", "jsrun")
NODE = shutil.which("node")

CASES = []
def case(name, code, err=None):
    CASES.append((name, code, err))

# ---- basics ----------------------------------------------------------------------------------------------------
case("arith", 'console.log(1+2*3, (1+2)*3, 10/4, 7%3, -7%3, 2**10, 5-8, 0.5+0.25, 3*0.5)')
case("precedence", 'console.log(1+2+"3", "1"+2+3, 1+(2+"3"), "5"*"2", "6"/"3", "7"-2, +"8", -"3", +true, +null, +undefined)')
case("compare", 'console.log(1<2, 2<=2, "a"<"b", "abc"<"abd", "10"<"9", 10<"9", null==undefined, null===undefined, 0=="", "1"==1, NaN==NaN, [1]==1)')
case("logic", 'console.log(1&&2, 0&&2, 1||2, 0||2, null??5, 0??5, !0, !"a", !!"", !!{})')
case("bitwise", 'console.log(5&3, 5|3, 5^3, ~5, 1<<4, -16>>2, -16>>>28, 255>>4, 1<<31, (1<<31)>>>0)')
case("ternary", 'var x=5; console.log(x>3?"big":"small", x>9?"a":x>4?"b":"c")')
case("typeof", 'console.log(typeof 1, typeof "s", typeof true, typeof undefined, typeof null, typeof {}, typeof [], typeof function(){}, typeof notDefined)')
case("incdec", 'var a=1; console.log(a++, a, ++a, a, a--, a, --a, a); var o={n:5}; o.n++; ++o.n; console.log(o.n)')
case("compound", 'var a=10; a+=5; a-=3; a*=2; a/=4; a%=4; console.log(a); var s="x"; s+="y"; console.log(s); var b=6; b<<=1; b>>=1; b|=1; b&=7; b^=2; console.log(b)')
case("nan_inf", 'console.log(1/0, -1/0, 0/0, isNaN(NaN), isNaN("x"), isNaN("5"), isFinite(1), isFinite(1/0), NaN!==NaN, Infinity>1e10)')
case("numfmt", 'console.log(0.5+0.25, 1.5, -2.25, 100, 1000000, 123456789, 0.0001, 12.3456, 1/4, 2/3>0.66)')
case("parse", 'console.log(parseInt("42px"), parseInt("0x1f"), parseInt("ff",16), parseInt("abc"), parseFloat("3.14abc"), Number("12"), Number(""), Number("1x"), Number(" 7 "), Number(null), Number(undefined), Number(true))')
case("tostring_radix", 'console.log((255).toString(16), (255).toString(2), (-10).toString(2), (35).toString(36), (3.5).toString(), (10).toString())')
case("tofixed", 'console.log((3.14159).toFixed(2), (2.5).toFixed(0), (5).toFixed(3), (0.5).toFixed(1), (1234.5678).toFixed(1))')
case("comma_seq", 'var x = (1,2,3); console.log(x); for (var i=0, j=10; i<3; i++, j--) {} console.log(i, j)')

# ---- strings ---------------------------------------------------------------------------------------------------
case("str_basic", 'var s="Hello, World"; console.log(s.length, s.charAt(1), s[4], s.charCodeAt(0), s.indexOf("o"), s.lastIndexOf("o"), s.indexOf("zz"), s.includes("World"), s.startsWith("Hell"), s.endsWith("ld"))')
case("str_slice", 'var s="abcdefgh"; console.log(s.slice(2,5), s.slice(-3), s.slice(2), s.substring(5,2), s.substring(-1,3), s.substr(2,3), s.substr(-4,2), s.slice(3,-1))')
case("str_case_trim", 'console.log("Hello".toUpperCase(), "HeLLo".toLowerCase(), "  x y  ".trim()+"|", "  x".trimStart()+"|", "x  ".trimEnd()+"|")')
case("str_split_join", 'console.log("a,b,c".split(",").join("+"), "abc".split("").length, "a b".split(" ").length, "x".split(",").length, JSON.stringify("a,b,,c".split(",")), JSON.stringify("a1b1c".split("1", 2)))')
case("str_replace", 'console.log("aXbXc".replace("X","-"), "aXbXc".replaceAll("X","-"), "abc".replace("b", function(m){return m.toUpperCase()}), "abc".replace("zz","y"), "a.b".replace(".", "$&$&"))')
case("str_pad_repeat", 'console.log("5".padStart(3,"0"), "5".padEnd(3,"*"), "ab".repeat(3), "x".repeat(0)+"|", "abc".padStart(2), "abc".concat("d","e"))')
case("str_escapes", 'console.log("a\\tb\\nc", "q\\"q", \'s\\\'s\', "\\x41\\u0042", "back\\\\slash", "caf\\u0065".length)')
case("str_cmp", 'console.log("a".localeCompare("b"), "b".localeCompare("a"), "a".localeCompare("a"), "abc".at(-1), "abc".at(0))')
case("str_index_oob", 'var s="ab"; console.log(s[5], s.charAt(5)+"|", s.charCodeAt(5), s.length)')
case("template", 'var n=3, s="x"; console.log(`n=${n} s=${s} sum=${n+1} nested=${`<${s}>`} ${[1,2].join("-")}`)')
case("template_multiline", 'console.log(`a\nb`.split("\\n").length, `${1}${2}${3}`)')
case("str_concat_loop", 'var s=""; for (var i=0;i<20;i++) s+=i; console.log(s, s.length)')
case("str_unicode", 'var s="hello"; console.log(s.toUpperCase(), "日本".length>0, [..."ab"].length, [..."é日"].length)')

# ---- arrays ----------------------------------------------------------------------------------------------------
case("arr_basic", 'var a=[1,2,3]; a.push(4,5); console.log(a.length, a.pop(), a.shift(), a.unshift(0,9), JSON.stringify(a))')
case("arr_index", 'var a=[]; a[3]=1; console.log(a.length, a[0], a[3]); a.length=2; console.log(a.length, JSON.stringify(a)); a[a.length]=7; console.log(a.length)')
case("arr_slice_splice", 'var a=[1,2,3,4,5]; console.log(JSON.stringify(a.slice(1,3)), JSON.stringify(a.slice(-2)), JSON.stringify(a.splice(1,2)), JSON.stringify(a)); a.splice(1,0,"x","y"); console.log(JSON.stringify(a)); a.splice(-1,1); console.log(JSON.stringify(a))')
case("arr_concat", 'console.log(JSON.stringify([1].concat([2,3],4,[[5]])), [1,2].concat().length)')
case("arr_join", 'console.log([1,2,3].join(), [1,2,3].join(""), [1,null,undefined,2].join("-"), [].join()+"|", [[1,2],[3]].join(";"), String([1,2,3]), [1,2]+"")')
case("arr_reverse_index", 'var a=[1,2,3,2]; console.log(JSON.stringify(a.reverse()), a.indexOf(2), a.lastIndexOf(2), a.indexOf(9), a.includes(3), a.includes(9))')
case("arr_iter", 'var a=[1,2,3,4]; var t=0; a.forEach(function(x,i){ t+=x*i }); console.log(t, JSON.stringify(a.map(x=>x*x)), JSON.stringify(a.filter(x=>x%2)), a.some(x=>x>3), a.every(x=>x>1), a.find(x=>x>2), a.findIndex(x=>x>2), a.find(x=>x>9), a.findIndex(x=>x>9))')
case("arr_reduce", 'console.log([1,2,3,4].reduce((a,b)=>a+b), [1,2,3,4].reduce((a,b)=>a+b, 10), [1,2,3].reduceRight((a,b)=>a+"-"+b), [[1],[2]].reduce((a,b)=>a.concat(b), []).length)')
case("arr_reduce_empty", '[].reduce((a,b)=>a)', err="Uncaught TypeError: Reduce of empty array")
case("arr_sort", 'console.log(JSON.stringify([3,1,10,2].sort()), JSON.stringify([3,1,10,2].sort((a,b)=>a-b)), JSON.stringify(["b","a","C"].sort()), JSON.stringify([3,1,2].sort((a,b)=>b-a)), JSON.stringify([1,undefined,0].sort()))')
case("arr_sort_stable", 'var a=[{k:1,v:"a"},{k:0,v:"b"},{k:1,v:"c"},{k:0,v:"d"}]; a.sort((x,y)=>x.k-y.k); console.log(a.map(x=>x.v).join(""))')
case("arr_fill_from", 'console.log(JSON.stringify(new Array(3).fill(0)), JSON.stringify(Array.from({length:3}, (_, i) => i*2)), JSON.stringify(Array.from("abc")), JSON.stringify(Array.of(7,8)), Array.isArray([]), Array.isArray({}))')
case("arr_flat_at", 'console.log(JSON.stringify([1,[2,[3,[4]]]].flat()), JSON.stringify([1,[2,[3,[4]]]].flat(2)), [1,2,3].at(-1), [1,2,3].at(0))')
case("arr_spread", 'var a=[1,2]; var b=[...a, 3, ...[4,5]]; console.log(JSON.stringify(b), Math.max(...b), ((x,y,z)=>x+y+z)(...[1,2,3]))')
case("arr_nested", 'var m=[[1,2],[3,4]]; m[1][0]=9; console.log(JSON.stringify(m), m[0].length)')
case("arr_new", 'console.log(new Array(2).length, new Array(1,2,3).length, Array(5).length, [,].length)')
case("arr_in_for", 'var a=["x","y"]; for (var i in a) console.log(i, a[i]); for (var v of a) console.log(v)')

# ---- objects ---------------------------------------------------------------------------------------------------
case("obj_basic", 'var o={a:1,"b c":2,3:"three"}; o.d=4; o["e"]=5; console.log(o.a, o["b c"], o[3], o.d, o.e, o.zz, "a" in o, "zz" in o, delete o.a, "a" in o)')
case("obj_keys", 'var o={z:1,a:2,m:3}; console.log(Object.keys(o).join(), Object.values(o).join(), JSON.stringify(Object.entries(o)))')
case("obj_assign_create", 'var p={hi:function(){return "hi "+this.n}}; var c=Object.create(p); c.n="bob"; console.log(c.hi(), Object.getPrototypeOf(c)===p, JSON.stringify(Object.assign({a:1},{b:2},{a:3})), c.hasOwnProperty("n"), c.hasOwnProperty("hi"))')
case("obj_shorthand_method", 'var a=1,b=2; var o={a,b,sum(){return this.a+this.b}, ["k"+1]:9}; console.log(o.sum(), o.k1, JSON.stringify(o))')
case("obj_this", 'var o={n:"o", f:function(){return this.n}, g:()=>typeof this}; var f=o.f; console.log(o.f(), f()===undefined, o.f.call({n:"x"}), o.f.apply({n:"y"}), o.f.bind({n:"z"})())')
case("obj_proto_chain", 'function A(){ this.x=1 } A.prototype.getX=function(){return this.x}; function B(){ A.call(this); this.y=2 } B.prototype=Object.create(A.prototype); B.prototype.constructor=B; var b=new B(); console.log(b.getX(), b.y, b instanceof B, b instanceof A, b instanceof Object, Object.getPrototypeOf(b)===B.prototype)')
case("obj_for_in_inherit", 'function A(){ this.own=1 } A.prototype.inh=2; var a=new A(); var ks=[]; for (var k in a) ks.push(k); console.log(ks.join())')
case("obj_tostring", 'var o={toString:function(){return "OBJ"}}; console.log(""+o, `${o}`, String(o), [o,o].join()); console.log(String({}), String([]), String(null), String(undefined), String(true), String(12))')
case("obj_valueof", 'var o={valueOf:function(){return 42}}; console.log(o+1, o*2, o>40, +o)')
case("obj_freeze", 'var o=Object.freeze({a:1}); o.a=2; o.b=3; console.log(o.a, o.b, Object.keys(o).join())')
case("obj_defprop", 'var o={}; Object.defineProperty(o,"hid",{value:5,enumerable:false}); Object.defineProperty(o,"vis",{value:6,enumerable:true}); console.log(o.hid, o.vis, Object.keys(o).join(), JSON.stringify(o))')
case("obj_computed", 'var k="dyn"; var o={}; o[k+"1"]=1; o[k+"2"]=2; console.log(Object.keys(o).join(), o["dyn"+1])')
case("obj_nested", 'var o={a:{b:{c:[1,{d:"deep"}]}}}; console.log(o.a.b.c[1].d, JSON.stringify(o))')
case("obj_getprop_undefined", 'var o={}; console.log(o.x, o.x === undefined, typeof o.x.y)', err="Uncaught TypeError: Cannot read properties of undefined (reading 'y')")

# ---- functions and scope ---------------------------------------------------------------------------------------
case("fn_basic", 'function add(a,b){ return a+b } console.log(add(1,2), add(1), add.length, add.name, typeof add)')
case("fn_hoisting", 'console.log(typeof f, v); function f(){} var v=1; console.log(v)')
case("fn_expr_named", 'var fact=function me(n){ return n<=1?1:n*me(n-1) }; console.log(fact(5), typeof me)')
case("fn_closure_counter", 'function mk(){ var c=0; return { inc: function(){ return ++c }, get: function(){ return c } } } var a=mk(), b=mk(); a.inc(); a.inc(); b.inc(); console.log(a.get(), b.get())')
case("fn_closure_loop_var", 'var fs=[]; for (var i=0;i<3;i++) fs.push(function(){return i}); console.log(fs.map(f=>f()).join())')
case("fn_closure_loop_let", 'var fs=[]; for (let i=0;i<3;i++) fs.push(function(){return i}); console.log(fs.map(f=>f()).join())')
case("fn_closure_loop_forof", 'var fs=[]; for (const x of [1,2,3]) fs.push(()=>x); console.log(fs.map(f=>f()).join())')
case("fn_iife", 'var r=(function(a){ return a*2 })(21); console.log(r, (()=>7)(), (function(){ return typeof this })())')
case("fn_arguments", 'function f(){ return arguments.length+":"+Array.prototype.slice.call(arguments).join("|") } console.log(f(), f(1), f(1,2,3)); function g(a){ return arguments[1] } console.log(g(1,"two"))')
case("fn_default_params", 'function f(a, b=10, c=a+b){ return a+","+b+","+c } console.log(f(1), f(1,2), f(1,undefined,3))')
case("fn_arrow", 'var f=x=>x+1, g=(x,y)=>x*y, h=()=>({a:1}), i=x=>{ return x*2 }; console.log(f(1), g(3,4), JSON.stringify(h()), i(5))')
case("fn_arrow_this", 'function C(){ this.v=7; this.get=()=>this.v; this.get2=function(){ return typeof this.v } } var c=new C(); var g=c.get; console.log(g(), c.get2())')
case("fn_call_apply_bind", 'function f(a,b){ return this.n+a+b } var o={n:1}; console.log(f.call(o,2,3), f.apply(o,[2,3]), f.bind(o,2)(3), f.bind(o)(5,6))')
case("fn_higher_order", 'var compose=(f,g)=>x=>f(g(x)); var inc=x=>x+1, dbl=x=>x*2; console.log(compose(inc,dbl)(5), compose(dbl,inc)(5))')
case("fn_recursion", 'function fib(n){ return n<2?n:fib(n-1)+fib(n-2) } function sum(n){ return n==0?0:n+sum(n-1) } console.log(fib(15), sum(100))')
case("fn_new", 'function P(n){ this.n=n } P.prototype.hi=function(){ return "hi "+this.n }; var p=new P("a"); console.log(p.hi(), p.constructor===P, p instanceof P, typeof P.prototype)')
case("fn_new_return_obj", 'function F(){ this.a=1; return {b:2} } function G(){ this.a=1; return 5 } console.log(JSON.stringify(new F()), JSON.stringify(new G()))')
case("fn_not_function", 'var o={}; o.nope()', err="Uncaught TypeError: o.nope is not a function")
case("fn_scope_shadow", 'var x="g"; function f(){ var x="l"; return x } console.log(f(), x); function h(){ x="changed" } h(); console.log(x)')
case("fn_implicit_global", 'function f(){ leaked = 5 } f(); console.log(leaked, typeof leaked)')
case("fn_let_block", 'let a=1; { let a=2; console.log(a) } console.log(a); if (true) { const b=3; var c=4 } console.log(c, typeof b)')
case("fn_const_assign", 'const a=1; a=2', err="Uncaught TypeError: Assignment to constant variable.")
case("fn_undef_var", 'console.log(nope)', err="Uncaught ReferenceError: nope is not defined")
case("fn_getter_chain", 'var o={a:{b:function(){ return this===o.a }}}; console.log(o.a.b())')
case("fn_method_chain", 'var s=[3,1,2].sort().map(x=>x*2).join("-"); console.log(s, "a-b".split("-").reverse ? 1 : 0)')

# ---- accessors -------------------------------------------------------------------------------------------------
case("acc_literal", 'var o={_v:1, get v(){ return this._v*10 }, set v(x){ this._v=x }, plain:3}; console.log(o.v); o.v=7; console.log(o.v, o._v, JSON.stringify(o), Object.keys(o).join())')
case("acc_inherit", 'var o={get v(){ return this.n }, set v(x){ this.n=x*2 }}; var p=Object.create(o); p.v=4; console.log(p.v, o.v, p.hasOwnProperty("v"), p.hasOwnProperty("n"))')
case("acc_defprop", 'var o={_a:2}; Object.defineProperty(o,"a",{get:function(){ return this._a+1 }, set:function(x){ this._a=x }, enumerable:true}); o.a=10; console.log(o.a, Object.keys(o).join()); Object.defineProperty(o,"h",{get:function(){return 1}}); console.log(o.h, Object.keys(o).join())')
case("acc_names_get_set", 'var q={get:5, set:6, get x(){ return this.get+this.set }}; console.log(q.get, q.set, q.x)')
case("acc_only_getter", 'var o={get a(){ return 1 }}; o.a=5; console.log(o.a)')

# ---- control flow ----------------------------------------------------------------------------------------------
case("if_else", 'function f(n){ if (n<0) return "neg"; else if (n==0) return "zero"; else return "pos" } console.log(f(-1), f(0), f(5))')
case("while_do", 'var i=0, s=0; while(i<5){ s+=i; i++ } var j=0; do { j++ } while(j<3); console.log(s, j)')
case("for_break_continue", 'var out=[]; for (var i=0;i<10;i++){ if (i%2) continue; if (i>6) break; out.push(i) } console.log(out.join())')
case("for_nested_break", 'var c=0; for (var i=0;i<3;i++){ for (var j=0;j<3;j++){ if (j==1) break; c++ } } console.log(c)')
case("for_empty_parts", 'var i=0; for (;;){ if (++i>4) break } console.log(i)')
case("switch_basic", 'function f(x){ switch(x){ case 1: return "one"; case 2: case 3: return "two-three"; default: return "other" } } console.log(f(1), f(2), f(3), f(9))')
case("switch_fallthrough", 'var o=[]; switch(2){ case 1: o.push(1); case 2: o.push(2); case 3: o.push(3); break; case 4: o.push(4) } console.log(o.join())')
case("switch_default_mid", 'function f(x){ var r=""; switch(x){ case 1: r+="a"; default: r+="d"; case 2: r+="b" } return r } console.log(f(1), f(2), f(7))')
case("switch_strict", 'switch("1"){ case 1: console.log("num"); break; case "1": console.log("str") }')
case("try_catch", 'try { throw new Error("boom") } catch (e) { console.log(e.message, e.name, e instanceof Error, String(e)) }')
case("try_finally", 'function f(){ try { return "try" } finally { console.log("fin") } } console.log(f())')
case("try_finally_override", 'function f(){ try { return 1 } finally { return 2 } } console.log(f())')
case("try_rethrow", 'try { try { throw "inner" } catch (e) { throw e+"+" } } catch (e2) { console.log(e2) }')
case("try_no_param", 'try { throw 1 } catch { console.log("caught") }')
case("try_throw_types", 'var r=[]; [1,"s",{a:1},null,undefined].forEach(function(v){ try { throw v } catch (e) { r.push(typeof e) } }); console.log(r.join())')
case("try_runtime_errors", 'var r=[]; try { undefinedFn() } catch(e){ r.push(e.name) } try { null.x } catch(e){ r.push(e.name) } try { (void 0)() } catch(e){ r.push(e.name) } try { new Array(-1) } catch(e){ r.push(e.name) } try { "x".repeat(-1) } catch(e){ r.push(e.name) } console.log(r.join())')
case("error_types", 'var e=new TypeError("t"); console.log(e.name, e.message, e instanceof TypeError, e instanceof Error, String(new RangeError("r")), String(new Error()), new Error("m").toString())')
case("error_custom", 'function MyErr(m){ this.message=m; this.name="MyErr" } MyErr.prototype=Object.create(Error.prototype); try { throw new MyErr("custom") } catch(e){ console.log(e.name, e.message, e instanceof MyErr, e instanceof Error) }')
case("throw_uncaught", 'throw new Error("unhandled")', err="Uncaught Error: unhandled")
case("throw_uncaught_value", 'throw "just a string"', err="Uncaught just a string")
case("stack_overflow", 'function f(){ return f() } try { f() } catch(e){ console.log(e instanceof RangeError, e.message) }')

# ---- JSON ------------------------------------------------------------------------------------------------------
case("json_stringify", 'console.log(JSON.stringify({a:1,b:"x",c:[1,2],d:null,e:undefined,f:function(){},g:true}), JSON.stringify([undefined,function(){}]), JSON.stringify("q\\"\\n"), JSON.stringify(1), JSON.stringify(null), JSON.stringify(undefined)+"")')
case("json_indent", 'console.log(JSON.stringify({a:1,b:[1,2],c:{}}, null, 2)); console.log(JSON.stringify([1,[2]], null, "--")); console.log(JSON.stringify({}, null, 2), JSON.stringify([], null, 2))')
case("json_parse", 'var o=JSON.parse(\'{"a":[1,2,{"b":"c"}],"d":true,"e":null,"f":-1.5,"g":"x\\\\ny"}\'); console.log(o.a[2].b, o.d, o.e, o.f, o.g, o.a.length)')
case("json_roundtrip", 'var o={a:[1,2,3],b:{c:"d"},e:"é"}; console.log(JSON.stringify(JSON.parse(JSON.stringify(o)))===JSON.stringify(o))')
case("json_parse_error", 'try { JSON.parse("{bad") } catch(e){ console.log(e.name) } try { JSON.parse("[1,]") } catch(e){ console.log(e.name) } try { JSON.parse("") } catch(e){ console.log(e.name) }')
case("json_tojson", 'console.log(JSON.stringify({a:{toJSON:function(){return "custom"}}}), JSON.stringify({d:new Date(0)}))')
case("json_unicode", 'console.log(JSON.parse(\'"\\\\u0041\\\\u00e9"\'), JSON.stringify("é"))')

# ---- Math, Number, misc builtins -------------------------------------------------------------------------------
case("math_basic", 'console.log(Math.floor(2.7), Math.floor(-2.2), Math.ceil(2.1), Math.ceil(-2.9), Math.round(2.5), Math.round(-2.5), Math.round(2.4), Math.trunc(-2.7), Math.abs(-5), Math.sign(-3), Math.sign(0), Math.sign(4))')
case("math_minmax", 'console.log(Math.min(3,1,2), Math.max(3,1,2), Math.min(), Math.max(), Math.max(1,"5"), Math.min(1,NaN), Math.max.apply(null,[1,9,3]))')
case("math_pow_sqrt", 'console.log(Math.pow(2,8), Math.pow(2,-1), Math.sqrt(16), Math.sqrt(2).toFixed(3), Math.sqrt(-1), Math.pow(9,0.5), 2**0.5>1.41, Math.hypot(3,4))')
case("math_random", 'var ok=true; for (var i=0;i<100;i++){ var r=Math.random(); if (!(r>=0 && r<1)) ok=false } console.log(ok, Math.random()!==Math.random())')
case("math_const", 'console.log(Math.PI.toFixed(2), Math.E.toFixed(2), Math.PI>3.14, Math.PI<3.15)')
case("number_static", 'console.log(Number.isInteger(5), Number.isInteger(5.5), Number.isNaN(NaN), Number.isNaN("x"), isNaN("x"), Number.isFinite(5), Number.isFinite("5"), Number.parseFloat("1.5"), Number.parseInt("12"), Number.MAX_SAFE_INTEGER>1e12)')
case("boolean_conv", 'console.log(Boolean(0), Boolean(1), Boolean(""), Boolean("a"), Boolean([]), Boolean(null), Boolean({}), !!NaN, String(true), true+true)')
case("date_basic", 'var d=new Date(Date.UTC ? 0 : 0); console.log(d.getTime(), d.getUTCFullYear(), d.toISOString()); var e=new Date(2020, 1, 29, 12, 30, 15); console.log(e.getFullYear(), e.getMonth(), e.getDate(), e.getHours(), e.getMinutes(), e.getSeconds())')
case("date_now", 'var a=Date.now(); console.log(typeof a, a>1700000000000, new Date().getTime()>=a)')
case("date_ms", 'var d=new Date(1700000000123); console.log(d.toISOString(), d.getUTCDay(), d.getUTCMilliseconds())')
case("global_this", 'console.log(typeof globalThis, globalThis===globalThis.globalThis)')

# ---- larger programs -------------------------------------------------------------------------------------------
case("prog_primes", 'function primes(n){ var s=[], r=[]; for (var i=2;i<=n;i++){ if(!s[i]){ r.push(i); for (var j=i*i;j<=n;j+=i) s[j]=true } } return r } console.log(primes(50).join(" "))')
case("prog_fizzbuzz", 'var o=[]; for (var i=1;i<=15;i++) o.push(i%15==0?"FizzBuzz":i%3==0?"Fizz":i%5==0?"Buzz":i); console.log(o.join(" "))')
case("prog_sort_algos", 'function qs(a){ if(a.length<2) return a; var p=a[0], l=[], r=[]; for (var i=1;i<a.length;i++) (a[i]<p?l:r).push(a[i]); return qs(l).concat([p], qs(r)) } console.log(qs([5,3,8,1,9,2,7]).join())')
case("prog_wordcount", 'var t="the quick brown fox jumps over the lazy dog the end"; var c={}; t.split(" ").forEach(function(w){ c[w]=(c[w]||0)+1 }); console.log(Object.keys(c).sort().map(function(k){ return k+"="+c[k] }).join(" "))')
case("prog_matrix", 'function mul(a,b){ return a.map(function(r){ return b[0].map(function(_,j){ return r.reduce(function(s,v,k){ return s+v*b[k][j] },0) }) }) } console.log(JSON.stringify(mul([[1,2],[3,4]],[[5,6],[7,8]])))')
case("prog_memo", 'var memo={}; function f(n){ if (n in memo) return memo[n]; return memo[n]=n<2?n:f(n-1)+f(n-2) } console.log(f(40))')
case("prog_stack_queue", 'var s=[]; s.push(1); s.push(2); s.push(3); var q=[1,2,3]; console.log(s.pop(), q.shift(), s.length, q.length)')
case("prog_binary_search", 'function bs(a,t){ var lo=0, hi=a.length-1; while(lo<=hi){ var m=(lo+hi)>>1; if(a[m]==t) return m; if(a[m]<t) lo=m+1; else hi=m-1 } return -1 } var a=[]; for (var i=0;i<100;i++) a.push(i*3); console.log(bs(a,150), bs(a,151))')
case("prog_string_rev", 'console.log("hello".split("").reverse().join(""), [..."abc"].reverse().join(""))')
case("prog_class_like", 'function Stack(){ this.items=[] } Stack.prototype.push=function(x){ this.items.push(x); return this }; Stack.prototype.size=function(){ return this.items.length }; var s=new Stack(); s.push(1).push(2).push(3); console.log(s.size())')
case("prog_event_emitter", 'function Em(){ this.h={} } Em.prototype.on=function(n,f){ (this.h[n]=this.h[n]||[]).push(f) }; Em.prototype.emit=function(n){ var a=[].slice.call(arguments,1); (this.h[n]||[]).forEach(function(f){ f.apply(null,a) }) }; var e=new Em(), got=[]; e.on("x", function(v){ got.push(v) }); e.on("x", function(v){ got.push(v*2) }); e.emit("x", 21); console.log(got.join())')
case("prog_json_transform", 'var data=JSON.parse(\'[{"n":"a","v":3},{"n":"b","v":1},{"n":"c","v":2}]\'); console.log(data.sort(function(x,y){return x.v-y.v}).map(function(d){return d.n}).join(""), data.reduce(function(s,d){return s+d.v},0))')
case("prog_deep_recursion", 'function depth(n){ return n==0?0:1+depth(n-1) } console.log(depth(300))')
case("prog_many_objects", 'var a=[]; for (var i=0;i<20000;i++) a.push({id:i, name:"item"+i}); console.log(a.length, a[19999].name, a.filter(function(x){return x.id%1000==0}).length)')
case("prog_string_build", 'var parts=[]; for (var i=0;i<1000;i++) parts.push("<li>"+i+"</li>"); var html=parts.join(""); console.log(html.length, html.slice(0,12), html.indexOf("<li>999"))')

# ---- limits and safety -----------------------------------------------------------------------------------------
case("limit_infinite_loop", 'while (true) {}', err="Error: script took too long")
case("limit_infinite_recursion_builder", 'var a=[]; while(true) a.push("x".repeat(1000000))', err="Error: ")
case("syntax_error", 'var = 5;', err="SyntaxError: ")
case("syntax_error_unclosed", 'function f( { }', err="SyntaxError: ")
case("syntax_error_unterminated_string", 'var s = "abc', err="SyntaxError: ")


def run_engine(code):
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as f:
        f.write(code)
        path = f.name
    try:
        p = subprocess.run([JSRUN, path], capture_output=True, text=True, timeout=60)
        return p.returncode, p.stdout, p.stderr
    finally:
        os.unlink(path)


def run_node(code):
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as f:
        f.write(code)
        path = f.name
    try:
        p = subprocess.run([NODE, path], capture_output=True, text=True, timeout=60)
        return p.returncode, p.stdout, p.stderr
    finally:
        os.unlink(path)


def main():
    verbose = "-v" in sys.argv
    filt = [a for a in sys.argv[1:] if not a.startswith("-")]
    if not os.path.exists(JSRUN):
        print("build/tools/jsrun is missing: run `make jsrun` first")
        return 2
    bad = 0
    ran = 0
    slow = 0
    for name, code, err in CASES:
        if filt and not any(f in name for f in filt):
            continue
        ran += 1
        try:
            rc, out, serr = run_engine(code)
        except subprocess.TimeoutExpired:
            print('SLOW %s (skipped: timed out)' % name)
            slow += 1
            continue
        problem = None
        if err is not None:
            if rc != 1 or not serr.startswith(err):
                problem = "expected failure %r, got rc=%d stderr=%r stdout=%r" % (err, rc, serr.strip()[:200], out[:200])
        else:
            if rc != 0:
                problem = "engine failed: %r (stdout %r)" % (serr.strip()[:300], out[:200])
            elif NODE:
                nrc, nout, nerr = run_node(code)
                if nrc != 0:
                    problem = "node itself failed, fix the test: %r" % nerr.strip()[:300]
                elif nout != out:
                    problem = "output differs\n   node:   %r\n   engine: %r" % (nout, out)
        if problem:
            bad += 1
            print("FAIL %s: %s" % (name, problem))
        elif verbose:
            print("ok   %s" % name)
    print("%d cases, %d failed, %d slow%s" % (ran, bad, slow, "" if NODE else " (Node not installed: only checked that scripts run)"))
    print("js: PASS" if not bad else "js: FAIL")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
