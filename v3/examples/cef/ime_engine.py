#!/usr/bin/env python3
"""Deterministic IBus preedit/commit engine for a private test session.

F6 starts Chinese preedit; Space commits it. This tests the real IBus protocol,
not the vocabulary or candidate UI of a production input method.
"""
import gi

gi.require_version("IBus", "1.0")
from gi.repository import IBus, GLib, GObject

IBus.init()

class SmokeEngine(IBus.Engine):
    __gtype_name__ = "CEFSmokeEngine"
    composing = False

    def do_process_key_event(self, keyval, keycode, state):
        if state & IBus.ModifierType.RELEASE_MASK:
            return False
        if keyval == IBus.KEY_F6:
            self.composing = True
            self.update_preedit_text(IBus.Text.new_from_string("中文输入"), 4, True)
            print("PREEDIT", flush=True)
            return True
        if self.composing and keyval == IBus.KEY_space:
            self.commit_text(IBus.Text.new_from_string("中文输入"))
            self.hide_preedit_text()
            self.composing = False
            print("COMMIT", flush=True)
            return True
        return False

GObject.type_register(SmokeEngine)
bus = IBus.Bus()
factory = IBus.Factory.new(bus.get_connection())
factory.add_engine("cef-smoke", SmokeEngine.__gtype__)
component = IBus.Component.new("org.freedesktop.IBus.CEFSmoke", "CEF smoke", "1.0",
                               "MIT", "Wails", "", "", "")
component.add_engine(IBus.EngineDesc.new("cef-smoke", "CEF smoke", "Chinese preedit test",
                                       "zh", "MIT", "Wails", "", "us"))
bus.register_component(component)
print("READY", flush=True)
GLib.MainLoop().run()
