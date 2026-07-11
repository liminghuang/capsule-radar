Import("env")


def skip_unsupported_lvgl_asm(node):
    """LVGL ships ARM Helium assembly even when the C fallback is configured."""
    if node.get_abspath().endswith(("lv_blend_helium.S", "lv_blend_neon.S")):
        return None
    return node


env.AddBuildMiddleware(skip_unsupported_lvgl_asm)
