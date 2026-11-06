"""App list drawn at the bottom of the e-paper screen."""

import lvgl

from lib.epd import display, display_width


_bar = None
_names = None
_known_apps: list[str] = []


def load_apps(apps):
    global _known_apps
    _known_apps = sorted([app.name for app in apps])


def draw_app_list():
    """Draw the splashscreen app list at the bottom, then flush the display."""
    global _bar, _names
    rows = []
    for start in range(0, len(_known_apps), 8):
        rows.append(
            " ".join(
                f"{idx}:{_known_apps[idx]}"
                for idx in range(start, min(start + 8, len(_known_apps)))
            )
        )
    loaded_apps = "\n".join(rows)

    if _bar is None:
        top = lvgl.layer_top()
        _bar = lvgl.obj(top)
        _bar.remove_style_all()
        _bar.set_width(display_width)
        _bar.set_height(lvgl.SIZE_CONTENT)
        _bar.align(lvgl.ALIGN.BOTTOM_MID, 0, 0)
        _bar.set_style_bg_color(lvgl.color_hex(0xFFFFFF), 0)
        _bar.set_style_bg_opa(lvgl.OPA.COVER, 0)
        _bar.set_flex_flow(lvgl.FLEX_FLOW.COLUMN)
        _bar.set_style_pad_all(2, 0)
        _bar.set_style_pad_row(0, 0)

        title = lvgl.label(_bar)
        title.set_width(display_width)
        title.set_text("Loaded Apps:")
        title.set_style_text_color(lvgl.color_hex(0x000000), 0)

        _names = lvgl.label(_bar)
        _names.set_width(lvgl.pct(100))
        # _names.set_long_mode(lvgl.LABEL_LONG_MODE.WRAP)
        _names.set_style_text_color(lvgl.color_hex(0xFF0000), 0)

    _names.set_text(loaded_apps)
    _bar.update_layout()
    # Drop any full-screen invalidation so this flush only covers the list.
    display.lv_display.inv_p = 0
    _bar.invalidate()
    lvgl.refr_now(display.lv_display)
