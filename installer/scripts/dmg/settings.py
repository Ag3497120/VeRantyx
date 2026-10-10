# dmgbuild settings for the Glovebox disk image (scripts/release.sh). dmgbuild writes the window's layout itself,
# so building it needs no Finder scripting and no Automation prompt. Values come in with -D:
#   app   the signed Glovebox.app      art   this folder      icon   the volume icon (.icns), may be empty
import os.path

app = defines["app"]
art = defines["art"]

format = "UDZO"
filesystem = "HFS+"
files = [app]
symlinks = {"Applications": "/Applications"}
hide_extensions = ["Glovebox.app"]
if defines.get("icon"):
    icon = defines["icon"]

# background.png and background@2x.png (dmgbuild joins them into one Retina TIFF). Finder always draws file names
# black over a picture, so the art stays light. It is 440 tall for a window that shows about 400: the spare
# strip at the bottom is plain gradient, whichever way this macOS counts the title bar.
background = os.path.join(art, "background.png")
window_rect = ((200, 640), (660, 428))
default_view = "icon-view"
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False
show_icon_preview = False
show_item_info = False
label_pos = "bottom"
arrange_by = None
icon_size = 128
text_size = 13
# The centres the art is drawn around: the glow behind Glovebox, the arrow between the two.
icon_locations = {"Glovebox.app": (170, 182), "Applications": (490, 182)}
