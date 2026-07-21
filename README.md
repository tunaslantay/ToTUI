# ToTUI - Terminal Kanban Todo

A lightweight, kanban-style TUI todo application written in C++17.
It uses native ANSI escape codes without external dependencies like ncurses.

## Features
- **Kanban Board Layout**: Multiple columns for different stages (e.g., Backlog, In Progress, Done).
- **Vim-like navigation**: Supports both Arrow keys and `h`, `j`, `k`, `l` for navigating between sections and tasks.
- **Edit & Rename**: Easily edit task text or rename your board sections.
- **Priorities**: High, Medium, and Low priorities with visual indicators.
- **Reordering**: Move tasks up and down within a section.
- **Persistence**: Automatically saves state to `todos.dat`.

## Build & Run
You can use the provided `Makefile`:

```bash
make
./todo
```

## Keybindings
| Key | Action |
| --- | --- |
| `↑` `↓` or `j` `k` | Navigate tasks vertically |
| `←` `→` or `h` `l` | Navigate sections horizontally |
| `J` `K` | Reorder task (move up/down) |
| `a` | Add new task to current section |
| `e` | Edit the selected task |
| `d` | Delete selected task |
| `Space` | Toggle task completion |
| `p` | Cycle priority (High → Medium → Low) |
| `m` | Move task to another section |
| `N` | Add new section |
| `R` | Rename current section |
| `X` | Remove current section (must be empty) |
| `[` `]` | Scroll section view left/right |
| `q` | Quit |
