"""Shared assertions for real GTK, Win32 and Cocoa file-picker interaction."""
import time
from pathlib import Path

CASES = ['open', 'open-attached', 'cancel-open', 'save', 'save-attached',
         'cancel-save', 'multiple', 'directory', 'filter', 'overwrite']


def prepare_files(root):
    # open/ and dirs/ are single-item views: their chooser opens with
    # exactly one selectable entry, so keyboard selection (End + Enter)
    # cannot miss regardless of the runner's sort order or resolution.
    root.mkdir(parents=True, exist_ok=True)
    (root / 'open').mkdir(exist_ok=True)
    (root / 'dirs').mkdir(exist_ok=True)
    (root / 'multiple').mkdir(exist_ok=True)
    (root / 'open/中文文件.txt').write_text('CEF file dialog fixture\n', encoding='utf-8')
    (root / 'dirs/目录').mkdir(exist_ok=True)
    (root / 'existing.txt').write_text('CEF file dialog fixture\n', encoding='utf-8')
    (root / 'multiple/first.txt').write_text('CEF file dialog fixture\n', encoding='utf-8')
    (root / 'multiple/第二个.txt').write_text('CEF file dialog fixture\n', encoding='utf-8')


def check_file_dialogs(root, platform, request, opened, key, paste, result, screenshot,
                       is_open=None):
    """Adapters provide physical input and native dialog geometry, never results."""
    root = root.resolve()
    for case in CASES:
        request(case)
        rect = opened(case)
        screenshot('file-' + case + '-open')
        if case.startswith('cancel'):
            key('esc')
            if is_open is not None:
                # Focus can sit on the parent window after programmatic
                # activation; Esc again while the chooser is still up.
                for _ in range(3):
                    time.sleep(1.2)
                    if not is_open():
                        break
                    key('esc')
            answer = result(case)
            assert answer['paths'] == [], answer
            error = answer['error'].lower()
            assert not error or 'cancel' in error or '800704c7' in error, answer
        else:
            expected = []
            if case == 'multiple':
                expected = [root / 'multiple/first.txt', root / 'multiple/第二个.txt']
                if platform == 'win32':
                    key('alt', 'n')
                    paste(' '.join('"' + str(p) + '"' for p in expected))
                elif platform == 'darwin':
                    # The runner pins the panel to the multiple/ folder,
                    # which the column browser shows selected: descend into
                    # it (the first row becomes the selection), then extend
                    # to the second row with a shift-click. Select-all is a
                    # synthetic chord and gets dropped by the runner.
                    key('right')
                    x0, y0, w0, h0 = rect
                    row_x = int(x0 + w0 * 0.72)
                    key('click', row_x, y0 + h0 * 0.205)
                    key('shift-click', row_x, y0 + h0 * 0.255)
                else:
                    # The chooser opens with the file list focused (the
                    # first row is highlighted on open), so select-all in
                    # the list needs no pointer interaction. The fixture
                    # directory holds exactly the two expected files.
                    key('ctrl', 'a')
            else:
                filename = {'save':'saved.txt', 'save-attached':'attached.txt',
                            'overwrite':'existing.txt', 'directory':'目录'}.get(case, '中文文件.txt')
                sub = {'open': 'open', 'open-attached': 'open', 'cancel-open': 'open',
                       'filter': 'open', 'directory': 'dirs', 'multiple': 'multiple'}.get(case, '')
                expected = [root / filename if not sub else root / sub / filename]
                if platform == 'darwin':
                    if case in ('save', 'save-attached', 'overwrite'):
                        # NSSavePanel: keyboard focus starts in the name
                        # field; select its prefilled text and confirm.
                        key('command', 'a')
                    else:
                        # The runner pins the open panel to the case's
                        # fixture folder, shown selected in the column
                        # browser: descend into it and its single entry
                        # becomes the selection.
                        key('right')
                elif platform == 'win32':
                    key('alt', 'n')
                    paste(str(expected[0]))
                    screenshot('file-' + case + '-pasted')
                else:
                    key('ctrl', 'l')
                    paste(str(expected[0]))
                    screenshot('file-' + case + '-pasted')
            key('enter')
            if is_open is not None:
                # One Enter can mean "navigate" instead of "accept"
                # (folder pickers, Go-to sheets). While the chooser is
                # still up, Enter again — bounded, so a case that can
                # never complete still fails with its own error.
                for _ in range(3):
                    time.sleep(1.2)
                    if not is_open():
                        break
                    key('enter')
            if case == 'overwrite':
                # NSSavePanel/IFileSaveDialog/GtkFileChooser ask before replacing
                # an existing file. The dialog itself never writes file contents.
                # The confirmation defaults to Replace/Yes on Cocoa and Win32
                # (Enter confirms); GTK focuses Cancel first, so move left.
                if platform == 'win32':
                    # The Save As box can swallow Enter while it parses the
                    # pasted path; Save's own accelerator (alt+S) works
                    # regardless of which control holds focus. Repeat it —
                    # on the confirmation alt+S is inert, and the Y
                    # accelerator below then confirms Replace.
                    for _ in range(3):
                        time.sleep(1.0)
                        key('alt', 's')
                time.sleep(.8)
                screenshot('file-overwrite-confirmation')
                if platform == 'win32':
                    # The Confirm Save As box supports Y/N accelerators;
                    # arrow-key + Enter proved unreliable on runners.
                    key('y')
                else:
                    key('enter')
            answer = result(case)
            assert not answer['error'], answer
            # GTK save dialogs append the active filter extension when the
            # typed name does not already end with it (saved.txt -> saved.txt.txt).
            def matches(actual, want):
                actual = Path(actual).resolve()
                if actual == want.resolve():
                    return True
                return (platform == 'linux' and want.suffix == '.txt'
                        and actual.name == want.name + '.txt'
                        and actual.parent == want.parent)
            got = sorted(Path(p).resolve() for p in answer['paths'])
            assert got == sorted(Path(w).resolve() for w in expected) or (
                len(got) == len(expected)
                and all(any(matches(a, w) for a in got) for w in expected)
                and all(any(matches(a, w) for w in expected) for a in got)
            ), answer
        screenshot('file-' + case + '-closed')
        print('PASS: native file dialog ' + case, flush=True)
    assert (root / 'existing.txt').read_text(encoding='utf-8') == 'CEF file dialog fixture\n'
