#!/usr/bin/env python3
"""Native OS file drag source, restricted to disposable CI desktops."""
import json
import os
from pathlib import Path
import sys
import tkinter as tk
from tkinterdnd2 import COPY, DND_FILES, TkinterDnD

if os.environ.get("GITHUB_ACTIONS") != "true":
    raise SystemExit("Native drag automation requires a disposable CI desktop")
root = TkinterDnD.Tk()
root.title("CEF smoke file source")
root.geometry("170x80+0+40")
root.attributes("-topmost", True)
label = tk.Label(root, text="Drag test file", bg="orange")
label.pack(fill="both", expand=True)
label.drag_source_register(1, DND_FILES)
label.dnd_bind(
    "<<DragInitCmd>>", lambda e: (COPY, DND_FILES, (str(Path(sys.argv[1]).resolve()),))
)
root.update()
Path(sys.argv[2]).write_text(
    json.dumps([label.winfo_rootx() + 50, label.winfo_rooty() + 40])
)
root.mainloop()
