from prompt_toolkit.filters import Condition
from prompt_toolkit.layout.containers import ConditionalContainer, Float, FloatContainer, HSplit, VSplit, Window
from prompt_toolkit.layout.controls import FormattedTextControl
from prompt_toolkit.layout.dimension import Dimension
from prompt_toolkit.layout.menus import CompletionsMenu

from .constants import (
    BOTTOM_AREA_HEIGHT,
    INPUT_BOX_HEIGHT,
    INPUT_CONTENT_HEIGHT,
    INPUT_EDITOR_HEIGHT,
    PINNED_STATUS_HEIGHT,
)
from .ui import FixedBottomUI


def make_layout_containers(ui: FixedBottomUI, editor):
    transcript_window = Window(
        content=FormattedTextControl(ui.display_fragments, focusable=False),
        height=Dimension(weight=1),
        ignore_content_height=True,
        wrap_lines=True,
        always_hide_cursor=True,
    )
    content_children = []
    spacer_height = INPUT_CONTENT_HEIGHT - INPUT_EDITOR_HEIGHT
    if spacer_height > 0:
        content_children.append(Window(height=spacer_height))
    content_children.append(VSplit([editor], height=INPUT_EDITOR_HEIGHT))
    input_box = HSplit([
        Window(height=1, char="─", style="class:border"),
        HSplit(content_children, height=INPUT_CONTENT_HEIGHT),
        Window(height=1, char="─", style="class:border"),
    ], height=Dimension.exact(INPUT_BOX_HEIGHT))
    pinned_status = Window(
        content=FormattedTextControl(ui.pinned_status_fragments),
        height=PINNED_STATUS_HEIGHT,
        style="class:banner-meta",
        always_hide_cursor=True,
    )
    approval_box = ConditionalContainer(
        content=Window(
            content=FormattedTextControl(ui.approval_fragments),
            height=lambda: Dimension.exact(ui.approval_height()),
            always_hide_cursor=True,
        ),
        filter=Condition(lambda: ui.awaiting_approval),
    )
    body = HSplit([transcript_window, approval_box, input_box, pinned_status])
    root = FloatContainer(
        content=body,
        floats=[
            Float(
                left=0,
                bottom=BOTTOM_AREA_HEIGHT,
                content=CompletionsMenu(max_height=6),
            )
        ],
    )
    return root, transcript_window, input_box
