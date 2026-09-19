PROMPT = [("class:prompt", "› ")]
MAX_RENDER_LINES = 240
MIN_REFRESH_SECONDS = 1 / 30
MOUSE_SUPPORT = True
INPUT_CONTENT_HEIGHT = 1
INPUT_EDITOR_HEIGHT = 1
INPUT_BOX_HEIGHT = INPUT_CONTENT_HEIGHT + 2
PINNED_STATUS_HEIGHT = 1
BOTTOM_AREA_HEIGHT = INPUT_BOX_HEIGHT + PINNED_STATUS_HEIGHT
APPROVAL_BOX_HEIGHT = 6  # minimum reserved rows; the box grows to fit the tool card
THINKING_WORDS = ("Thinking", "Reading", "Planning", "Working", "Writing")
FULLSCREEN_STYLE = {
    "border": "ansibrightblack",
    "banner-border": "ansibrightcyan",
    "banner-meta": "ansiwhite",
    "banner-model": "ansibrightblue",
    "banner-repo": "ansiyellow",
    "banner-title": "ansiwhite bold",
    "input": "",
    "prompt": "ansicyan bold",
    "accent": "ansicyan bold",
    "approval-header": "ansibrightblue bold",
    "user-input-prompt": "bg:#333636 ansibrightblack bold",
    "user-input-row": "bg:#333636 ansiwhite",
    "assistant-marker": "ansiblue bold",
    "assistant-text": "ansiwhite",
    "markdown-heading": "ansiwhite bold",
    "markdown-list": "ansiwhite",
    "markdown-code": "ansibrightblack",
    "markdown-table": "ansiwhite",
    "markdown-table-header": "ansiwhite bold",
    "inline-code": "ansicyan",
    "transcript": "ansiwhite",
    "muted": "ansibrightblack",
    "warning": "ansired bold",
    "selection": "reverse",
}
