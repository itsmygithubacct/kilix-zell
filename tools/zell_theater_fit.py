"""Aspect, cell pad, and pointer mapping for the theater picture."""

MAP_LIMIT = 2048
DEST_MAX = 16384


def contain(src_w, src_h, dest_w, dest_h):
    """Largest integer picture of the source aspect inside the destination."""
    if min(src_w, src_h, dest_w, dest_h) <= 0:
        raise ValueError('picture sizes must be positive')
    if src_w > DEST_MAX or src_h > DEST_MAX or dest_w > DEST_MAX or dest_h > DEST_MAX:
        raise ValueError('picture edge exceeds 16384')
    if src_w * dest_h >= dest_w * src_h:
        width = dest_w
        height = max(1, (dest_w * src_h + src_w // 2) // src_w)
    else:
        height = dest_h
        width = max(1, (dest_h * src_w + src_h // 2) // src_h)
    return min(width, dest_w), min(height, dest_h)


def bound_frame(width, height, limit=MAP_LIMIT):
    """Shrink a contained picture so both edges fit the memfd."""
    if width <= 0 or height <= 0:
        raise ValueError('frame edges must be positive')
    if width <= limit and height <= limit:
        return width, height
    if width >= height:
        return limit, max(1, (height * limit + width // 2) // width)
    return max(1, (width * limit + height // 2) // height), limit


def cell_pad(width, height):
    """Round up to a MultiInk 2x4 cell without scaling the picture away."""
    return width + (-width % 2), height + (-height % 4)


def map_pointer(x, y, dest_w, dest_h, content_w, content_h, display_w, display_h):
    """Map a destination-rectangle point into the private display.

    Points in the letterbox bars return None. The content is centered.
    """
    if min(dest_w, dest_h, content_w, content_h, display_w, display_h) <= 0:
        raise ValueError('pointer sizes must be positive')
    left = (dest_w - content_w) // 2
    top = (dest_h - content_h) // 2
    if not (left <= x < left + content_w and top <= y < top + content_h):
        return None
    px = (x - left) * display_w // content_w
    py = (y - top) * display_h // content_h
    return min(px, display_w - 1), min(py, display_h - 1)
