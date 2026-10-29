# NimbusOS JavaScript engine

A small, original JavaScript interpreter (tree-walking, mark-and-sweep GC) for the NimbusOS web browser.
No libc and **no floating point**: it builds the same on the host (tests) and in the kernel
(`-mno-sse -mno-80387`, only `kmalloc`/`krealloc`/`kfree` and `mem*`/`strlen` are used).

| File | What |
|---|---|
| `num.c` | fixed-point numbers: parse, format, `+ - * / %`, `pow`, `sqrt`, rounding |
| `obj.c` | strings, atoms, objects, arrays, string builder, garbage collector |
| `parse.c` | lexer and parser (source to AST) |
| `eval.c` | evaluator: scopes, closures, calls, exceptions, operators |
| `builtins.c` | `Object` `Function` `Array` `String` `Number` `Boolean` `Math` `JSON` `Error`s `Date` `console` + the public API |

## What works

ES5 plus: `let`/`const` (block scoped, fresh binding per loop iteration), arrow functions, template literals,
default parameters, spread in arrays and calls, `for...of`, `??`, `**`, shorthand and computed object keys, method
shorthand. `try/catch/finally`, `switch`, closures, prototypes and `new`, `call`/`apply`/`bind`, `JSON`.

## Known differences from real JavaScript (on purpose)

- **Numbers are fixed point**: four decimal places, magnitude up to 4e13, no exponent output
  (`0.1 + 0.2` is `0.3`, `1/3` is `0.3333`, `Math.sin` and friends do not exist yet). Larger values become `Infinity`.
- **Strings are UTF-8 bytes**: `"café".length` is 5, `toUpperCase` is ASCII only. Iteration with `for...of` and
  spread walks whole characters.
- Not yet: classes, destructuring, regular expressions, getters/setters, labels, optional chaining (`?.`),
  `Map`/`Set`/`Symbol`/`Promise`, generators, async.
- Safety limits (all configurable with `js_set_limits`): heap bytes, evaluation steps per script, call depth.
  A runaway script ends with an `Error: script took too long` instead of freezing the system.

## Testing

`make test-js` runs `tools/test_js.py`: every case runs in this engine and in Node and the output must match.
Also run clean under ASan/UBSan and about 5,500 fuzzed (randomly damaged) scripts without a crash.

## Stage 2: DOM and the Web Browser

`dom_tree.c` / `dom_js.c` give scripts a `document`, `window`, elements, events
(bubbling), `setTimeout`/`setInterval`, `location` and `console`. The browser
keeps the DOM as its own tree and serialises it back to HTML for the renderer;
clickable elements carry `nimbus-js:ID` links. Timers run from the browser's
50 ms timer. Ctrl+J turns scripts on or off. Only inline `<script>` blocks run;
`<script src>` is skipped with a log line. Limits: 16 MB heap, 8M steps per
script run, call depth 48. Tests: `make test-js` (host) and
`tools/test_js_browser.py` (QEMU).
