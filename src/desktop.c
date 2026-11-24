#include "desktop.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include "draw.h"
#include "command_term.h"
#include "runtime.h"
#include "desktop_assets.h"
/* The live tiles share their edges inside one full-width bronze frame. */
enum { WALL_SIDE_RIM=21, WALL_COL=(ZELL_WORLD_W-2*WALL_SIDE_RIM)/3,
       WALL_WEST_CENTER=WALL_SIDE_RIM+WALL_COL/2, WALL_EAST_CENTER=ZELL_WORLD_W-WALL_WEST_CENTER,
       WALL_TOP=ZELL_ROOM_HUD_BOTTOM+29, WALL_SEAM=386, MANA_BOTTOM=484, WALL_BOTTOM=648, WALL_FOOT_RIM=18 };
/* Command Center staff stand along the south wall. */
#define KM_Y .965f
const zell_station zell_stations[ZELL_STATIONS]={
 {"Wizard","Ask, think, discover",.23f,.35f,5},
 {"Archivist","Logs and sessions",.50f,.35f,6},
 {"Administrator","Settings and software",.75f,.35f,7},
 {"Housekeeper","Storage and system health",.14f,.80f,8},
 {"DJ","Music and playlists",.69f,.84f,9},
 {"Librarian","Books, OCR and documents",.83f,.80f,10},
 {"Kilix TUI","Your terminal control center",.10f,.53f,11},
 {"Kilix shell","A full interactive shell",.90f,.53f,12},
 {"Wall map","Applications and places",.10f,.32f,14},
 {"Noticeboard","Notes and tasks",.90f,.40f,15},
 {"Voice switch","Toggle the wizard's spoken answers",.064f,.34f,16},
 {"Wall clock","Time and system health",.88f,.32f,13},
 {"Neuronte shell switch","Neural terminal / behind the shell computer",.90f,.42f,ZELL_ART_SWITCH},
 {"Command center hatch","Five connected coding screens downstairs",.30f,.645f,ZELL_ART_COMMAND_HATCH},
 {"Laundry room hatch","Drive bins and storage explorer downstairs",.78f,.645f,ZELL_ART_LAUNDRY_HATCH},
 {"Command screen","Coordinate the four worker sessions",.50f,.37f,ZELL_ART_CONTROLLER},
 {"Worker screen 1","Coding session",(float)WALL_WEST_CENTER/ZELL_WORLD_W,.37f,ZELL_ART_WORKER},
 {"Worker screen 2","Coding session",(float)WALL_EAST_CENTER/ZELL_WORLD_W,.37f,ZELL_ART_WORKER},
 {"Worker screen 3","Coding session",(float)WALL_WEST_CENTER/ZELL_WORLD_W,(float)WALL_BOTTOM/ZELL_WORLD_H,ZELL_ART_WORKER},
 {"Worker screen 4","Coding session",(float)WALL_EAST_CENTER/ZELL_WORLD_W,(float)WALL_BOTTOM/ZELL_WORLD_H,ZELL_ART_WORKER},
 {"Drive bin 1","Capacity and files",.22f,.46f,-1},
 {"Drive bin 2","Capacity and files",.40f,.46f,-1},
 {"Drive bin 3","Capacity and files",.58f,.46f,-1},
 {"Drive bin 4","Capacity and files",.76f,.46f,-1},
 {"Drive bin 5","Capacity and files",.22f,.72f,-1},
 {"Drive bin 6","Capacity and files",.40f,.72f,-1},
 {"Drive bin 7","Capacity and files",.58f,.72f,-1},
 {"Drive bin 8","Capacity and files",.76f,.72f,-1},
 /* The way out of a downstairs room. The Command Center draws no stairs. */
 {"Way back to the cave","Return upstairs",.50f,.91f,ZELL_ART_COMMAND_STAIRS},
 {"Git Wizard","Projects, changes and health watches",.235f,KM_Y,ZELL_ART_GIT_IDLE},
 {"Pilot","Project fleets and session arrangements",.61f,KM_Y,ZELL_ART_PILOT_IDLE},
 /* The Key Master keeps the bottom right corner of the Command Center. */
 {"Key Master","API keys, logins and encrypted secrets",(float)ZELL_KEY_MASTER_X/ZELL_WORLD_W,KM_Y,ZELL_ART_KEY_MASTER_IDLE},
 {"Gatekeeper","Power, sessions and restart checks",.41f,.88f,ZELL_ART_CIVIC_IDLE},
 {"Signalman","Wi-Fi, connections and Bluetooth",.34f,.62f,ZELL_ART_CIVIC_IDLE+4},
 {"Porter","Files, trash and archives",.36f,.86f,ZELL_ART_CRAFT_IDLE},
 {"Laundress","Drives, removable media and health",.64f,.86f,ZELL_ART_CRAFT_IDLE+4},
 {"Merchant","Packages, Kilix apps and local models",.66f,.62f,ZELL_ART_CIVIC_IDLE+8},
 {"Herald","Notices, jobs and attention",.59f,.88f,ZELL_ART_CIVIC_IDLE+12},
 {"Timekeeper","Timers, reminders and scheduled work",.81f,.34f,ZELL_ART_CRAFT_IDLE+8},
 {"Sentinel","Accounts, updates and security",.66f,.62f,ZELL_ART_CIVIC_IDLE+16},
 {"Vault Keeper","Backups, verification and restore",.50f,.55f,ZELL_ART_CRAFT_IDLE+12},
 {"Tinkerer","Displays, sound, printers and devices",.50f,.55f,ZELL_ART_CRAFT_IDLE+16},
 {"Vault hatch","Backups and restore downstairs",.46f,.645f,ZELL_ART_VAULT_HATCH},
 {"Workshop hatch","Device workshop downstairs",.62f,.645f,ZELL_ART_WORKSHOP_HATCH},
 {"Dictation switch","Listen once; review words before sending",.078f,.34f,16},
 {"Music mute switch","Pause or resume the DJ and room music",.092f,.34f,16},
 {"YOLO switch","Enable or disable shared coding-agent YOLO mode",393.f/1536.f,820.f/1024.f,-1},
 {"Agent updates switch","Choose and update installed coding agents",893.f/1536.f,820.f/1024.f,-1},
 {"Mana console","Coding-token balances and the providers' usage pages",.50f,484.f/1024.f,ZELL_ART_WORKER},
 {"Workshop computer","Sit at the stool and use this computer.",797.f/1536.f,408.f/1024.f,-1},
 {"Fleet board","Saved fleets and their live workers",1143.f/1536.f,820.f/1024.f,-1},
 {"Floor panel","Reserved, not an available action.",643.f/1536.f,820.f/1024.f,-1},
 /* Between the staff above and the older hatches below. The open lids
  * rise into the gap while the stone rims stay on the cave floor. */
 {"Game room hatch","Games downstairs",.30f,.52f,ZELL_ART_GAME_HATCH},
 {"Movie theater hatch","Screen downstairs",.46f,.52f,ZELL_ART_THEATER_HATCH},
 {"Meditation room hatch","Quiet room downstairs",.62f,.52f,ZELL_ART_MEDITATION_HATCH},
 {"Library hatch","Reading room downstairs",.78f,.52f,ZELL_ART_LIBRARY_HATCH},
 {"Command computer","Shell and mana scripts",.50f,(float)WALL_BOTTOM/ZELL_WORLD_H,ZELL_ART_WORKER},
 {"PuzzLLM hatch","Play the coding puzzle rooms",960.f/ZELL_WORLD_W,780.f/ZELL_WORLD_H,ZELL_ART_GAME_HATCH},
 /* Drawn in the room's own art. The stool's point is the floor east of it,
  * outside the table's block; the cabinet's is the floor in front of it. */
 {"Chess stool","Hop up and play Chess Bash",452.f/ZELL_WORLD_W,590.f/ZELL_WORLD_H,-1},
 {"Arcade cabinet","Play the coding puzzle rooms",768.f/ZELL_WORLD_W,392.f/ZELL_WORLD_H,-1},
 {"Lander cabinet","Play Terminal Lander",547.f/ZELL_WORLD_W,392.f/ZELL_WORLD_H,-1},
 {"Tanks cabinet","Play Bashed Earth",993.f/ZELL_WORLD_W,392.f/ZELL_WORLD_H,-1}
};
static const char *const actions[ZELL_STATIONS][17]={
 {"Ask the wizard","First-run tour of Zell",NULL},
 {"System journal","User service journal","Boot warnings","Browse agent rollouts","Browse Kilix transcripts","Search a log","Open another log",NULL},
 {"Kilix settings","Software and models (Merchant)","Language, region and font size","System and user services","Connections (Signalman)","GPU and model settings","Update OS in floating terminal","Health, stamina and mana",NULL},
 {"System health report","Disk usage","Largest home folders","Preview and clean old thumbnails","Process monitor","Clean downloaded package cache","Stop one of my processes","Startup applications",NULL},
 {"Open Kilix Amp","Play a file or URL","Playlists and creators","Follow a creator or playlist","Volume controls","Transcribe media","Transcripts and jobs","Music library on disk","Zell background music","Find a playlist or creator in a browser window",NULL},
 {"Books and documents library","Find a document","OCR an image","OCR a PDF","Read text aloud","Document converter","Export a text audiobook","How to find and add files",NULL},
 {"Open Kilix TUI",NULL}, {"Open Kilix shell",NULL},
 {"Application launcher","Home folder","Downloads","Web browser","Weather on the map","Screenshot or screen region capture",NULL},
 {"Read notebook","Add a note","Edit notebook","Task board","Station conversations","Encrypted clipboard history",NULL},
 {"Toggle voice",NULL}, {"Time and calendar","System health report","Check coding-token mana",NULL},
 {"Switch to Neuronte shell",NULL},
 {"Go down to command center",NULL}, {"Go down to laundry room",NULL},
 {"Open controller screen",NULL}, {"Open worker screen",NULL}, {"Open worker screen",NULL}, {"Open worker screen",NULL}, {"Open worker screen",NULL},
 {"Dig through drive",NULL}, {"Dig through drive",NULL}, {"Dig through drive",NULL}, {"Dig through drive",NULL},
 {"Dig through drive",NULL}, {"Dig through drive",NULL}, {"Dig through drive",NULL}, {"Dig through drive",NULL},
 {"Return to the cave",NULL},
 {"Projects and Git health",NULL}, {"Launch a project fleet",NULL},
 {"Open the secret vault",NULL},
#include "os_tasks.inc"
 [42]={"Go down to the Vault",NULL}, [43]={"Go down to the Workshop",NULL},
 [44]={"Start / finish dictation",NULL}, [45]={"Pause / resume music",NULL},
 [46]={"Toggle shared YOLO mode",NULL}, [47]={"Update selected coding agents",NULL},
 [48]={"Zoom into the usage dashboard","Zoom into the Codex usage page","Zoom into the Claude usage page","Zoom into the Grok usage page","Read mana now","What needs me","Automatic readings on / off",NULL},
 [49]={NULL}, [50]={NULL}, [51]={NULL},
 [52]={"Go down to the game room",NULL},
 [53]={"Go down to the movie theater",NULL},
 [54]={"Go down to the meditation room",NULL},
 [55]={"Go down to the library",NULL},
 [56]={"Open a shell beside the mana scripts",NULL},
 [ZELL_PUZZLLM_HATCH]={"Play PuzzLLM",NULL},
 [ZELL_CHESS_STOOL]={"Play Chess Bash",NULL},
 [ZELL_PUZZLLM_ARCADE]={"Play PuzzLLM",NULL},
 [ZELL_LANDER_ARCADE]={"Play Terminal Lander",NULL},
 [ZELL_TANKS_ARCADE]={"Play Bashed Earth",NULL}
};
int zell_desktop_actions(int station){int n=0;
 if(station<0||station>=ZELL_STATIONS)return 0;
 while(n<16&&actions[station][n])n++;
 return n;}
const char *zell_desktop_action(int station,int index){return index>=0&&index<zell_desktop_actions(station)?actions[station][index]:"";}
void zell_desktop_start(zell_scene *s){zell_desktop_init(s);zell_desktop_enter_hall(s);}
void zell_desktop_enter_hall(zell_scene *s){s->room=0;s->x=(float)ZELL_WORLD_W*.50f;s->y=(float)ZELL_WORLD_H*ZELL_HALL_ENTRY_Y;s->direction=ZELL_DIR_UP;s->step=0;s->step_ticks=0;}
void zell_desktop_init(zell_scene *s){s->desktop=true;s->x=(float)ZELL_WORLD_W*.51f;s->y=(float)ZELL_WORLD_H*.48f;s->station=0;s->desktop_launch=false;s->hp=100;s->max_hp=100;s->stamina=100;s->mana=-1;s->update_age_days=-1;s->mana_age=-1;s->os_health=-1;s->dirty_files=0;}
bool zell_desktop_station_visible(const zell_scene *s,int station){
 if(station<0||station>=ZELL_STATIONS)return false;
 if(s->room==0)return station<ZELL_CAVE_STATIONS||station==32||station==37||station==38||(station>=42&&station<=45)||(station>=ZELL_GAME_HATCH&&station<=ZELL_LIBRARY_HATCH);
 /* A pane room is all terminal: nothing in it is a station. */
 if(ZELL_ROOM_IS_PANES(s->room))return false;
 if(station==28)return s->room!=2;
 if(s->room==1)return (station>=15&&station<=19)||(station>=29&&station<=31)||(station>=46&&station<=48)||station==ZELL_FLOOR_PANEL||station==ZELL_FLEET_BOARD||station==ZELL_COMMAND_COMPUTER;
 if(s->room==2)return (station>=20&&station<20+s->disk_count)||station==34||station==35;
 /* The Sentinel keeps the Vault with its keeper; the Signalman and the
  * Merchant work in the Workshop. */
 return (s->room==3&&(station==40||station==39))||(s->room==4&&(station==41||station==33||station==36||station==ZELL_WORKSHOP_COMPUTER||station==ZELL_PUZZLLM_HATCH))||
  (s->room==5&&(station==ZELL_CHESS_STOOL||zell_arcade_game(station)));
}
/* The cabinets' painted bodies, measured from the room art. */
static const struct { int station; zell_rect body; const char *game; } arcades[]={
 {ZELL_LANDER_ARCADE, {467,110,160,264},"Terminal Lander"},
 {ZELL_PUZZLLM_ARCADE,{688,84,164,292}, "PuzzLLM"},
 {ZELL_TANKS_ARCADE,  {913,110,160,264},"Bashed Earth"}
};
const char *zell_arcade_game(int station){
 for(size_t i=0;i<sizeof arcades/sizeof arcades[0];i++)if(arcades[i].station==station)return arcades[i].game;
 return NULL;
}
static bool contains_folded(const char *haystack,const char *needle){
 for(;*haystack;haystack++){size_t i=0;while(needle[i]&&haystack[i]&&tolower((unsigned char)haystack[i])==tolower((unsigned char)needle[i]))i++;if(!needle[i])return true;}
 return !*needle;
}
static int launcher_entry(const zell_scene *s,int index,int *action){
 bool search=s->launcher_query[0]!=0;int count=0;
 for(int station=0;station<ZELL_STATIONS;station++){
  if(search&&s->room==2&&station==28)continue;
  if(!search){if(!zell_desktop_station_visible(s,station)||zell_desktop_actions(station)==0)continue;if(count++==index){if(action)*action=-1;return station;}continue;}
  for(int a=0;a<zell_desktop_actions(station);a++){
   if(!contains_folded(zell_stations[station].name,s->launcher_query)&&!contains_folded(actions[station][a],s->launcher_query))continue;
   if(count++==index){if(action)*action=a;return station;}
  }
 }
 return index<0?count:-1;
}
int zell_desktop_launcher_count(const zell_scene *s){
 return launcher_entry(s,-1,NULL);
}
int zell_desktop_launcher_station(const zell_scene *s,int index){
 return launcher_entry(s,index,NULL);
}
int zell_desktop_launcher_action(const zell_scene *s,int index){int action=-1;launcher_entry(s,index,&action);return action;}
void zell_desktop_launcher_label(const zell_scene *s,int index,char *text,size_t capacity){
 int action=-1,station=launcher_entry(s,index,&action);
 if(station<0){snprintf(text,capacity,"No matching actions");return;}
 if(action<0)snprintf(text,capacity,"%s",zell_stations[station].name);
 else snprintf(text,capacity,"%s / %s",zell_stations[station].name,actions[station][action]);
}
float zell_desktop_speed_factor(const zell_scene *s){
 int reserve=s->hp<0?100:s->hp;
 if(s->stamina>=0&&s->stamina<reserve)reserve=s->stamina;
 if(reserve<0)reserve=0;
 if(reserve>100)reserve=100;
 return .35f+.65f*(float)reserve/100.0f;
}
int zell_desktop_near(const zell_scene *s) {
 int best=-1;float dist=96.0f;
 for(int i=0;i<ZELL_STATIONS;i++){
  if(!zell_desktop_station_visible(s,i))continue;
  /* The Command Center's way out is its whole bottom edge and has nothing
   * drawn at one point, so it is walked through, never selected. */
  if(s->room==1&&(i==28||i==ZELL_FLEET_BOARD))continue;
  float dx=s->x-zell_stations[i].x*ZELL_WORLD_W;
  float dy=s->y-zell_stations[i].y*ZELL_WORLD_H;
  float d=sqrtf(dx*dx+dy*dy);
  if(d<dist){best=i;dist=d;}
 }
 return best;
}
/* The Command Center and Laundry open along their south edges. */
static bool open_south_edge(int room){return room==1||room==2;}
bool zell_desktop_on_stairs(const zell_scene *s){
 if(s->room<1||s->room>ZELL_DOWNSTAIRS_ROOMS)return false;
 float nx=s->x/(float)ZELL_WORLD_W,ny=s->y/(float)ZELL_WORLD_H;
 if(open_south_edge(s->room))return ny>.975f;
 return nx>=ZELL_STAIRS_LEFT&&nx<=ZELL_STAIRS_RIGHT&&ny>ZELL_STAIRS_PAINTED_Y;
}
/* Objects standing on the floor hide Kilix when he is further from the viewer
 * than their feet. Floor and wall fittings never do. */
bool zell_desktop_station_in_front(const zell_scene *s,int i){
 if(i<0||i>=ZELL_STATIONS||!zell_desktop_station_visible(s,i))return false;
 bool stands=s->room==0?(i<6||(i>=32&&i<=41)):(i!=28&&i>=15&&i<46);
 return !(s->room==1&&i>=15&&i<=19)&&stands&&zell_stations[i].y*(float)ZELL_WORLD_H>s->y;
}
/* The Game Room's furniture is one full-room foreground. Each solid pixel
 * stands as deep as the bottom of its run down the column: the solid pixels
 * below it, across gaps of up to ZELL_FRONT_RUN_GAP rows, so a stool's
 * cushion, stretchers and legs are one piece standing on the floor. Kilix is
 * drawn over the layer, then the pixels deeper than his feet go back over him. */
static const short *foreground_depth(int room){
 static short depth[ZELL_WORLD_W*ZELL_WORLD_H];
 static bool ready;
 if(room!=5)return NULL;
 if(!ready){
  const zell_art *a=zell_art_get(ZELL_ART_GAME_FOREGROUND);
  if(!a||a->width!=ZELL_WORLD_W||a->height!=ZELL_WORLD_H)return NULL;
  for(int x=0;x<ZELL_WORLD_W;x++){
   int bottom=-1,gap=0;
   for(int y=ZELL_WORLD_H-1;y>=0;y--){
    size_t at=(size_t)y*ZELL_WORLD_W+(size_t)x;
    unsigned alpha=a->pixels[at]>>24;
    if(alpha>=ZELL_FRONT_SOLID){if(bottom<0||gap>ZELL_FRONT_RUN_GAP)bottom=y;gap=0;}
    else gap++;
    /* Soft edges take the depth of the run they fringe. */
    depth[at]=(short)(alpha&&bottom>=0&&gap<=ZELL_FRONT_RUN_GAP?bottom:-1);
   }
  }
  /* Parts that hang in the air end their runs above the floor they stand
   * on. Measured from the art: each stands at least as deep as its leg. */
  static const struct { short x0,y0,x1,y1,depth; } hanging[]={
   {1056,330,1076,410,468}, /* pool table's left corner pocket, over its back leg */
  };
  for(size_t i=0;i<sizeof hanging/sizeof hanging[0];i++)
   for(int y=hanging[i].y0;y<hanging[i].y1;y++)for(int x=hanging[i].x0;x<hanging[i].x1;x++){
    short *d=&depth[(size_t)y*ZELL_WORLD_W+(size_t)x];
    if(*d>=0&&*d<hanging[i].depth)*d=hanging[i].depth;
   }
  ready=true;
 }
 return depth;
}
bool zell_desktop_foreground_by_depth(int room){return foreground_depth(room)!=NULL;}
int zell_desktop_front_depth(int room,int x,int y){
 const short *depth=foreground_depth(room);
 if(!depth||x<0||y<0||x>=ZELL_WORLD_W||y>=ZELL_WORLD_H)return -1;
 return depth[(size_t)y*ZELL_WORLD_W+(size_t)x];
}
/* The world box his sprite and shadow cover, clamped to the world. */
bool zell_desktop_kilix_box(const zell_scene *s,int *x0,int *y0,int *x1,int *y1){
 zell_actor_pose pose=zell_desktop_pose(s,-1);
 const zell_art *a=zell_art_get(pose.art);
 if(!a||a->height<1)return false;
 int width=ZELL_KILIX_DRAW_HEIGHT*a->width/a->height;
 int foot=(int)s->y+pose.offset_y+zell_seat_draw_dy(s);
 *x0=(int)s->x+pose.offset_x-width/2-1;*x1=*x0+width+2;
 *y0=foot-ZELL_KILIX_DRAW_HEIGHT-1;
 *y1=(int)s->y+ZELL_FOOT_RADIUS_Y+1;
 if(*y1<foot+1)*y1=foot+1;
 if(*x0<0)*x0=0;
 if(*y0<0)*y0=0;
 if(*x1>ZELL_WORLD_W)*x1=ZELL_WORLD_W;
 if(*y1>ZELL_WORLD_H)*y1=ZELL_WORLD_H;
 return *x0<*x1&&*y0<*y1;
}
/* The theater, meditation room and library foregrounds still always cover him. */
bool zell_desktop_foreground_in_front(const zell_scene *s){
 if(!s||s->room<5||s->room>8)return false;
 /* On the chess stool, or hopping onto it, he is above the furniture. */
 if(s->seat_ticks>0)return false;
 const short *depth=foreground_depth(s->room);
 if(!depth)return true;
 int x0,y0,x1,y1;
 if(!zell_desktop_kilix_box(s,&x0,&y0,&x1,&y1))return false;
 for(int y=y0;y<y1;y++)for(int x=x0;x<x1;x++)
  if((float)depth[(size_t)y*ZELL_WORLD_W+(size_t)x]>s->y)return true;
 return false;
}

/* Stool cushion and the floor just south of its legs, in world pixels.
 * Measured from the compiled stool sheet: opaque bounds x 759..835, y 291..383. */
enum { ZELL_STOOL_X=797, ZELL_STOOL_SEAT_Y=312, ZELL_STOOL_FLOOR_Y=408 };
static const char workshop_full_notice[]="No window slot is free.";

/* The Game Room's right stool. Its cushion is x 317..424 with its top near
 * y 500 and its legs reach y 627, inside the table's block, so he hops up
 * from the open floor east of it and sits facing the board. */
enum { ZELL_CHESS_SEAT_X=370, ZELL_CHESS_SEAT_Y=531, ZELL_CHESS_FLOOR_X=452, ZELL_CHESS_FLOOR_Y=590 };
typedef struct { int room,station; float floor_x,floor_y,seat_x,seat_y; int facing; } zell_seat;
static const zell_seat seats[]={
 {4,ZELL_WORKSHOP_COMPUTER,ZELL_STOOL_X,ZELL_STOOL_FLOOR_Y,ZELL_STOOL_X,ZELL_STOOL_SEAT_Y,ZELL_DIR_UP},
 {5,ZELL_CHESS_STOOL,ZELL_CHESS_FLOOR_X,ZELL_CHESS_FLOOR_Y,ZELL_CHESS_SEAT_X,ZELL_CHESS_SEAT_Y,ZELL_DIR_LEFT}
};
static const zell_seat *room_seat(int room){
 for(size_t i=0;i<sizeof seats/sizeof seats[0];i++)if(seats[i].room==room)return &seats[i];
 return NULL;
}
void zell_workshop_floor_point(float *x,float *y){*x=(float)ZELL_STOOL_X;*y=(float)ZELL_STOOL_FLOOR_Y;}
void zell_workshop_seat_point(float *x,float *y){*x=(float)ZELL_STOOL_X;*y=(float)ZELL_STOOL_SEAT_Y;}
void zell_chess_floor_point(float *x,float *y){*x=(float)ZELL_CHESS_FLOOR_X;*y=(float)ZELL_CHESS_FLOOR_Y;}
void zell_chess_seat_point(float *x,float *y){*x=(float)ZELL_CHESS_SEAT_X;*y=(float)ZELL_CHESS_SEAT_Y;}
/* lift(t) = peak * t * (T-t) / (T*T/4). Zero at both ends, 22 at tick 8. */
int zell_seat_lift(int tick){
 int span=ZELL_SEAT_HOP_TICKS;
 if(tick<=0||tick>=span)return 0;
 return (ZELL_SEAT_LIFT_PEAK*tick*(span-tick))/((span*span)/4);
}
int zell_seat_draw_dy(const zell_scene *s){
 if(!s)return 0;
 if(s->seat_ticks>0&&s->seat_ticks<ZELL_SEAT_HOP_TICKS)return -zell_seat_lift(s->seat_ticks);
 if(s->seated&&s->seat_ticks==ZELL_SEAT_HOP_TICKS)return ZELL_SEAT_SIT_DROP;
 return 0;
}
static void workshop_place(zell_scene *s,int tick){
 const zell_seat *seat=room_seat(s->room);
 if(!seat)return;
 float x0=seat->floor_x,y0=seat->floor_y,x1=seat->seat_x,y1=seat->seat_y,t;
 if(tick<0)tick=0;
 if(tick>ZELL_SEAT_HOP_TICKS)tick=ZELL_SEAT_HOP_TICKS;
 t=(float)tick/(float)ZELL_SEAT_HOP_TICKS;
 s->x=x0+(x1-x0)*t;s->y=y0+(y1-y0)*t;
}
static int workshop_slot(const zell_scene *s){
 for(int i=0;i<ZELL_WINDOWS;i++){
  if(!s->windows[i].used||s->windows[i].close_requested)continue;
  if(!strcmp(s->windows[i].title,"Workshop computer"))return i;
 }
 return -1;
}
static int workshop_next_generation(const zell_scene *s){
 int generation=1;
 for(int i=0;i<ZELL_WINDOWS;i++)if(s->windows[i].generation>=generation)generation=s->windows[i].generation+1;
 if(generation<=0||generation>1000000000)generation=1;
 return generation;
}
static void workshop_show_full(zell_scene *s){
 s->notice_count=1;
 zell_notice_apply(s,workshop_full_notice);
}
static bool workshop_reserve(zell_scene *s){
 if(workshop_slot(s)>=0)return true;
 int used=0,free_slot=-1,order=1;
 for(int i=0;i<ZELL_WINDOWS;i++){
  if(s->windows[i].used)used++;
  else if(free_slot<0)free_slot=i;
  if(s->windows[i].open_order>=order)order=s->windows[i].open_order+1;
 }
 if(used>=ZELL_WINDOWS||free_slot<0){workshop_show_full(s);return false;}
 zell_window *w=&s->windows[free_slot];
 *w=(zell_window){0};
 w->generation=workshop_next_generation(s);
 w->open_order=order;
 w->z_order=order;
 w->used=true;
 w->minimized=true;
 w->owner_service=0;
 snprintf(w->title,sizeof w->title,"Workshop computer");
 return true;
}
static bool workshop_terminal_live(const zell_scene *s){
 return s->service_phase==5&&!strcmp(s->terminal_title,"Workshop computer");
}
static bool workshop_window_focused(const zell_scene *s){
 return s->mode==ZELL_MODE_SERVICE&&s->service_phase==5&&!s->terminal_minimized;
}
static void workshop_mirror_terminal(zell_scene *s){
 if(strcmp(s->terminal_title,"Workshop computer")!=0)return;
 int slot=workshop_slot(s);
 if(slot<0)return;
 s->windows[slot].minimized=s->terminal_minimized;
 s->windows[slot].maximized=s->terminal_maximized;
}
static void workshop_focus_live(zell_scene *s){
 int slot=workshop_slot(s);
 if(slot>=0)s->windows[slot].minimized=false;
 s->terminal_minimized=false;
 s->mode=ZELL_MODE_SERVICE;
 s->desktop_launch=false;
}
static void workshop_request_window(zell_scene *s){
 int slot;
 if(workshop_terminal_live(s)){workshop_focus_live(s);return;}
 if(workshop_slot(s)<0&&!workshop_reserve(s)){s->desktop_launch=false;return;}
 slot=workshop_slot(s);
 if(slot>=0)s->windows[slot].minimized=false;
 s->terminal_minimized=false;
 s->station=ZELL_WORKSHOP_COMPUTER;
 s->menu_index=0;
 s->desktop_launch=true;
}
/* What sitting down, or Enter while seated, does at this room's seat. */
static void seat_use(zell_scene *s){
 if(s->room==4){workshop_request_window(s);return;}
 s->station=ZELL_CHESS_STOOL;s->menu_index=0;s->menu_hold=0;
 s->mode=ZELL_MODE_DESK;s->desktop_launch=true;
}
static void workshop_land(zell_scene *s){
 s->seated=true;
 s->seat_ticks=ZELL_SEAT_HOP_TICKS;
 s->direction=room_seat(s->room)->facing;
 s->step=0;s->step_ticks=0;
 workshop_place(s,ZELL_SEAT_HOP_TICKS);
 seat_use(s);
}
static void workshop_hop_down(zell_scene *s){
 if(s->room==4){
  int slot=workshop_slot(s);
  if(slot>=0)s->windows[slot].minimized=true;
  s->terminal_minimized=true;
 }
 if(s->mode==ZELL_MODE_SERVICE)s->mode=ZELL_MODE_WALK;
 s->direction=room_seat(s->room)->facing;
 s->step=0;s->step_ticks=0;
 s->running=false;
 s->seat_ticks=ZELL_SEAT_HOP_TICKS-1;
 workshop_place(s,s->seat_ticks);
}
static void workshop_begin_hop(zell_scene *s){
 const zell_seat *seat=room_seat(s->room);
 if(!seat)return;
 if(s->room==5&&s->seated&&s->seat_ticks==ZELL_SEAT_HOP_TICKS){seat_use(s);return;}
 if(s->seated||s->seat_ticks>0)return;
 if(s->room==4){
  if(!workshop_reserve(s))return;
  if(s->notice_count>0&&!strcmp(s->notice_text,workshop_full_notice)){s->notice_count=0;s->notice_text[0]=0;}
 }
 s->running=false;
 s->station=seat->station;s->menu_index=0;
 s->mode=ZELL_MODE_WALK;s->desktop_launch=false;
 s->direction=seat->facing;s->step=0;s->step_ticks=0;
 s->seated=false;s->seat_ticks=1;
 workshop_place(s,1);
}
static void workshop_release_window(zell_scene *s,int slot){
 if(slot<0||slot>=ZELL_WINDOWS)return;
 memset(&s->windows[slot],0,sizeof s->windows[slot]);
}
static void workshop_on_frame(zell_scene *s){
 for(int i=0;i<ZELL_WINDOWS;i++){
  if(!s->windows[i].used||!s->windows[i].close_requested)continue;
  if(strcmp(s->windows[i].title,"Workshop computer")!=0)continue;
  workshop_release_window(s,i);
  if(s->mode==ZELL_MODE_SERVICE){s->mode=ZELL_MODE_WALK;s->service_cancel=true;}
 }
 if(s->station==ZELL_WORKSHOP_COMPUTER&&s->service_phase==4){
  int slot=workshop_slot(s);
  if(slot>=0)workshop_release_window(s,slot);
  if(strstr(s->service_text,"Could not start")){
   s->notice_count=1;
   zell_notice_apply(s,s->service_text);
  }
  s->mode=ZELL_MODE_WALK;
  s->service_phase=0;
  s->terminal_minimized=false;
 }
}
static bool workshop_seat_tick(zell_scene *s,const zell_controls *c){
 if(!s->seated&&s->seat_ticks<=0)return false;
 const zell_seat *seat=room_seat(s->room);
 /* A seat belongs to its room. Leaving the room any other way stands him up. */
 if(!seat){s->seated=false;s->seat_ticks=0;return false;}
 s->direction=seat->facing;s->step=0;s->step_ticks=0;s->running=false;
 if(s->seat_ticks>0&&s->seat_ticks<ZELL_SEAT_HOP_TICKS){
  if(s->seated){
   s->seat_ticks--;
   workshop_place(s,s->seat_ticks);
   if(s->seat_ticks==0)s->seated=false;
  }else{
   s->seat_ticks++;
   workshop_place(s,s->seat_ticks);
   if(s->seat_ticks==ZELL_SEAT_HOP_TICKS)workshop_land(s);
  }
  return true;
 }
 if(s->seated&&s->seat_ticks==ZELL_SEAT_HOP_TICKS){
  if(!workshop_window_focused(s)&&(c->move_x||c->move_y)){workshop_hop_down(s);return true;}
  if(c->confirm)seat_use(s);
  return true;
 }
 return false;
}
const zell_cave_hatch zell_cave_hatches[ZELL_DOWNSTAIRS_ROOMS+1]={
 [1]={ZELL_COMMAND_HATCH,    ZELL_ART_COMMAND_HATCH_OPEN,    "Command center"},
 [2]={ZELL_LAUNDRY_HATCH,    ZELL_ART_LAUNDRY_HATCH_OPEN,    "Laundry room"},
 [3]={ZELL_VAULT_HATCH,      ZELL_ART_VAULT_HATCH_OPEN,      "Vault"},
 [4]={ZELL_WORKSHOP_HATCH,   ZELL_ART_WORKSHOP_HATCH_OPEN,   "Workshop"},
 [5]={ZELL_GAME_HATCH,       ZELL_ART_GAME_HATCH_OPEN,       "Game room"},
 [6]={ZELL_THEATER_HATCH,    ZELL_ART_THEATER_HATCH_OPEN,    "Movie theater"},
 [7]={ZELL_MEDITATION_HATCH, ZELL_ART_MEDITATION_HATCH_OPEN, "Meditation"},
 [8]={ZELL_LIBRARY_HATCH,    ZELL_ART_LIBRARY_HATCH_OPEN,    "Library"}
};
int zell_hatch_room(int station){
 for(int room=1;room<=ZELL_DOWNSTAIRS_ROOMS;room++)
  if(zell_cave_hatches[room].station==station)return room;
 return 0;
}
const zell_cave_hatch *zell_station_hatch(int station){
 static const zell_cave_hatch puzzle={ZELL_PUZZLLM_HATCH,ZELL_ART_GAME_HATCH_OPEN,"PuzzLLM"};
 if(station==ZELL_PUZZLLM_HATCH)return &puzzle;
 int room=zell_hatch_room(station);
 return room?&zell_cave_hatches[room]:NULL;
}
static void open_station(zell_scene *s,int n){
 s->command_focus=0;
 if(n==ZELL_WORKSHOP_COMPUTER){workshop_begin_hop(s);return;}
 /* The launcher's search finds the stool from any room. */
 if(n==ZELL_CHESS_STOOL){
  if(s->room!=5){s->room=5;s->seated=false;s->seat_ticks=0;}
  workshop_begin_hop(s);return;
 }
 s->running=false;s->station=n;s->menu_index=0;s->menu_hold=0;
 int room=zell_hatch_room(n);
 if(n==28||room){
  if(n==28){
   int hatch=s->room>=1&&s->room<=ZELL_DOWNSTAIRS_ROOMS?zell_cave_hatches[s->room].station:ZELL_COMMAND_HATCH;
   s->room=0;s->x=zell_stations[hatch].x*ZELL_WORLD_W;s->y=zell_stations[hatch].y*ZELL_WORLD_H+50;
  }else{s->room=room;s->x=ZELL_WORLD_W*.50f;s->y=room==1?ZELL_COMMAND_LANDING_Y:ZELL_WORLD_H*ZELL_ROOM_ENTRY_Y;}
  /* The glass waits for a new confirmed read. The toggle does not guess. */
  if(s->room==1){s->coding_yolo_ready=false;s->coding_yolo_refresh=true;}
  s->mode=ZELL_MODE_WALK;s->step_ticks=0;s->desktop_launch=false;
  s->seated=false;s->seat_ticks=0;return;
 }
 if(n==10){s->voice_on=!s->voice_on;s->voice_changed=true;s->mode=ZELL_MODE_WALK;}
 else if(n==44){s->dictation_on=!s->dictation_on;s->dictation_changed=true;s->mode=ZELL_MODE_WALK;}
 else if(n==ZELL_FLOOR_PANEL){s->mode=ZELL_MODE_WALK;s->desktop_launch=false;}
 else {s->mode=ZELL_MODE_DESK;if(n==12||(n>=15&&n<=31)||(n>=45&&n<=47)||n==ZELL_COMMAND_COMPUTER||n==ZELL_PUZZLLM_HATCH||zell_arcade_game(n))s->desktop_launch=true;}}
#include "desktop_collide.inc"
int zell_desktop_tick(zell_scene *s,const zell_controls *c){
 if(!s->desktop)return 0;
 if(s->utility_panel){s->running=false;s->step=0;s->step_ticks=0;return 1;}
 /* Pumps the theater helper. A consumed key, including fullscreen Escape, wins. */
 if(zell_theater_input(s,c))return 1;
 if(s->zoom_ticks>0){if(++s->zoom_ticks>ZELL_ZOOM_TICKS){s->zoom_ticks=0;s->desktop_launch=true;}return 1;}
 if(ZELL_ROOM_IS_PANES(s->room))return zell_panes_tick(s,c);
 if(s->mode!=ZELL_MODE_WALK){s->running=false;}
 workshop_on_frame(s);

 if(s->mode==ZELL_MODE_SERVICE){
  /* A focused terminal keeps the Workshop seat. Movement does not hop down. */
  if(s->service_phase==5)return 1;
  if(s->service_phase==1)zell_edit_text(s->service_input,sizeof s->service_input,c->edits);
  if(s->service_phase==4&&(c->confirm||c->cancel)){s->mode=ZELL_MODE_DESK;return 1;}
  if(c->cancel){
   if(s->service_phase==0)s->service_cancel=true;
   else {strcpy(s->service_reply,"\033\n");s->service_send=true;s->service_phase=0;}
   return 1;
  }
  if(c->move_y&&s->menu_hold==0){
   if(s->service_phase==2&&s->service_count>0)s->service_index=(s->service_index+c->move_y+s->service_count)%s->service_count;
   else {s->service_scroll+=c->move_y;int limit=zell_service_scroll_limit(s);if(s->service_scroll<0)s->service_scroll=0;if(s->service_scroll>limit)s->service_scroll=limit;}
   s->menu_hold=5;
  }else if(s->menu_hold>0)s->menu_hold--;
  if(c->confirm&&s->service_phase>0){
   if(s->service_phase==2)snprintf(s->service_reply,sizeof s->service_reply,"%d\n",s->service_index);
   else snprintf(s->service_reply,sizeof s->service_reply,"%s\n",s->service_phase==1?s->service_input:"");
   s->service_send=true;s->service_phase=0;
  }
  return 1;
 }
 if(c->launcher&&s->mode!=ZELL_MODE_WAIT){s->running=false;s->mode=s->mode==ZELL_MODE_LAUNCHER?ZELL_MODE_WALK:ZELL_MODE_LAUNCHER;s->menu_index=0;s->menu_hold=0;s->launcher_query[0]=0;return 1;}
 if(s->mode==ZELL_MODE_WALK){
  if(s->room==1&&s->command_focus)return 1;
  if(workshop_seat_tick(s,c))return 1;
  float dx=(float)c->move_x,dy=(float)c->move_y;
  float d=sqrtf(dx*dx+dy*dy);
  s->running=c->run&&d>0;
  float speed=(s->running?ZELL_RUN_SPEED:ZELL_WALK_SPEED)*zell_desktop_speed_factor(s);
  float start_x=s->x,start_y=s->y;
  if(d>0&&speed>0){
   s->direction=dx<0?2:dx>0?3:dy<0?1:0;
   /* Substeps retain walking collision precision at dash speed. */
   int steps=(int)ceilf(speed/ZELL_WALK_SPEED);
   for(int step=0;step<steps;step++){
    float oldx=s->x,oldy=s->y;
    s->x+=dx/d*speed/(float)steps;s->y+=dy/d*speed/(float)steps;
    float nx=s->x/(float)ZELL_WORLD_W,ny=s->y/(float)ZELL_WORLD_H;
    /* The Command Center has its own lower-corner thresholds. */
    if(s->room!=1&&nx<.135f)s->x=(float)ZELL_WORLD_W*.135f;
    if(s->room!=1&&nx>.865f)s->x=(float)ZELL_WORLD_W*.865f;
    {
     float north=zell_desktop_north(s->room);
     if(s->y<north)s->y=north;
    }
    if(!open_south_edge(s->room)&&ny>.91f&&(nx<ZELL_STAIRS_LEFT||nx>ZELL_STAIRS_RIGHT))s->y=(float)ZELL_WORLD_H*.91f;
    /* L1 calls zell_desktop_blocked. Slide one axis when the other is free. */
    {
     float try_x=s->x,try_y=s->y;
     if(zell_desktop_blocked(s,try_x,oldy))s->x=oldx;
     if(zell_desktop_blocked(s,s->x,try_y))s->y=oldy;
     if(zell_desktop_blocked(s,s->x,s->y)){s->x=oldx;s->y=oldy;}
    }
    for(int i=0;i<ZELL_STATIONS;i++){
     if(!zell_desktop_station_visible(s,i)||i==28||i==ZELL_WORKSHOP_COMPUTER||i==ZELL_PUZZLLM_HATCH||i==ZELL_CHESS_STOOL||zell_arcade_game(i))continue;
     /* The wall and floor panels already contributed their rectangles.
      * Only the Command Center staff need a separate body check. */
     if(s->room==1&&(i<29||i>31))continue;
     if(s->room==0&&i>=6&&!(i>=32&&i<=41))continue;
     float ax=s->x-zell_stations[i].x*ZELL_WORLD_W,ay=s->y-zell_stations[i].y*ZELL_WORLD_H;
     if(s->room==1){
      /* The staff's feet occupy a short strip. Walk past their shoulders
       * on the south floor, or slide along the feet on diagonal contact. */
      if(fabsf(ax)<20.f&&fabsf(ay)<8.f){
       if(fabsf(oldy-zell_stations[i].y*ZELL_WORLD_H)>=8.f)s->y=oldy;
       else s->x=oldx;
       break;
      }
      continue;
     }
     if(ax*ax+ay*ay<26*26){s->x=oldx;s->y=oldy;break;}
    }
    /* Walk sideways through a tunnel at either end of the south floor. */
    if(s->room==1){
     int passage=command_passage(s->x,s->y);
     if(passage){zell_panes_enter(s,passage);return 1;}
    }
    if(zell_desktop_on_stairs(s)){open_station(s,28);return 1;}
    if(s->x==oldx&&s->y==oldy)break;
   }
  }
  if(s->x!=start_x||s->y!=start_y){
   s->step_ticks++;s->step=(s->step_ticks/(s->running?3:7))%2;
  }else{
   s->step_ticks=0;s->step=0;
   s->running=false;
  }
  if(s->room==0&&s->y/(float)ZELL_WORLD_H>.975f)s->leave=true;
  if(c->confirm){int n=zell_desktop_near(s);
 if(n>=0)open_station(s,n);}
  if(c->cancel){s->running=false;s->mode=ZELL_MODE_LAUNCHER;s->menu_index=0;}
  return 1;
 }
 if(s->mode!=ZELL_MODE_DESK&&s->mode!=ZELL_MODE_LAUNCHER)return 0;
 if(s->mode==ZELL_MODE_LAUNCHER&&zell_edit_text(s->launcher_query,sizeof s->launcher_query,c->edits))s->menu_index=0;
 int count=s->mode==ZELL_MODE_LAUNCHER?zell_desktop_launcher_count(s):zell_desktop_actions(s->station);
 if(c->cancel){s->mode=ZELL_MODE_WALK;return 1;}
 /* The visible drive bins can change while the Laundry launcher is open.
  * Let the player see the new selection before accepting Enter. */
 if(s->menu_index>=count&&s->mode==ZELL_MODE_LAUNCHER){
  s->menu_index=count?count-1:0;
  return 1;
 }
 if(c->move_y&&s->menu_hold==0&&count){s->menu_index=(s->menu_index+c->move_y+count)%count;s->menu_hold=9;}else if(s->menu_hold>0)s->menu_hold--;
 if(c->confirm&&count){
  if(s->mode==ZELL_MODE_LAUNCHER){int a=zell_desktop_launcher_action(s,s->menu_index),station=zell_desktop_launcher_station(s,s->menu_index);open_station(s,station);if(a>=0&&s->mode==ZELL_MODE_DESK){s->menu_index=a;s->desktop_launch=true;}}
  /* The console's browser pages open after the screen has zoomed in. */
  else if(s->station==ZELL_MANA_CONSOLE&&s->menu_index<4)s->zoom_ticks=1;
  else s->desktop_launch=true;
  if(s->station==0&&s->menu_index==0&&s->desktop_launch){s->desktop_launch=false;s->mode=ZELL_MODE_QUESTION;s->question_len=0;s->question[0]=0;}
 }
 return 1;
}

/* L3 calls zell_world_to_screen. */
void zell_desktop_camera(const zell_scene *s,int *x,int *y){
 *x=(int)s->x-s->width/2;*y=(int)s->y-s->height/2;
 int maxx=ZELL_WORLD_W-s->width,maxy=ZELL_WORLD_H-s->height;
 if(maxx<0)maxx=0;
 if(maxy<0)maxy=0;
 if(*x<0)*x=0;
 if(*y<0)*y=0;
 if(*x>maxx)*x=maxx;
 if(*y>maxy)*y=maxy;
}

/* Kilix Land uses four walk phases at seven ticks per phase and staggered
 * one-pixel NPC idle bobs. Keep a local walk phase that resets on stopping. */
zell_actor_pose zell_desktop_pose(const zell_scene *s, int actor)
{
    static const int idle_y[4] = {0, -1, -1, 0};
    zell_actor_pose pose = {1, 0, 0};
    if (actor >= 6 && actor <= 8) {
        int phase = (s->flame_tick / 20 + actor) % 4;
        pose.art = (actor == 6 ? ZELL_ART_GIT_IDLE : actor == 7 ? ZELL_ART_PILOT_IDLE : ZELL_ART_KEY_MASTER_IDLE) + phase;
    } else if (actor >= 0 && actor < 6) {
        int phase = (s->flame_tick / 16 + actor * 3) % 4;
        pose.art = zell_stations[actor].art;
        pose.offset_y = idle_y[phase];
    } else if (s->mode == ZELL_MODE_WALK && s->step_ticks > 0) {
        int frame = ((s->step_ticks - 1) / (s->running?3:7)) % 4;
        pose.art = 17 + s->direction * 4 + frame;
    } else {
        pose.art = 1 + s->direction;
        pose.offset_y = idle_y[(s->flame_tick / 11) % 4];
    }
    return pose;
}

/* Windowed, the terminal takes about 70% of the pane; maximized, all of it
 * (bounded by the 150x40-cell shared surface). */
void zell_terminal_cells(const zell_scene *s,int *cols,int *rows){
 int c=(s->width-64)/8,r=(s->height-160)/16;
 if(!s->terminal_maximized){c=c*7/10;r=r*7/10;}
 if(c<20)c=20;
 if(c>150)c=150;
 if(r<8)r=8;
 if(r>40)r=40;
 *cols=c;*rows=r;
}
void zell_terminal_layout_for(const zell_scene *s,int width,int height,zell_terminal_layout *out){
 int w=s->terminal_width,h=s->terminal_height;
 out->image=(zell_rect){(width-w)/2,(height-h)/2,w,h};
 out->frame=(zell_rect){out->image.x-8,out->image.y-30,w+16,h+58};
 out->maximize=(zell_rect){out->frame.x+out->frame.w-34,out->frame.y+5,26,20};
 out->minimize=(zell_rect){out->maximize.x-30,out->frame.y+5,26,20};
 out->close=(zell_rect){out->minimize.x-30,out->frame.y+5,26,20};
 /* Window buttons share the bottom row and line up from the left. The
  * button is as wide as its title. */
 int title=(int)strlen(s->terminal_title[0]?s->terminal_title:"Neuronte terminal")*8+36;
 if(title>236)title=236;
 int fit=width-2*(width<360?width/4:93)-20-ZELL_WINDOW_ROW_X;
 if(fit>0&&title>fit)title=fit;
 out->tab=(zell_rect){ZELL_WINDOW_ROW_X,height-64,title,26};
}
static void window_note_terminal(zell_scene *s){
 if(!s->windows[0].used)return;
 s->windows[0].minimized=s->terminal_minimized;
 s->windows[0].maximized=s->terminal_maximized;
 s->windows[0].private_surface=s->terminal_private;
}
void zell_terminal_restore(zell_scene *s){
 if(!s->terminal_minimized)return;
 s->terminal_minimized=false;s->mode=ZELL_MODE_SERVICE;
 window_note_terminal(s);
 workshop_mirror_terminal(s);
}
static bool rect_holds(const zell_rect *r,int x,int y){return x>=r->x&&x<r->x+r->w&&y>=r->y&&y<r->y+r->h;}
static zell_rect hatch_world_rect(const zell_station *station,const zell_art *art){
 int x=(int)(station->x*(float)ZELL_WORLD_W),y=(int)(station->y*(float)ZELL_WORLD_H);
 int w=art?art->width:76,h=art?art->height:76;
 return (zell_rect){x-w/2,y+ZELL_HATCH_FOOT_OFFSET-h,w,h};
}
static int clicked_hatch(const zell_scene *s,int x,int y){
 int near=zell_desktop_near(s),margin=-1;
 /* A raised lid can reach the row above. Prefer the painted sprite at
  * the pointer, in front-to-back order, then the forgiving frame bounds. */
 for(int i=ZELL_STATIONS-1;i>=0;i--){
  if(!zell_desktop_station_visible(s,i))continue;
  const zell_cave_hatch *hatch=zell_station_hatch(i);
  if(!hatch)continue;
  const zell_station *p=&zell_stations[hatch->station];
  const zell_art *art=zell_art_get(hatch->station==near?hatch->open_art:p->art);
  zell_rect drawn=hatch_world_rect(p,art);
  if(!rect_holds(&drawn,x,y))continue;
  if(art&&(art->pixels[(y-drawn.y)*drawn.w+x-drawn.x]>>24)>32)
   return hatch->station;
  if(margin<0)margin=hatch->station;
 }
 return margin;
}
zell_rect zell_command_wall_bounds(void){
 return (zell_rect){0,ZELL_ROOM_HUD_BOTTOM,ZELL_WORLD_W,WALL_BOTTOM+WALL_FOOT_RIM-ZELL_ROOM_HUD_BOTTOM};
}
static void wall_tile(int col,int top,int bottom,zell_rect *frame,zell_rect *body){
 *frame=(zell_rect){WALL_SIDE_RIM+col*WALL_COL,top,WALL_COL,bottom-top};
 *body=(zell_rect){frame->x+7,top+32,WALL_COL-14,bottom-top-38};
}
void zell_command_term_layout(int index,zell_rect *frame,zell_rect *body){
 static const int columns[ZELL_COMMAND_TERMS]={1,0,2,0,2,1};
 if(index<0||index>=ZELL_COMMAND_TERMS){*frame=*body=(zell_rect){0};return;}
 int top=index<3?WALL_TOP:index==ZELL_COMMAND_SHELL?MANA_BOTTOM:WALL_SEAM;
 wall_tile(columns[index],top,index<3?WALL_SEAM:WALL_BOTTOM,frame,body);
}
void zell_monitor_layout(int screen,zell_rect *frame,zell_rect *display){
 zell_command_term_layout(screen>=0&&screen<5?screen:-1,frame,display);
}
void zell_fleet_board_layout(zell_rect *board){
 if(board)zell_floor_panel_layout(3,board,NULL);
}
bool zell_fleet_board_line(const zell_scene *s,int index,char *text,size_t capacity){
 if(!s||!text||capacity<8||index<0||index>=s->fleet_count||index>=ZELL_FLEET_ROWS)return false;
 const zell_fleet_row *row=&s->fleet_rows[index];
 int n=snprintf(text,capacity,"%s / %s / %d workers / %d live",
  row->display_name,row->project_label,row->worker_count,row->live_count);
 return n>0&&(size_t)n<capacity;
}
void zell_mana_console_layout(zell_rect *frame,zell_rect *display){
 wall_tile(1,WALL_SEAM,MANA_BOTTOM,frame,display);
}
void zell_command_computer_layout(zell_rect *frame,zell_rect *display){
 zell_command_term_layout(ZELL_COMMAND_SHELL,frame,display);
}
void zell_floor_panel_layout(int index,zell_rect *frame,zell_rect *glass){
 static const int stations[ZELL_FLOOR_PANELS]={46,47,ZELL_FLOOR_PANEL,ZELL_FLEET_BOARD};
 zell_rect r={0};
 if(index>=0&&index<ZELL_FLOOR_PANELS){
  const zell_station *p=&zell_stations[stations[index]];
  r=(zell_rect){(int)(p->x*ZELL_WORLD_W)-117,(int)(p->y*ZELL_WORLD_H)-112,234,112};
 }
 if(frame)*frame=r;
 if(glass)*glass=r.w?(zell_rect){r.x+6,r.y+5,r.w-12,r.h-11}:r;
}
void zell_command_switch_layout(int index,zell_rect *frame){
 zell_floor_panel_layout(index>=0&&index<2?index:-1,frame,NULL);
}
void zell_command_focus(zell_scene *s,int index){
 s->command_focus=index+1;
 s->running=false;s->step=0;s->step_ticks=0;
 if(index>=0)zell_command_term_retry(index);
}
void zell_utility_rects(const zell_scene *s,zell_utility_layout *l){
 int w=s->width,h=s->height,tab=w<360?w/4:93,bottom=h-38;
 *l=(zell_utility_layout){0};
 l->updates=(zell_rect){w-tab-6,bottom-26,tab,26};
 l->messages=(zell_rect){w-2*tab-11,bottom-26,tab,26};
 int bw=w-16,bh=h-80;
 int maxw=s->utility_term?920:640;
 int maxh=s->utility_term?680:s->utility_panel==ZELL_UPDATES_PANEL?244:445;
 if(bw>maxw)bw=maxw;
 if(bh>maxh)bh=maxh;
 l->box=(zell_rect){w-bw-8,bottom-29-bh,bw,bh};
 /* A short pane uses the whole canvas for the open panel. */
 bool short_pane=h<240;
 if(short_pane){bw=w-8;if(bw>maxw)bw=maxw;bh=h-8;l->box=(zell_rect){w-bw-4,4,bw,bh};}
 int x=l->box.x,y=l->box.y;
 l->close=(zell_rect){x+bw-32,y+4,26,22};
 l->older=(zell_rect){x+bw-158,y+4,56,22};
 l->newer=(zell_rect){x+bw-98,y+4,56,22};
 l->all=(zell_rect){x+12,y+34,44,22};
 l->stamina=(zell_rect){x+62,y+34,78,22};
 if(bw<300){
  int cell=(bw-8)/4;
  l->all=(zell_rect){x+4,y+31,cell-1,20};
  l->stamina=(zell_rect){x+4+cell,y+31,cell-1,20};
  l->older=(zell_rect){x+4+2*cell,y+31,cell-1,20};
  l->newer=(zell_rect){x+4+3*cell,y+31,cell-1,20};
 }
 int start=y+(h<330?77:104),row=h<420?21:25;
 if(short_pane){start=y+(s->utility_term?30:62);row=16;}
 int columns=h<300?2:1,margin=short_pane?4:10,cell=(bw-2*margin)/columns;
 int button_h=short_pane?row-1:row-2;
 int rows=(ZELL_MAINT_ACTIONS+columns-1)/columns;
 if(s->utility_term&&h<380){
  l->action[s->utility_term-1]=(zell_rect){x+margin,start,bw-2*margin,button_h};
  rows=1;
 }else for(int i=0;i<ZELL_MAINT_ACTIONS;i++)
  l->action[i]=(zell_rect){x+margin+i%columns*cell,start+i/columns*row,cell-(columns-1)*2,button_h};
 int yy=start+rows*row+4;
 l->body=(zell_rect){x+12,yy,bw-24,y+bh-yy-9};
}
bool zell_utility_hit(const zell_scene *s,int x,int y){
 if(!s->desktop||zell_theater_fullscreen())return false;
 zell_utility_layout l;zell_utility_rects(s,&l);
 return rect_holds(&l.messages,x,y)||rect_holds(&l.updates,x,y)||
        (s->utility_panel&&rect_holds(&l.box,x,y));
}
int zell_input_terminal(const zell_scene *s){
 if(s->utility_panel)return s->utility_panel==ZELL_UPDATES_PANEL&&s->utility_term?
     ZELL_COMMAND_TERMS+s->utility_term-1:-1;
 return s->desktop&&s->room==1&&s->mode==ZELL_MODE_WALK?s->command_focus-1:-1;
}
static int message_view_count(const zell_scene *s){
 if(!s->stamina_messages_only)return s->message_count;
 int count=0;
 for(int i=0;i<s->message_count;i++)if(s->messages[i].stamina)count++;
 return count;
}
void zell_messages_scroll(zell_scene *s,int step){
 int last=message_view_count(s)-1;
 s->message_scroll+=step;
 if(s->message_scroll>last)s->message_scroll=last;
 if(s->message_scroll<0)s->message_scroll=0;
}
void zell_message_add(zell_scene *s,int64_t stamp,const char *source,const char *text,bool stamina){
 if(!text||!*text||stamp<=0)return;
 int pos=0,view_pos=0,same=0,last_same=-1;
 for(int i=0;i<s->message_count;i++){
  const zell_message *m=&s->messages[i];
  if(m->stamina==stamina){
   if(!strcmp(source,m->source)&&!strcmp(text,m->text)&&
      (stamina?stamp==m->time:llabs(stamp-m->time)<10))return;
   last_same=i;same++;
  }
  if(m->time>stamp){
   pos=i+1;
   if(!s->stamina_messages_only||m->stamina)view_pos++;
  }
 }
 /* Each source group keeps its own oldest slot in this chronological list. */
 bool full=same==(stamina?ZELL_STAMINA_MESSAGES:ZELL_SYSTEM_MESSAGES);
 int end=full?last_same:s->message_count;
 if(pos>end)return;
 if(!full)s->message_count++;
 memmove(&s->messages[pos+1],&s->messages[pos],(size_t)(end-pos)*sizeof s->messages[0]);
 zell_message *m=&s->messages[pos];m->time=stamp;m->stamina=stamina;
 snprintf(m->source,sizeof m->source,"%.31s",source);snprintf(m->text,sizeof m->text,"%.319s",text);
 if(s->utility_panel==ZELL_MESSAGES_PANEL&&(!s->stamina_messages_only||stamina)&&
    s->message_scroll>0&&view_pos<=s->message_scroll&&(!full||s->message_scroll<(s->stamina_messages_only?same-1:last_same)))
  s->message_scroll++;
}
static bool utility_click(zell_scene *s){
 if(zell_theater_fullscreen())return false;
 if(!s->desktop||!s->mouse_valid||!s->mouse_click)return false;
 int x=s->mouse_x,y=s->mouse_y;
 zell_utility_layout l;zell_utility_rects(s,&l);
 int panel=rect_holds(&l.messages,x,y)?ZELL_MESSAGES_PANEL:rect_holds(&l.updates,x,y)?ZELL_UPDATES_PANEL:0;
 if(panel&&!(s->utility_panel&&rect_holds(&l.box,x,y))){
  s->utility_panel=s->utility_panel==panel?0:panel;s->utility_term=0;s->message_scroll=0;
  s->command_focus=0;
 }else if(!s->utility_panel)return false;
 else if(!rect_holds(&l.box,x,y)||rect_holds(&l.close,x,y)){s->utility_panel=0;s->utility_term=0;}
 else if(s->utility_panel==ZELL_MESSAGES_PANEL){
  if(rect_holds(&l.all,x,y)||rect_holds(&l.stamina,x,y)){
   s->stamina_messages_only=rect_holds(&l.stamina,x,y);s->message_scroll=0;
  }else if(rect_holds(&l.older,x,y))zell_messages_scroll(s,1);
  else if(rect_holds(&l.newer,x,y))zell_messages_scroll(s,-1);
 }else for(int i=0;i<ZELL_MAINT_ACTIONS;i++){
  if(!rect_holds(&l.action[i],x,y))continue;
  s->utility_term=s->utility_term==i+1?0:i+1;
  if(s->utility_term)zell_command_term_retry(ZELL_COMMAND_TERMS+i);
  break;
 }
 s->mouse_click=false;return true;
}
bool zell_desktop_click(zell_scene *s){
 if((s->utility_panel||!zell_theater_menu_open())&&utility_click(s))return true;
 /* Screen-space theater hits consume the click before any world conversion. */
 if(zell_theater_click(s))return true;
 if(!s->desktop||!s->mouse_valid||!s->mouse_click||s->mode!=ZELL_MODE_WALK)return false;
 /* A click in a pane room picks the pane that typing goes to. */
 if(ZELL_ROOM_IS_PANES(s->room))return zell_panes_click(s);
 /* One inverse for every room. Hits are world rectangles, never the camera. */
 float wx,wy;zell_screen_to_world(s,(float)s->mouse_x,(float)s->mouse_y,&wx,&wy);
 int ix=(int)floorf(wx),iy=(int)floorf(wy);
 if(s->room==0){
  const int ids[3]={10,44,45};
  for(int i=0;i<3;i++){
   zell_rect hit={88+i*21,298,21,77};
   if(rect_holds(&hit,ix,iy)){s->mouse_click=false;open_station(s,ids[i]);return true;}
  }
 }
 if(s->room==0||s->room==4){
  int hatch=clicked_hatch(s,ix,iy);
  if(hatch>=0){s->mouse_click=false;open_station(s,hatch);return true;}
 }
 if(s->room==5){
  /* The stool's cushion and legs, and each cabinet's painted body. */
  const zell_rect stool={317,495,108,133};
  int station=rect_holds(&stool,ix,iy)?ZELL_CHESS_STOOL:-1;
  for(size_t i=0;i<sizeof arcades/sizeof arcades[0]&&station<0;i++)
   if(rect_holds(&arcades[i].body,ix,iy))station=arcades[i].station;
  if(station>=0){s->mouse_click=false;open_station(s,station);return true;}
 }
 if(s->room!=1)return false;
 s->command_focus=0;
 for(int i=0;i<ZELL_FLOOR_PANELS;i++){
  zell_rect frame;zell_floor_panel_layout(i,&frame,NULL);
  if(!rect_holds(&frame,ix,iy))continue;
  s->mouse_click=false;
  if(i<2)open_station(s,46+i);
  return true;
 }
 zell_rect frame,body;zell_mana_console_layout(&frame,&body);
 if(rect_holds(&frame,ix,iy)){s->mouse_click=false;open_station(s,ZELL_MANA_CONSOLE);return true;}
 for(int i=0;i<ZELL_COMMAND_TERMS;i++){
  zell_command_term_layout(i,&frame,&body);
  if(!rect_holds(&frame,ix,iy))continue;
  s->mouse_click=false;
  if(iy<body.y)open_station(s,i==ZELL_COMMAND_SHELL?ZELL_COMMAND_COMPUTER:15+i);
  else zell_command_focus(s,i);
  return true;
 }
 return false;
}
/* Consume one left press. True when it pressed a terminal window button. */
bool zell_terminal_click(zell_scene *s){
 if(!s->mouse_click)return false;
 s->mouse_click=false;
 if(!s->desktop||s->service_phase!=5||!s->mouse_valid)return false;
 zell_terminal_layout l;zell_terminal_layout_for(s,s->width,s->height,&l);
 if(s->terminal_minimized){
  if(!rect_holds(&l.tab,s->mouse_x,s->mouse_y))return false;
  zell_terminal_restore(s);return true;
 }
 if(s->mode!=ZELL_MODE_SERVICE||s->terminal_width<=0)return false;
 /* Close asks this slot to stop. Escape stays a key for the terminal. */
 if(rect_holds(&l.close,s->mouse_x,s->mouse_y)){s->windows[0].close_requested=true;return true;}
 if(rect_holds(&l.minimize,s->mouse_x,s->mouse_y)){s->terminal_minimized=true;s->mode=ZELL_MODE_WALK;window_note_terminal(s);workshop_mirror_terminal(s);return true;}
 if(rect_holds(&l.maximize,s->mouse_x,s->mouse_y)){s->terminal_maximized=!s->terminal_maximized;window_note_terminal(s);workshop_mirror_terminal(s);return true;}
 return false;
}

#include "desktop_command.inc"
/* L3 replaces zell_room_view_identity with the fit in src/desktop_command.inc. */
void zell_room_view_identity(zell_scene *s){
 zell_room_view_fit(s);
}
void zell_world_to_screen(const zell_scene *s,float world_x,float world_y,float *screen_x,float *screen_y){
 const zell_room_view *v=&s->room_view;
 *screen_x=(float)v->viewport_x+(world_x-(float)v->logical_x)*v->scale+(float)v->offset_x;
 *screen_y=(float)v->viewport_y+(world_y-(float)v->logical_y)*v->scale+(float)v->offset_y;
}
void zell_screen_to_world(const zell_scene *s,float screen_x,float screen_y,float *world_x,float *world_y){
 const zell_room_view *v=&s->room_view;
 float scale=v->scale==0.f?1.f:v->scale;
 *world_x=(float)v->logical_x+(screen_x-(float)v->viewport_x-(float)v->offset_x)/scale;
 *world_y=(float)v->logical_y+(screen_y-(float)v->viewport_y-(float)v->offset_y)/scale;
}
bool zell_window_accepts_frame(const zell_scene *s,int slot,int generation){
 if(!s||slot<0||slot>=ZELL_WINDOWS)return false;
 if(generation==0)return slot==0;
 return s->windows[slot].generation==generation;
}

static bool token_is(const char *s,size_t n,const char *lit){
 size_t m=strlen(lit);
 return n==m&&memcmp(s,lit,n)==0;
}
static bool token_in(const char *s,size_t n,const char *const *list,size_t count){
 for(size_t i=0;i<count;i++)if(token_is(s,n,list[i]))return true;
 return false;
}
static bool parse_uint(const char *s,size_t n,int *out,int max_value){
 if(n==0||n>10)return false;
 long long value=0;
 for(size_t i=0;i<n;i++){
  if(s[i]<'0'||s[i]>'9')return false;
  value=value*10+(s[i]-'0');
 }
 if(value>max_value)return false;
 *out=(int)value;
 return true;
}
static bool parse_percent(const char *s,size_t n,int *out){
 if(n==2&&s[0]=='-'&&s[1]=='1'){*out=-1;return true;}
 return parse_uint(s,n,out,100);
}
static bool copy_token(char *dst,size_t cap,const char *s,size_t n){
 if(n>=cap)return false;
 memcpy(dst,s,n);
 dst[n]=0;
 return true;
}
static int split_lines(const char *text,const char **start,size_t *len,int cap){
 size_t total=strlen(text);
 if(total>0&&text[total-1]=='\n')total--;
 if(total==0)return 0;
 int n=0;
 size_t i=0;
 for(;;){
  size_t begin=i;
  while(i<total&&text[i]!='\n')i++;
  if(n>=cap)return -1;
  start[n]=text+begin;
  len[n]=i-begin;
  n++;
  if(i>=total)break;
  i++;
  if(i==total){
   if(n>=cap)return -1;
   start[n]=text+i;
   len[n]=0;
   n++;
   break;
  }
 }
 return n;
}
static int split_spaces(const char *s,size_t n,const char **tok,size_t *toklen,int cap){
 int count=0;
 size_t i=0;
 if(n==0)return 0;
 if(s[0]==' '||s[n-1]==' ')return -1;
 while(i<n){
  size_t begin=i;
  while(i<n&&s[i]!=' '){
   unsigned char c=(unsigned char)s[i];
   if(c<32||c==127)return -1;
   i++;
  }
  if(begin==i)return -1;
  if(count>=cap)return -1;
  tok[count]=s+begin;
  toklen[count]=i-begin;
  count++;
  if(i==n)break;
  i++;
  if(i==n||s[i]==' ')return -1;
 }
 return count;
}
static int split_tabs(const char *s,size_t n,const char **tok,size_t *toklen,int expect){
 int count=0;
 size_t begin=0;
 for(size_t i=0;i<=n;i++){
  if(i<n&&s[i]!='\t'){
   unsigned char c=(unsigned char)s[i];
   if(c<32||c==127)return -1;
   continue;
  }
  if(count>=expect)return -1;
  tok[count]=s+begin;
  toklen[count]=i-begin;
  count++;
  begin=i+1;
 }
 return count==expect?count:-1;
}
static bool label_ok(const char *s,size_t n){
 if(n<1||n>95)return false;
 for(size_t i=0;i<n;i++){
  unsigned char c=(unsigned char)s[i];
  if(c<32||c==127)return false;
 }
 return true;
}
static bool fleet_id_ok(const char *s,size_t n){
 if(n<1||n>63)return false;
 for(size_t i=0;i<n;i++){
  char c=s[i];
  if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-')continue;
  return false;
 }
 return true;
}
static int provider_percent(const zell_scene *s,const char *id){
 for(int i=0;i<s->provider_count;i++)if(!strcmp(s->provider_rows[i].id,id))return s->provider_rows[i].percent;
 return -1;
}
bool zell_provider_apply(zell_scene *s,const char *text){
 static const char *const ids[]={"codex","claude","grok","qwen","minimax","gemini","elevenlabs"};
 static const char *const states[]={"configured","verified","unknown","stale","invalid"};
 static const char *const sources[]={"unknown","api","cli","vault"};
 const char *line[40];
 size_t line_len[40];
 const char *tok[8];
 size_t toklen[8];
 zell_provider_row parsed[ZELL_PROVIDER_ROWS];
 int lines,fields,generation=0,count=0;
 if(!s||!text)return false;
 lines=split_lines(text,line,line_len,40);
 if(lines<1)return false;
 fields=split_spaces(line[0],line_len[0],tok,toklen,8);
 if(fields!=3||!token_is(tok[0],toklen[0],"1"))return false;
 if(!parse_uint(tok[1],toklen[1],&generation,1000000000))return false;
 if(!parse_uint(tok[2],toklen[2],&count,ZELL_PROVIDER_ROWS))return false;
 if(lines!=count+1)return false;
 memset(parsed,0,sizeof parsed);
 for(int row=0;row<count;row++){
  int percent=0;
  fields=split_spaces(line[row+1],line_len[row+1],tok,toklen,8);
  if(fields!=5)return false;
  if(!token_in(tok[0],toklen[0],ids,sizeof ids/sizeof ids[0]))return false;
  if(!parse_percent(tok[1],toklen[1],&percent))return false;
  if(!token_in(tok[2],toklen[2],states,sizeof states/sizeof states[0]))return false;
  if(!token_in(tok[3],toklen[3],sources,sizeof sources/sizeof sources[0]))return false;
  if(!token_is(tok[4],toklen[4],"unknown"))return false;
  if(!copy_token(parsed[row].id,sizeof parsed[row].id,tok[0],toklen[0]))return false;
  parsed[row].percent=percent;
  if(!copy_token(parsed[row].state,sizeof parsed[row].state,tok[2],toklen[2]))return false;
  if(!copy_token(parsed[row].source,sizeof parsed[row].source,tok[3],toklen[3]))return false;
  if(!copy_token(parsed[row].plan,sizeof parsed[row].plan,tok[4],toklen[4]))return false;
  for(int prev=0;prev<row;prev++)if(!strcmp(parsed[prev].id,parsed[row].id))return false;
 }
 memset(s->provider_rows,0,sizeof s->provider_rows);
 if(count>0)memcpy(s->provider_rows,parsed,(size_t)count*sizeof parsed[0]);
 s->provider_count=count;
 s->provider_generation=generation;
 s->provider_mana[0]=provider_percent(s,"codex");
 s->provider_mana[1]=provider_percent(s,"claude");
 s->provider_mana[2]=provider_percent(s,"grok");
 s->provider_mana_ready=true;
 return true;
}
bool zell_provider_check_apply(zell_scene *s,const char *text){
 const char *line[4];
 size_t line_len[4];
 const char *tok[4];
 size_t toklen[4];
 int lines,fields,generation=0,running=0;
 if(!s||!text)return false;
 lines=split_lines(text,line,line_len,4);
 if(lines!=1)return false;
 fields=split_spaces(line[0],line_len[0],tok,toklen,4);
 if(fields!=3||!token_is(tok[0],toklen[0],"1"))return false;
 if(!parse_uint(tok[1],toklen[1],&generation,1000000000))return false;
 if(!parse_uint(tok[2],toklen[2],&running,1))return false;
 /* A late result must not clear a newer check. */
 if(generation<s->provider_check_generation)return false;
 s->provider_check_generation=generation;
 s->provider_check_running=running;
 return true;
}
bool zell_fleet_apply(zell_scene *s,const char *text){
 const char *line[40];
 size_t line_len[40];
 const char *tok[8];
 size_t toklen[8];
 zell_fleet_row parsed[ZELL_FLEET_ROWS];
 int lines,fields,revision=0,total=0,offset=0,count=0;
 if(!s||!text||strlen(text)>3500)return false;
 lines=split_lines(text,line,line_len,40);
 if(lines<1)return false;
 fields=split_spaces(line[0],line_len[0],tok,toklen,8);
 if(fields!=5||!token_is(tok[0],toklen[0],"1"))return false;
 if(!parse_uint(tok[1],toklen[1],&revision,1000000000))return false;
 if(!parse_uint(tok[2],toklen[2],&total,1000000000))return false;
 if(!parse_uint(tok[3],toklen[3],&offset,1000000000))return false;
 if(!parse_uint(tok[4],toklen[4],&count,ZELL_FLEET_ROWS))return false;
 if(total==0){if(offset!=0||count!=0)return false;}
 else if((long long)offset+count>total)return false;
 if(lines!=count+1)return false;
 memset(parsed,0,sizeof parsed);
 for(int row=0;row<count;row++){
  const char *field[5];
  size_t field_len[5];
  int workers=0,live=0;
  if(split_tabs(line[row+1],line_len[row+1],field,field_len,5)!=5)return false;
  if(!fleet_id_ok(field[0],field_len[0]))return false;
  if(!label_ok(field[1],field_len[1])||!label_ok(field[2],field_len[2]))return false;
  if(!parse_uint(field[3],field_len[3],&workers,100000))return false;
  if(!parse_uint(field[4],field_len[4],&live,100000))return false;
  if(!copy_token(parsed[row].fleet_id,sizeof parsed[row].fleet_id,field[0],field_len[0]))return false;
  if(!copy_token(parsed[row].display_name,sizeof parsed[row].display_name,field[1],field_len[1]))return false;
  if(!copy_token(parsed[row].project_label,sizeof parsed[row].project_label,field[2],field_len[2]))return false;
  parsed[row].worker_count=workers;
  parsed[row].live_count=live;
 }
 memset(s->fleet_rows,0,sizeof s->fleet_rows);
 if(count>0)memcpy(s->fleet_rows,parsed,(size_t)count*sizeof parsed[0]);
 s->fleet_revision=revision;
 s->fleet_total=total;
 s->fleet_offset=offset;
 s->fleet_count=count;
 return true;
}
bool zell_update_apply(zell_scene *s,const char *text){
 const char *line[4];
 size_t line_len[4];
 const char *tok[8];
 size_t toklen[8];
 char state[16];
 int lines,fields,generation=0,done=0,total=0;
 if(!s||!text)return false;
 lines=split_lines(text,line,line_len,4);
 if(lines!=1)return false;
 fields=split_spaces(line[0],line_len[0],tok,toklen,8);
 if(fields!=5||!token_is(tok[0],toklen[0],"1"))return false;
 if(!parse_uint(tok[1],toklen[1],&generation,1000000000))return false;
 if(!parse_uint(tok[2],toklen[2],&done,1000000))return false;
 if(!parse_uint(tok[3],toklen[3],&total,1000000))return false;
 if(done>total)return false;
 if(!token_is(tok[4],toklen[4],"idle")&&!token_is(tok[4],toklen[4],"running")&&!token_is(tok[4],toklen[4],"done")&&!token_is(tok[4],toklen[4],"failed")&&!token_is(tok[4],toklen[4],"cancelled"))return false;
 memset(state,0,sizeof state);
 if(!copy_token(state,sizeof state,tok[4],toklen[4]))return false;
 if(s->update_job_active&&generation!=s->update_job_generation)return false;
 if(!s->update_job_active){
  s->update_job_generation=generation;
  s->update_job_active=true;
 }
 s->update_done=done;
 s->update_total=total;
 memcpy(s->update_state,state,sizeof s->update_state);
 return true;
}

static bool notice_clock_fixed;
static int64_t notice_clock_value;

int64_t zell_notice_clock_ms(void){
 struct timespec ts;
 if(notice_clock_fixed)return notice_clock_value;
 clock_gettime(CLOCK_MONOTONIC,&ts);
 return (int64_t)ts.tv_sec*1000+(int64_t)(ts.tv_nsec/1000000L);
}
void zell_notice_set_clock(int64_t ms){
 notice_clock_fixed=true;
 notice_clock_value=ms;
}
/* Identity of the full notice text, so a shared prefix is still a change. */
static void notice_identity(const char *text,char *out,size_t cap){
 uint64_t hash=14695981039346656037ull;
 for(const unsigned char *p=(const unsigned char *)text;*p;p++){
  hash^=(uint64_t)(*p);
  hash*=1099511628211ull;
 }
 snprintf(out,cap,"%016llx",(unsigned long long)hash);
}
void zell_notice_apply(zell_scene *s,const char *text){
 char incoming[320];
 if(!s)return;
 if(!text)text="";
 snprintf(incoming,sizeof incoming,"%.319s",text);
 if(!incoming[0]){
  s->notice_until_ms=0;
  s->notice_key[0]=0;
  s->notice_text[0]=0;
  return;
 }
 if(s->notice_key[0]&&strcmp(incoming,s->notice_text)==0)return;
 snprintf(s->notice_text,sizeof s->notice_text,"%s",incoming);
 zell_message_add(s,(int64_t)time(NULL),"Zell",incoming,false);
 notice_identity(s->notice_text,s->notice_key,sizeof s->notice_key);
 s->notice_until_ms=zell_notice_clock_ms()+8000;
}

#include "desktop_theater.inc"
#include "desktop_panes.inc"
#include "desktop_input_scan.inc"
