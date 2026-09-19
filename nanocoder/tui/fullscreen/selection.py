import base64


def copy_selection(text: str, app=None, clipboard_writer=None):
    if not text:
        return
    if clipboard_writer is not None:
        clipboard_writer(text)
        return
    if app is None:
        return
    try:
        encoded = base64.b64encode(text.encode("utf-8")).decode("ascii")
        app.output.write_raw(f"\x1b]52;c;{encoded}\x07")
        app.output.flush()
    except Exception:
        pass
