# Desktop and Built-in Applications

## Desktop stack

`gui/gfx.c` provides the framebuffer drawing primitives and text rendering.
The window manager in `gui/wm.c` owns windows, focus, z-order, resizing,
minimizing, maximizing, modal dialogs, timers, and damage-based composition.
`gui/desktop.c` manages icons, wallpaper, desktop files, and context menus;
`gui/taskbar.c` implements the Start menu, window buttons, clock, and memory
meter. Widgets, icons, menus, and dialogs are shared by built-in apps.

Built-in applications are compiled into the kernel image and registered in
`apps/apps.c`. They are not `.npkg` programs: they use internal kernel APIs
and do not run in user mode.

## Included applications

The desktop includes File Explorer, Notepad, Paint, Terminal, Task Manager,
System Properties, Settings, Nimbus Update, Web Browser, Media Player,
Calculator, Minesweeper, Snake, Solitaire, Breakout, and the NimbusOS disk
installer. App behavior and file associations are visible in the app modules
under `apps/`.

The File Explorer opens directories and uses file extensions for basic
associations. `.elf` files open as user programs; text, BMP, WAV, and HTML
files are handled by the corresponding built-in app paths.

## Terminal

The Terminal provides filesystem, process, system, networking, sound, and
FAT32 commands. It supports command history, output redirection, and launches
static ELF64 user programs from the current directory or `/Programs`.
`Ctrl+C` asks a running user program to stop. Use `help` inside NimbusOS for
the current command list.

For externally built apps and their installer, see
[user programs and `.npkg` packages](../development/user-programs.md).