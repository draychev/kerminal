# kerminal

A minimal tmux control-mode client that launches `tmux -CC new` and renders panes.

## Next steps
1. Add full ANSI/VT features (colors, attributes, scroll regions, alternate screen) for robust TUI compatibility.
2. Implement copy/paste and scrollback via tmux buffers and `%paste-buffer-*` notifications.
