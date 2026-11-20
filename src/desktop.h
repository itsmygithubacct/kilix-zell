#ifndef ZELL_DESKTOP_H
#define ZELL_DESKTOP_H
#include "scene.h"
#define ZELL_VERSION "0.1.1"
#define ZELL_WORLD_W 1536
#define ZELL_WORLD_H 1024
/* Measured chrome. Vitals bars are y=39 h=38, so the HUD ends at row 77.
 * The status strip is the bottom 30 rows. The room fit reserves both. */
#define ZELL_ROOM_VITALS_Y 39
#define ZELL_ROOM_VITALS_H 38
#define ZELL_ROOM_HUD_BOTTOM (ZELL_ROOM_VITALS_Y + ZELL_ROOM_VITALS_H)
#define ZELL_ROOM_STATUS_STRIP 30
#define ZELL_STATIONS 57
/* The Command Center computer that zooms into a usage browser. */
#define ZELL_MANA_CONSOLE 48
#define ZELL_WORKSHOP_COMPUTER 49
#define ZELL_FLEET_BOARD 50
#define ZELL_FLOOR_PANEL 51
/* Cave hatches, in the two rows leading downstairs. */
#define ZELL_COMMAND_HATCH 13
#define ZELL_LAUNDRY_HATCH 14
#define ZELL_VAULT_HATCH 42
#define ZELL_WORKSHOP_HATCH 43
#define ZELL_GAME_HATCH 52
#define ZELL_THEATER_HATCH 53
#define ZELL_MEDITATION_HATCH 54
#define ZELL_LIBRARY_HATCH 55
#define ZELL_DOWNSTAIRS_ROOMS 8
#define ZELL_HATCH_FOOT_OFFSET 14
/* The Command Center's shell computer, in its bottom left corner. */
#define ZELL_COMMAND_COMPUTER 56
/* The Command Center's controller and four worker screens. */
#define ZELL_COMMAND_SCREEN_FIRST 15
#define ZELL_COMMAND_SCREEN_LAST 19
#define ZELL_ZOOM_TICKS 22
#define ZELL_CAVE_STATIONS 15
/* Where the Neuronte lever is drawn, relative to its floor interaction point. */
#define ZELL_SWITCH_WALL_DX 104
#define ZELL_SWITCH_WALL_DY 6
#define ZELL_RUN_CHARGE_TICKS 24
#define ZELL_WALK_SPEED 3.5f
#define ZELL_RUN_SPEED 9.0f
#define ZELL_MODE_DESK 6
#define ZELL_MODE_LAUNCHER 7
#define ZELL_MODE_SERVICE 8
typedef struct { const char *name, *role; float x,y; int art; } zell_station;
typedef struct { int art, offset_x, offset_y; } zell_actor_pose;
/* actor -1 is Kilix; 0..5 are cave staff, 6/7 Git Wizard/Pilot. */
zell_actor_pose zell_desktop_pose(const zell_scene *s, int actor);
extern const zell_station zell_stations[ZELL_STATIONS];
/* Indexed by destination room (1..8). The closed art is on the station. */
typedef struct { int station, open_art; const char *label; } zell_cave_hatch;
extern const zell_cave_hatch zell_cave_hatches[ZELL_DOWNSTAIRS_ROOMS+1];
int zell_hatch_room(int station);
void zell_desktop_camera(const zell_scene *s, int *x, int *y);
void zell_desktop_init(zell_scene *s);
void zell_desktop_enter_hall(zell_scene *s);
/* How Zell starts the desktop: zell_desktop_init, then the entrance. */
void zell_desktop_start(zell_scene *s);
int zell_desktop_tick(zell_scene *s,const zell_controls *c);
int zell_desktop_near(const zell_scene *s);
bool zell_desktop_station_visible(const zell_scene *s,int station);
/* Walk-on stairs back to the cave, as fractions of the world. */
#define ZELL_STAIRS_LEFT .44f
#define ZELL_STAIRS_RIGHT .56f
/* Where Kilix comes in when Zell starts: on the cave's steps, facing in. */
#define ZELL_HALL_ENTRY_Y .94f
#define ZELL_STAIRS_PAINTED_Y .935f
#define ZELL_STAIRS_CUTOUT_Y .87f
#define ZELL_ROOM_ENTRY_Y .78f
bool zell_desktop_on_stairs(const zell_scene *s);
/* World-unit footprints. Screens come from the command layout, not a second table. */
bool zell_desktop_blocked(const zell_scene *s,float x,float y);
/* North edge of the walkable floor. Rooms 1–4 keep the old corridor. */
float zell_desktop_north(int room);
int zell_desktop_block_count(const zell_scene *s);
void zell_desktop_block_rect(const zell_scene *s,int index,float *x0,float *y0,float *x1,float *y1);
int zell_desktop_block_table(int index,int *room,float *x0,float *y0,float *x1,float *y1);
bool zell_desktop_station_in_front(const zell_scene *s,int station);
float zell_desktop_speed_factor(const zell_scene *s);
int zell_desktop_launcher_count(const zell_scene *s);
int zell_desktop_launcher_station(const zell_scene *s,int index);
int zell_desktop_launcher_action(const zell_scene *s,int index);
void zell_desktop_launcher_label(const zell_scene *s,int index,char *text,size_t capacity);
int zell_desktop_actions(int station);
/* Floating terminal window. The same rectangles are drawn and hit-tested. */
typedef struct { int x, y, w, h; } zell_rect;
/* World-space monitor geometry shared by drawing, collision and picking. */
void zell_monitor_layout(int screen,zell_rect *frame,zell_rect *display);
void zell_mana_console_layout(zell_rect *frame,zell_rect *display);
void zell_command_computer_layout(zell_rect *frame,zell_rect *display);
enum { ZELL_COMMAND_TERMS=6, ZELL_COMMAND_SHELL=5, ZELL_FLOOR_PANELS=4 };
void zell_command_term_layout(int index,zell_rect *frame,zell_rect *body);
void zell_command_focus(zell_scene *s,int index);
/* The Key Master's two lines, shown while Kilix is beside him. He stands in
 * the bottom right corner, with the AGENTS panel above him and the wall to
 * his right, so his lines go in the strip under that panel, ending left of
 * where Kilix can stand beside him. World pixels: the right edge of both
 * boxes and the top of the first. */
#define ZELL_KEY_MASTER_LABEL_RIGHT 1256
#define ZELL_KEY_MASTER_LABEL_TOP 903
#define ZELL_KEY_MASTER_HINT "Enter: encrypted secret vault"
/* The Command Center's walls, world pixels. Its bottom edge is open and
 * unmarked. Beside the screens a strip runs up each side wall to an opening
 * in the top corner; the tunnel floor is OPENING_TOP..OPENING_BOTTOM, and
 * Kilix goes through when his feet are OPENING_DEPTH from the world edge.
 * Below STRIP_BOTTOM the walls stand where they always did. */
#define ZELL_COMMAND_STRIP_LEFT 48
#define ZELL_COMMAND_STRIP_RIGHT (ZELL_WORLD_W-ZELL_COMMAND_STRIP_LEFT)
#define ZELL_COMMAND_STRIP_BOTTOM 760
#define ZELL_COMMAND_OPENING_TOP 226
#define ZELL_COMMAND_OPENING_BOTTOM 330
#define ZELL_COMMAND_OPENING_DEPTH 40
/* Rooms through the Command Center's openings: a Kilix tab of two panes on
 * the left, of four on the right, side by side, across the whole Zell pane.
 * Along the top is a walkway for Kilix, with buttons over each pane that
 * start a coding agent in it; under it each pane has a title bar, then its
 * cells. Clicking a pane gives it the keys; clicking the walkway gives them
 * back to Kilix. The way back is a thin green line at the walkway's end on
 * the side he came in by. In canvas pixels: */
#define ZELL_ROOM_PANES_LEFT 9
#define ZELL_ROOM_PANES_RIGHT 10
#define ZELL_ROOM_IS_PANES(room) ((room)==ZELL_ROOM_PANES_LEFT||(room)==ZELL_ROOM_PANES_RIGHT)
#define ZELL_PANES_WALK_H 64
#define ZELL_PANES_CHROME_H 20
#define ZELL_PANES_CELLS_Y (ZELL_PANES_WALK_H+ZELL_PANES_CHROME_H)
#define ZELL_PANES_EXIT_H ZELL_PANES_WALK_H
#define ZELL_PANES_EXIT_W 3
#define ZELL_PANES_EXIT_RGB 0x3ee07cu
/* Kilix stands on the walkway, feet at FEET_Y, drawn KILIX_H high, his feet
 * at least SIDE from either end. */
#define ZELL_PANES_FEET_Y 58
#define ZELL_PANES_KILIX_H 52
#define ZELL_PANES_SIDE 24
/* The agent buttons on the walkway's back wall. */
#define ZELL_PANES_BUTTON_Y 6
#define ZELL_PANES_BUTTON_H 22
#define ZELL_PANES_AGENTS 6
/* Elsewhere Zell's picture is at most 1600x1000; a pane room is as wide and
 * high as its pane, up to this. */
#define ZELL_PANES_MAX_W 3840
#define ZELL_PANES_MAX_H 2160
/* A pane's scrollbar is the right BAR_W pixels of its cells, while it has
 * rows scrolled out of view; its thumb is at least BAR_MIN high. */
#define ZELL_PANES_BAR_W 10
#define ZELL_PANES_BAR_MIN 16
/* Title-bar buttons, right to left: Restart, Zoom, and End (back to the
 * live rows, only while scrolled back). */
enum { ZELL_PANES_ZOOM, ZELL_PANES_RESTART, ZELL_PANES_END };
void zell_panes_frame_limits(int room,int *max_w,int *max_h);
int zell_panes_count(int room);
/* Side the exit line is on: -1 left, 1 right, 0 not a pane room. */
int zell_panes_exit_side(int room);
void zell_panes_exit_rect(const zell_scene *s,zell_rect *r);
/* Walk in, walk back out. Both place Kilix just inside the room. */
void zell_panes_enter(zell_scene *s,int room);
void zell_panes_leave(zell_scene *s);
int zell_panes_tick(zell_scene *s,const zell_controls *c);
/* The helper process: started by the pump while a pane room is shown. */
void zell_panes_pump(zell_scene *s);
void zell_panes_shutdown(void);
/* Bytes typed in a pane room, for the focused pane. */
void zell_panes_key(const unsigned char *bytes,size_t count);
bool zell_panes_click(zell_scene *s);
/* A pointer report in a pane room: buttons as SGR sends them (low bits the
 * button, +4 Shift, +8 Alt, +16 Ctrl, +32 motion, +64 wheel), x and y in
 * Zell's pixels, release for the report ending in m. Presses, drags and the
 * wheel over a pane's cells go to its program or select text there; the
 * scrollbar moves the rows shown. False if the panes did not take it. */
bool zell_panes_pointer(zell_scene *s,unsigned buttons,int x,int y,bool release);
/* Text pasted into Zell, for the pane with the keys; false if no pane has
 * them. Sent to the helper over the next pumps. */
bool zell_panes_paste(const zell_scene *s,const char *bytes,size_t count);
#define ZELL_PANES_PASTE_MAX (4u<<20)
/* Shift with Page Up, Page Down, Home or End while a pane has the keys
 * scrolls it (or, on its alternate screen, goes to its program). */
bool zell_panes_scroll_key(const zell_scene *s,uint32_t key,uint32_t modifiers);
/* A pane's rows as the helper last said: kept, first shown, shown. False
 * until it has said. */
bool zell_panes_scrollback(int room,int index,uint64_t out[3]);
/* Rows scrolled out of view below the ones shown. */
uint64_t zell_panes_lines_back(int room,int index);
/* Its scrollbar's track and thumb, empty while there is nothing to show. */
void zell_panes_scrollbar_rect(const zell_scene *s,int index,zell_rect *track,zell_rect *thumb);
/* The pane whose scrollbar the pointer is on, or that is being dragged;
 * -1 for none. */
int zell_panes_bar_hover(void);
/* Text selected in a pane, as the escape sequence that puts it on the
 * outer terminal's clipboard (OSC 52), once the helper has sent all of it.
 * main writes it and says so. */
const char *zell_panes_copy_sequence(size_t *length);
void zell_panes_copy_written(void);
/* Who has the keys in a pane room: a pane (after a click on it) or Kilix
 * (on the way in, and after a click on the walkway). */
bool zell_panes_keys_to_pane(void);
/* What a key does there: the pane's, Kilix presses the button he stands
 * on (Enter or Space), or nothing. Kilix walks on the arrows only while
 * he has the keys. */
enum { ZELL_PANES_KEY_NONE, ZELL_PANES_KEY_PANE, ZELL_PANES_KEY_PRESS };
int zell_panes_route(bool press_key);
/* Kilix presses what he aims at (zell_panes_aim); false if nothing. */
bool zell_panes_press(zell_scene *s);
/* What Enter presses where Kilix stands: of an agent button (index,
 * agent), the plaque in a pane's middle (index) and a title-bar button
 * below him (index, button), the one whose middle is nearest him; else the
 * pane itself (index), or nothing. Nothing while a pane has the keys. */
enum { ZELL_PANES_AIM_NONE, ZELL_PANES_AIM_AGENT, ZELL_PANES_AIM_PAD, ZELL_PANES_AIM_CHROME, ZELL_PANES_AIM_PANE, ZELL_PANES_AIM_HOTKEYS };
int zell_panes_aim(const zell_scene *s,int *index,int *which);
/* The plaque: ZELL_PANES_PAD_W wide at the pane's middle, on panes at least
 * ZELL_PANES_PAD_MIN wide; w is 0 otherwise. */
#define ZELL_PANES_PAD_W 48
#define ZELL_PANES_PAD_MIN 160
void zell_panes_pad_rect(const zell_scene *s,int index,zell_rect *r);
/* The latest picture of the shown room, or NULL. */
const uint32_t *zell_panes_frame(int room,int *width,int *height);
/* Why there is no picture, or "". */
const char *zell_panes_error(void);
/* Terminal cells under the walkway and title bars: 8x16 pixels, at most
 * 480x135. */
void zell_panes_cells(int width,int height,int *cols,int *rows);
/* Pane index's cells, title bar and walkway part, in canvas pixels; w is 0
 * for a pane hidden by another one's zoom. */
void zell_panes_pane_rect(const zell_scene *s,int index,zell_rect *r);
void zell_panes_chrome_rect(const zell_scene *s,int index,zell_rect *r);
/* Its title bar's buttons: 0 zoom, 1 restart. */
void zell_panes_chrome_button(const zell_scene *s,int index,int button,zell_rect *r);
/* The pane a canvas x is over; a separator belongs to the pane on its left. */
int zell_panes_pane_at(const zell_scene *s,int x);
/* Alt and an arrow: from a pane, Left and Right give the keys to the pane
 * beside it and Up gives them to Kilix, over that pane; from the walkway,
 * Left and Right take Kilix over the pane beside the one he is over, and
 * Down gives the keys to the pane he is over. Not to panes a zoom hides.
 * False outside a pane room. */
bool zell_panes_move(zell_scene *s,int dx,int dy);
/* Ctrl+Tab (step 1) and Ctrl+Shift+Tab (-1); Ctrl+Alt+Z. */
bool zell_panes_cycle(zell_scene *s,int step);
bool zell_panes_zoom_key(zell_scene *s);
/* The Hotkeys button on the walkway (w 0 when the room is too narrow), and
 * the window it opens: what it says, a line each. */
#define ZELL_PANES_HOTKEYS_LABEL "Hotkeys"
void zell_panes_hotkeys_rect(const zell_scene *s,zell_rect *r);
bool zell_panes_hotkeys_open(void);
void zell_panes_hotkeys_show(bool open);
#define ZELL_PANES_HOTKEYS_LINE_H 18
#define ZELL_PANES_HOTKEYS_EDGES 56 /* its title above the lines, a margin below */
int zell_panes_hotkeys_fit(int height);
int zell_panes_hotkeys_top(int height);
void zell_panes_hotkeys_scroll(const zell_scene *s,int lines);
extern const char *const zell_panes_hotkeys_text[];
extern const int zell_panes_hotkeys_lines;
/* Agent button `agent` over pane index; w is 0 when it does not fit. */
void zell_panes_button_rect(const zell_scene *s,int index,int agent,zell_rect *r);
int zell_panes_agent_count(void);
const char *zell_panes_agent_label(int agent);
/* What the helper says a pane runs ("" until it says), a notice it gave
 * for one (NULL once it is 4 seconds old), and whether its restart button
 * waits for the second click (3 seconds). */
const char *zell_panes_title(int room,int index);
const char *zell_panes_notice(int room,int index);
bool zell_panes_confirming(int room,int index);
int zell_panes_focus(int room);
int zell_panes_zoom(int room);
/* Pane-protocol lines, for tests: what the pump would send next, and a
 * line as if the helper had written it. */
const char *zell_panes_pending(void);
int zell_panes_room_line(int room,int cols,int rows,char *out,size_t capacity);
void zell_panes_test_line(const char *line);
void zell_panes_test_room(zell_scene *s);
int zell_panes_test_grow_pipe(int fd);
void zell_panes_test_read(int fd);
/* Paste lines still to go to the helper; tests drain them as a pump would. */
size_t zell_panes_paste_left(void);
void zell_panes_test_drain(void);
void zell_panes_test_reset(void);
void zell_panes_test_copy(void *map);
void zell_panes_test_fail(const char *text);
size_t zell_panes_map_bytes(void);
void zell_command_switch_layout(int index,zell_rect *frame);
/* World frame and glass for floor panel 0 (YOLO), 1 (agents), or 2 (spare).
 * Panels 0 and 1 use the command-switch rectangles. glass may be NULL. */
void zell_floor_panel_layout(int index,zell_rect *frame,zell_rect *glass);
bool zell_desktop_click(zell_scene *s);
/* Left edge of the bottom row of minimized-window buttons. */
#define ZELL_WINDOW_ROW_X 12
/* The DJ task that opens a text browser in the floating window. */
#define ZELL_DJ_STATION 4
#define ZELL_DJ_BROWSER_ACTION 9
typedef struct { zell_rect frame, image, minimize, maximize, close, tab; } zell_terminal_layout;
void zell_terminal_layout_for(const zell_scene *s,int width,int height,zell_terminal_layout *out);
void zell_terminal_cells(const zell_scene *s,int *cols,int *rows);
bool zell_terminal_click(zell_scene *s);
void zell_terminal_restore(zell_scene *s);
const char *zell_desktop_action(int station,int index);
/* L3 replaces zell_room_view_identity with the fit in src/desktop_command.inc. */
void zell_room_view_identity(zell_scene *s);
void zell_world_to_screen(const zell_scene *s,float world_x,float world_y,float *screen_x,float *screen_y);
void zell_screen_to_world(const zell_scene *s,float screen_x,float screen_y,float *world_x,float *world_y);
bool zell_window_accepts_frame(const zell_scene *s,int slot,int generation);
/* Sixteen ticks from the floor in front of the stool to the seat. */
#define ZELL_SEAT_HOP_TICKS 16
#define ZELL_SEAT_LIFT_PEAK 22
#define ZELL_SEAT_SIT_DROP 10
int zell_seat_lift(int tick);
int zell_seat_draw_dy(const zell_scene *s);
void zell_workshop_floor_point(float *x,float *y);
void zell_workshop_seat_point(float *x,float *y);
bool zell_provider_apply(zell_scene *s,const char *text);
bool zell_provider_check_apply(zell_scene *s,const char *text);
bool zell_fleet_apply(zell_scene *s,const char *text);
/* Display-only board beside worker screen 4. Station 50 stays non-interactive. */
void zell_fleet_board_layout(zell_rect *board);
bool zell_fleet_board_line(const zell_scene *s,int index,char *text,size_t capacity);
bool zell_update_apply(zell_scene *s,const char *text);
/* Monotonic milliseconds. zell_notice_set_clock pins this for tests. */
int64_t zell_notice_clock_ms(void);
void zell_notice_set_clock(int64_t ms);
/* Empty text clears the deadline. The same text again does not move it. */
void zell_notice_apply(zell_scene *s,const char *text);
/* Theater helper, separate from the station service. Draw is screen-space. */
struct sr_canvas;
void zell_theater_draw(const zell_scene *s,struct sr_canvas *canvas);
int zell_theater_input(zell_scene *s,const zell_controls *c);
bool zell_theater_click(zell_scene *s);
bool zell_theater_typing(const zell_scene *s);
void zell_theater_shutdown(void);
bool zell_theater_install_json(char *dst,size_t cap);
bool zell_theater_install_action_ok(const char *browser,const char *action,const char *deb);
bool zell_theater_wants_keys(const zell_scene *s);
void zell_theater_fullscreen_layout(int width,int height,zell_rect *picture,zell_rect *caption,zell_rect *help,zell_rect *drop);
int zell_theater_listing_count(void);
const char *zell_theater_listing_path(int index);
const char *zell_theater_picture(void);
const char *zell_theater_queue_label(int index);
bool zell_theater_playing(void);
bool zell_theater_help_open(void);
bool zell_theater_queue_nav(int width,int height,int dir,zell_rect *out);
void zell_ui_fill(struct sr_canvas *canvas,int x,int y,int w,int h,uint32_t color,float alpha);
void zell_ui_stroke(struct sr_canvas *canvas,int x,int y,int w,int h,float thickness,uint32_t color,float alpha);
void zell_ui_text(struct sr_canvas *canvas,int x,int y,const char *text,uint32_t color,int width);
void zell_ui_blit(struct sr_canvas *canvas,int dx,int dy,int dw,int dh,const uint32_t *pixels,int src_w,int src_h);
int zell_theater_copy_frame(const void *map,size_t map_bytes,uint32_t want_generation,uint32_t *last_seq,uint32_t *dst,size_t dst_pixels,int *out_w,int *out_h);
void zell_theater_contain(int src_w,int src_h,int dest_w,int dest_h,int *x,int *y,int *w,int *h);
void zell_theater_video_rect(zell_rect *r);
void zell_theater_caption_rect(zell_rect *r);
/* Screen bar and the one or two glyph rows drawn inside it. An unused row has h 0. */
void zell_theater_caption_screen(const zell_scene *s,int width,int height,zell_rect *bar,zell_rect *line0,zell_rect *line1);
void zell_theater_remote_rect(zell_rect *r);
void zell_theater_queue_rect(int index,int width,int height,zell_rect *r);
int zell_theater_format_open_file(char *dst,size_t cap,const char *id,const char *path);
void zell_theater_test_reset(void);
int zell_theater_apply_line(zell_scene *s,const char *line);
bool zell_theater_submit(zell_scene *s,const char *json);
bool zell_theater_menu_open(void);
bool zell_theater_fullscreen(void);
int zell_theater_queue_count(void);
const char *zell_theater_error(void);
const char *zell_theater_caption(void);
const char *zell_theater_last_command(void);
void zell_theater_menu_layout(const zell_scene *s,zell_rect *panel,zell_rect *rows,int row_cap,int *row_count);
int zell_theater_present(const void *map,size_t map_bytes);
/* The outer terminal's input, split into keys, pointer reports and pastes
 * (see desktop_input_scan.inc). */
#define ZELL_INPUT_PASTE_MAX (4u<<20)
/* An escape held this long with nothing after it goes on as keys. */
#define ZELL_INPUT_ESCAPE_MS 30
/* A paste with no byte for this long has lost its end: it is dropped. */
#define ZELL_INPUT_PASTE_IDLE_MS 5000
/* The count given with no bytes for a paste whose end never came. */
#define ZELL_INPUT_PASTE_UNENDED ((size_t)-1)
/* Lines a paste would make: 0 with no line break in it (CR, LF or CR LF),
 * else the breaks, and one more for text after the last. */
size_t zell_paste_lines(const char *bytes,size_t count);
/* How long a paste of more than one line waits to be let through. */
#define ZELL_PASTE_ASK_MS 8000
/* A notice on the pane with the keys in the room shown, for ms. */
void zell_panes_tell(const zell_scene *s,const char *text,int64_t ms);
typedef struct {
    void (*keys)(void *user,const char *bytes,size_t count);
    void (*pointer)(void *user,unsigned buttons,int x,int y,bool release);
    void (*paste)(void *user,const char *bytes,size_t count);
    void *user;
} zell_input_sinks;
typedef struct {
    char pending[96];
    size_t used;
    int64_t changed;
    bool pasting,paste_over;
    char *paste;
    size_t paste_n,paste_cap,end_match;
} zell_input;
void zell_input_feed(zell_input *in,const zell_input_sinks *sinks,const char *bytes,size_t count,int64_t now);
void zell_input_idle(zell_input *in,const zell_input_sinks *sinks,int64_t now);
bool zell_input_pasting(const zell_input *in);
void zell_input_free(zell_input *in);

#endif
