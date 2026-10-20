# visual

Visual SSH terminal + remote desktop. Connects to a remote host over SSH and
presents a windowed desktop in an SDL2 window: a titled, draggable, resizable
terminal; SFTP-backed file browsers with create/rename/delete; a simple file
editor; a dock with launcher and clock; right-click context menus;
double-click-to-open desktop icons sourced from your remote home dir.

Terminal keystrokes go straight to the SSH channel; the server's echo is fed
through libvterm. File listings and writes run on worker threads so the UI
never freezes on network round-trips. Scrollback via mouse wheel.

## Build

Depends on `libssh2`, `libvterm`, `SDL2`, `SDL2_ttf`.

```
brew install libssh2 libvterm sdl2 sdl2_ttf
make
make install          # installs to $HOME/.local/bin
```

## Usage

```
visual user@host[:port]      # direct connect
visual                       # use default_host from ~/.config/visual/config
echo user@host | visual      # pipe target in
visual -r                    # read-only (file write ops hidden)
visual -s                    # stop a running instance
visual -h                    # help
ssh user@host | vis          # "vis" can also be used as a shortned form of the command, it means the same
```

See `config.example` for the full set of knobs.

## Layout

- `src/sdl.c` — rendering, event loop, glyph atlas, hover tracking
- `src/windows.c` — window list, Files/Editor/Terminal windows, launcher,
  context menus, async SFTP writes
- `src/ssh.c` — libssh2 session + reader/keepalive threads
- `src/sftp.c` — SFTP ops, lock-per-readdir
- `src/vterm.c` — libvterm glue, keystroke → channel, scrollback push
- `include/vis.h` — all types + prototypes
