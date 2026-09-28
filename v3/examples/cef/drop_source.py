#!/usr/bin/env python3
"""Private-display GTK file drag source used by smoke.py."""
import sys
from pathlib import Path
import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, Gtk

window = Gtk.Window(title="CEF smoke file source")
window.set_default_size(160, 90)
label = Gtk.Label(label="Drag test file")
source = Gtk.EventBox()
source.add(label)
window.add(source)
source.drag_source_set(Gdk.ModifierType.BUTTON1_MASK,
                      [Gtk.TargetEntry.new("text/uri-list", 0, 0)], Gdk.DragAction.COPY)

def provide(widget, context, selection, info, timestamp):
    print("DRAG_DATA", file=sys.stderr, flush=True)
    selection.set_uris([Path(sys.argv[1]).resolve().as_uri()])

source.connect("drag-data-get", provide)
source.connect("drag-failed", lambda widget, context, result: print("DRAG_FAILED", result, file=sys.stderr, flush=True))
source.connect("drag-begin", lambda *args: print("DRAG_BEGIN", file=sys.stderr, flush=True))
window.connect("destroy", Gtk.main_quit)
window.show_all()
Gtk.main()
