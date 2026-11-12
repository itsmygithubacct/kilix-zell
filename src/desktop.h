#ifndef ZELL_DESKTOP_H
#define ZELL_DESKTOP_H
#include "scene.h"
#define ZELL_VERSION "0.1.1"
#define ZELL_WORLD_W 1536
#define ZELL_WORLD_H 1024
#define ZELL_STATIONS 46
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
bool zell_desktop_click(zell_scene *s);
typedef struct { zell_rect frame, image, minimize, maximize, tab; } zell_terminal_layout;
void zell_terminal_layout_for(const zell_scene *s,int width,int height,zell_terminal_layout *out);
void zell_terminal_cells(const zell_scene *s,int *cols,int *rows);
bool zell_terminal_click(zell_scene *s);
void zell_terminal_restore(zell_scene *s);
const char *zell_desktop_action(int station,int index);
#endif
