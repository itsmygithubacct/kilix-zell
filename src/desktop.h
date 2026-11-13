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
#define ZELL_STATIONS 52
/* The Command Center computer that zooms into a usage browser. */
#define ZELL_MANA_CONSOLE 48
#define ZELL_WORKSHOP_COMPUTER 49
#define ZELL_FLEET_BOARD 50
#define ZELL_FLOOR_PANEL 51
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
void zell_desktop_camera(const zell_scene *s, int *x, int *y);
void zell_desktop_init(zell_scene *s);
int zell_desktop_tick(zell_scene *s,const zell_controls *c);
int zell_desktop_near(const zell_scene *s);
bool zell_desktop_station_visible(const zell_scene *s,int station);
/* Walk-on stairs back to the cave, as fractions of the world. */
#define ZELL_STAIRS_LEFT .44f
#define ZELL_STAIRS_RIGHT .56f
#define ZELL_STAIRS_PAINTED_Y .935f
#define ZELL_STAIRS_CUTOUT_Y .87f
#define ZELL_ROOM_ENTRY_Y .78f
bool zell_desktop_on_stairs(const zell_scene *s);
/* World-unit footprints. Screens come from the command layout, not a second table. */
bool zell_desktop_blocked(const zell_scene *s,float x,float y);
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
#endif
