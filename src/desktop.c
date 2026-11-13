#include "desktop.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "draw.h"
#include "desktop_assets.h"
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
 {"Command screen","Coordinate the four worker sessions",.50f,.43f,ZELL_ART_CONTROLLER},
 {"Worker screen 1","Coding session",.22f,.42f,ZELL_ART_WORKER},
 {"Worker screen 2","Coding session",.78f,.42f,ZELL_ART_WORKER},
 {"Worker screen 3","Coding session",.22f,.70f,ZELL_ART_WORKER},
 {"Worker screen 4","Coding session",.78f,.70f,ZELL_ART_WORKER},
 {"Drive bin 1","Capacity and files",.22f,.46f,-1},
 {"Drive bin 2","Capacity and files",.40f,.46f,-1},
 {"Drive bin 3","Capacity and files",.58f,.46f,-1},
 {"Drive bin 4","Capacity and files",.76f,.46f,-1},
 {"Drive bin 5","Capacity and files",.22f,.72f,-1},
 {"Drive bin 6","Capacity and files",.40f,.72f,-1},
 {"Drive bin 7","Capacity and files",.58f,.72f,-1},
 {"Drive bin 8","Capacity and files",.76f,.72f,-1},
 {"Stairs to the cave","Return upstairs",.50f,.91f,ZELL_ART_COMMAND_STAIRS},
 {"Git Wizard","Projects, changes and health watches",.36f,.72f,ZELL_ART_GIT_IDLE},
 {"Pilot","Project fleets and session arrangements",.64f,.72f,ZELL_ART_PILOT_IDLE},
 {"Key Master","API keys, logins and encrypted secrets",.50f,.72f,ZELL_ART_KEY_MASTER_IDLE},
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
 {"YOLO switch","Enable or disable shared coding-agent YOLO mode",.21f,.88f,-1},
 {"Agent updates switch","Choose and update installed coding agents",.79f,.88f,-1},
 {"Mana console","Coding-token balances and the providers' usage pages",.38f,.58f,ZELL_ART_WORKER},
 {"Workshop computer","Reserved, not an available action.",0.f,0.f,-1},
 {"Fleet board","Reserved, not an available action.",0.f,0.f,-1},
 {"Floor panel","Reserved, not an available action.",0.f,0.f,-1}
};
static const char *const actions[ZELL_STATIONS][17]={
 {"Ask the wizard","First-run tour of Zell",NULL},
 {"System journal","User service journal","Boot warnings","Browse agent rollouts","Browse Kilix transcripts","Search a log","Open another log",NULL},
 {"Kilix settings","Software and models (Merchant)","Language, region and font size","System and user services","Connections (Signalman)","GPU and model settings","Update OS in floating terminal","Health, fatigue and mana",NULL},
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
 [49]={NULL}, [50]={NULL}, [51]={NULL}
};
int zell_desktop_actions(int station){int n=0;
 if(station<0||station>=ZELL_STATIONS)return 0;
 while(n<16&&actions[station][n])n++;
 return n;}
const char *zell_desktop_action(int station,int index){return index>=0&&index<zell_desktop_actions(station)?actions[station][index]:"";}
void zell_desktop_init(zell_scene *s){s->desktop=true;s->x=(float)ZELL_WORLD_W*.51f;s->y=(float)ZELL_WORLD_H*.48f;s->station=0;s->desktop_launch=false;s->hp=100;s->max_hp=100;s->fatigue=100;s->mana=-1;s->update_age_days=-1;s->mana_age=-1;s->os_health=-1;s->dirty_files=0;}
bool zell_desktop_station_visible(const zell_scene *s,int station){
 if(s->room==0)return station<ZELL_CAVE_STATIONS||station==32||station==37||station==38||(station>=42&&station<=45);
 if(station==28)return true;
 if(s->room==1)return (station>=15&&station<=19)||(station>=29&&station<=31)||(station>=46&&station<=48);
 if(s->room==2)return (station>=20&&station<20+s->disk_count)||station==34||station==35;
 /* The Sentinel keeps the Vault with its keeper; the Signalman and the
  * Merchant work in the Workshop. */
 return (s->room==3&&(station==40||station==39))||(s->room==4&&(station==41||station==33||station==36));
}
static bool contains_folded(const char *haystack,const char *needle){
 for(;*haystack;haystack++){size_t i=0;while(needle[i]&&haystack[i]&&tolower((unsigned char)haystack[i])==tolower((unsigned char)needle[i]))i++;if(!needle[i])return true;}
 return !*needle;
}
static int launcher_entry(const zell_scene *s,int index,int *action){
 bool search=s->launcher_query[0]!=0;int count=0;
 for(int station=0;station<ZELL_STATIONS;station++){
  if(!search){if(!zell_desktop_station_visible(s,station))continue;if(count++==index){if(action)*action=-1;return station;}continue;}
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
 if(s->fatigue>=0&&s->fatigue<reserve)reserve=s->fatigue;
 if(reserve<0)reserve=0;
 if(reserve>100)reserve=100;
 return .35f+.65f*(float)reserve/100.0f;
}
int zell_desktop_near(const zell_scene *s) {
 int best=-1;float dist=96.0f;
 for(int i=0;i<ZELL_STATIONS;i++){
  if(!zell_desktop_station_visible(s,i))continue;
  float dx=s->x-zell_stations[i].x*ZELL_WORLD_W;
  float dy=s->y-zell_stations[i].y*ZELL_WORLD_H;
  float d=sqrtf(dx*dx+dy*dy);
  if(d<dist){best=i;dist=d;}
 }
 return best;
}
/* The stairs at the bottom of a downstairs room are walked onto, not opened.
 * Rooms 1 and 2 leave through the corridor at the cave's .975 line. Rooms 3
 * and 4 keep the painted steps. */
bool zell_desktop_on_stairs(const zell_scene *s){
 if(s->room==0)return false;
 float nx=s->x/(float)ZELL_WORLD_W,ny=s->y/(float)ZELL_WORLD_H;
 if(nx<ZELL_STAIRS_LEFT||nx>ZELL_STAIRS_RIGHT)return false;
 if(s->room>=3)return ny>ZELL_STAIRS_PAINTED_Y;
 return ny>.975f;
}
/* Objects standing on the floor hide Kilix when he is further from the viewer
 * than their feet. Floor and wall fittings never do. */
bool zell_desktop_station_in_front(const zell_scene *s,int i){
 if(i<0||i>=ZELL_STATIONS||!zell_desktop_station_visible(s,i))return false;
 bool stands=s->room==0?(i<6||(i>=32&&i<=41)):(i!=28&&i>=15&&i<46);
 return stands&&zell_stations[i].y*(float)ZELL_WORLD_H>s->y;
}
static void open_station(zell_scene *s,int n){s->running=false;s->run_charge=0;s->run_blocked=true;s->station=n;s->menu_index=0;s->menu_hold=0;
 if(n==13||n==14||n==28||n==42||n==43){
  if(n==28){int hatch=s->room==1?13:s->room==2?14:s->room==3?42:43;s->room=0;s->x=zell_stations[hatch].x*ZELL_WORLD_W;s->y=zell_stations[hatch].y*ZELL_WORLD_H+50;}
  else {s->room=n==13?1:n==14?2:n==42?3:4;s->x=ZELL_WORLD_W*.50f;s->y=ZELL_WORLD_H*ZELL_ROOM_ENTRY_Y;}
  if(s->room==1){s->coding_yolo_ready=false;s->coding_yolo_refresh=true;}
  s->mode=ZELL_MODE_WALK;s->step_ticks=0;s->desktop_launch=false;return;
 }
 if(n==10){s->voice_on=!s->voice_on;s->voice_changed=true;s->mode=ZELL_MODE_WALK;}
 else if(n==44){s->dictation_on=!s->dictation_on;s->dictation_changed=true;s->mode=ZELL_MODE_WALK;}
 else {s->mode=ZELL_MODE_DESK;if(n==12||(n>=15&&n<=31)||(n>=45&&n<=47))s->desktop_launch=true;}}
#include "desktop_collide.inc"
int zell_desktop_tick(zell_scene *s,const zell_controls *c){
 if(!s->desktop)return 0;
 if(s->zoom_ticks>0){if(++s->zoom_ticks>ZELL_ZOOM_TICKS){s->zoom_ticks=0;s->desktop_launch=true;}return 1;}
 if(!c->run){s->running=false;s->run_charge=0;s->run_blocked=false;}
 if(s->mode!=ZELL_MODE_WALK){s->running=false;s->run_charge=0;s->run_bump=0;s->run_blocked=c->run;}

 if(s->mode==ZELL_MODE_SERVICE){
  if(s->service_phase==5)return 1;
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
  if(s->service_phase==1){
   size_t n=strlen(s->service_input);
   if(c->backspace&&n)s->service_input[--n]=0;
   if(c->character>=32&&c->character<=126&&n+1<sizeof s->service_input){s->service_input[n]=(char)c->character;s->service_input[n+1]=0;}
  }
  if(c->confirm&&s->service_phase>0){
   if(s->service_phase==2)snprintf(s->service_reply,sizeof s->service_reply,"%d\n",s->service_index);
   else snprintf(s->service_reply,sizeof s->service_reply,"%s\n",s->service_phase==1?s->service_input:"");
   s->service_send=true;s->service_phase=0;
  }
  return 1;
 }
 if(c->launcher&&s->mode!=ZELL_MODE_WAIT){s->running=false;s->run_charge=0;s->run_blocked=c->run;s->mode=s->mode==ZELL_MODE_LAUNCHER?ZELL_MODE_WALK:ZELL_MODE_LAUNCHER;s->menu_index=0;s->menu_hold=0;s->launcher_query[0]=0;return 1;}
 if(s->mode==ZELL_MODE_WALK){
  float dx=(float)c->move_x,dy=(float)c->move_y;
  float d=sqrtf(dx*dx+dy*dy),speed=ZELL_WALK_SPEED;
  if(s->run_bump>0){s->run_bump--;dx=dy=d=0;}
  else if(c->run&&!s->run_blocked){
   if(s->run_charge==0&&!s->running){
    if(d>0)s->direction=dx<0?2:dx>0?3:dy<0?1:0;
    s->run_direction=s->direction;s->step_ticks=0;
   }
   if(!s->running){
    s->run_charge++;
    if(s->run_charge>=ZELL_RUN_CHARGE_TICKS){s->running=true;s->step_ticks=0;}
   }
   /* Charge in place, then commit to one cardinal direction until release. */
   s->direction=s->run_direction;
   dx=s->direction==2?-1.0f:s->direction==3?1.0f:0.0f;
   dy=s->direction==1?-1.0f:s->direction==0?1.0f:0.0f;
   d=1;speed=s->running?ZELL_RUN_SPEED:0;
  }
  speed*=zell_desktop_speed_factor(s);
  bool blocked=false;
  if(d>0&&speed>0){
   if(!s->running)s->direction=dx<0?2:dx>0?3:dy<0?1:0;
   /* Substeps retain walking collision precision at dash speed. */
   int steps=(int)ceilf(speed/ZELL_WALK_SPEED);
   for(int step=0;step<steps;step++){
    float oldx=s->x,oldy=s->y;
    s->x+=dx/d*speed/(float)steps;s->y+=dy/d*speed/(float)steps;
    float nx=s->x/(float)ZELL_WORLD_W,ny=s->y/(float)ZELL_WORLD_H;
    if(nx<.135f){s->x=(float)ZELL_WORLD_W*.135f;blocked=true;}
    if(nx>.865f){s->x=(float)ZELL_WORLD_W*.865f;blocked=true;}
    if(ny<.32f){s->y=(float)ZELL_WORLD_H*.32f;blocked=true;}
    if(ny>.91f&&(nx<ZELL_STAIRS_LEFT||nx>ZELL_STAIRS_RIGHT)){s->y=(float)ZELL_WORLD_H*.91f;blocked=true;}
    /* L1 calls zell_desktop_blocked. Slide one axis when the other is free. */
    {
     float try_x=s->x,try_y=s->y;
     if(zell_desktop_blocked(s,try_x,oldy)){s->x=oldx;blocked=true;}
     if(zell_desktop_blocked(s,s->x,try_y)){s->y=oldy;blocked=true;}
     if(zell_desktop_blocked(s,s->x,s->y)){s->x=oldx;s->y=oldy;blocked=true;}
    }
    for(int i=0;i<ZELL_STATIONS;i++){
     if(!zell_desktop_station_visible(s,i)||i==28||i==46||i==47||(s->room==0&&i>=6&&!(i>=32&&i<=41)))continue;
     float ax=s->x-zell_stations[i].x*ZELL_WORLD_W,ay=s->y-zell_stations[i].y*ZELL_WORLD_H;
     if(ax*ax+ay*ay<26*26){s->x=oldx;s->y=oldy;blocked=true;break;}
    }
    if(blocked)break;
    if(s->room==0&&s->y/(float)ZELL_WORLD_H>.975f)s->leave=true;
    if(zell_desktop_on_stairs(s)){open_station(s,28);return 1;}
   }
   s->step_ticks++;s->step=(s->step_ticks/(s->running?3:7))%2;
  }else if(s->run_charge>0&&!s->run_blocked){s->step_ticks++;}
  else {s->step=0;s->step_ticks=0;}
  if(blocked){
   s->step_ticks=0;s->step=0;
   if(s->running){s->running=false;s->run_charge=0;s->run_blocked=true;s->run_bump=6;}
  }
  if(s->room==0&&s->y/(float)ZELL_WORLD_H>.975f)s->leave=true;
  if(c->confirm){int n=zell_desktop_near(s);
 if(n>=0)open_station(s,n);}
  if(c->cancel){s->running=false;s->run_charge=0;s->run_blocked=c->run;s->mode=ZELL_MODE_LAUNCHER;s->menu_index=0;}
  return 1;
 }
 if(s->mode!=ZELL_MODE_DESK&&s->mode!=ZELL_MODE_LAUNCHER)return 0;
 if(s->mode==ZELL_MODE_LAUNCHER){
  size_t len=strlen(s->launcher_query);bool changed=false;
  if(c->backspace&&len){s->launcher_query[--len]=0;changed=true;}
  if(c->character>=32&&c->character<=126&&len+1<sizeof s->launcher_query){s->launcher_query[len]=(char)c->character;s->launcher_query[len+1]=0;changed=true;}
  if(changed)s->menu_index=0;
 }
 int count=s->mode==ZELL_MODE_LAUNCHER?zell_desktop_launcher_count(s):zell_desktop_actions(s->station);
 if(c->cancel){s->mode=ZELL_MODE_WALK;return 1;}
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
        int frame = ((s->step_ticks - 1) / ((s->running || s->run_charge>0)?3:7)) % 4;
        pose.art = 17 + s->direction * 4 + frame;
    } else {
        if(s->run_bump>0){pose.offset_x=s->direction==3?-2:s->direction==2?2:0;pose.offset_y=s->direction==0?-2:s->direction==1?2:0;}
        pose.art = 1 + s->direction;
        if(!s->run_bump)pose.offset_y = idle_y[(s->flame_tick / 11) % 4];
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
}
static bool rect_holds(const zell_rect *r,int x,int y){return x>=r->x&&x<r->x+r->w&&y>=r->y&&y<r->y+r->h;}
void zell_monitor_layout(int screen,zell_rect *frame,zell_rect *display){
 if(screen<0||screen>=5){*frame=*display=(zell_rect){0};return;}
 const zell_station *station=&zell_stations[15+screen];
 int w=screen==0?420:320,h=screen==0?310:235;
 *frame=(zell_rect){(int)(station->x*ZELL_WORLD_W)-w/2,(int)(station->y*ZELL_WORLD_H)-h,w,h};
 *display=(zell_rect){frame->x+w*11/100,frame->y+h*(screen==0?26:15)/100,w*78/100,h*(screen==0?52:62)/100};
}
void zell_mana_console_layout(zell_rect *frame,zell_rect *display){
 const zell_station *station=&zell_stations[ZELL_MANA_CONSOLE];
 int w=176,h=129;
 *frame=(zell_rect){(int)(station->x*ZELL_WORLD_W)-w/2,(int)(station->y*ZELL_WORLD_H)-h,w,h};
 *display=(zell_rect){frame->x+w*11/100,frame->y+h*15/100,w*78/100,h*62/100};
}
void zell_command_switch_layout(int index,zell_rect *frame){
 if(index<0||index>1){*frame=(zell_rect){0};return;}
 const zell_station *station=&zell_stations[46+index];
 *frame=(zell_rect){(int)(station->x*ZELL_WORLD_W)-150,(int)(station->y*ZELL_WORLD_H)-150,300,150};
}
bool zell_desktop_click(zell_scene *s){
 if(!s->desktop||!s->mouse_valid||!s->mouse_click||s->mode!=ZELL_MODE_WALK)return false;
 int cx,cy;zell_desktop_camera(s,&cx,&cy);
 float hit_x,hit_y;
 zell_screen_to_world(s,(float)s->mouse_x,(float)s->mouse_y,&hit_x,&hit_y);
 /* Identity view ignores the avatar camera, so that pan is still applied. */
 if(s->room==0){
  const int ids[3]={10,44,45};
  for(int i=0;i<3;i++){
   zell_rect hit={88+i*21-cx,298-cy,21,77};
   if(rect_holds(&hit,(int)hit_x,(int)hit_y)){s->mouse_click=false;open_station(s,ids[i]);return true;}
  }
 }
 if(s->room!=1)return false;
 hit_x+=(float)cx;hit_y+=(float)cy;
 for(int i=0;i<2;i++){
  zell_rect frame;zell_command_switch_layout(i,&frame);
  if(rect_holds(&frame,(int)hit_x,(int)hit_y)){s->mouse_click=false;open_station(s,46+i);return true;}
 }
 for(int i=0;i<5;i++){
  zell_rect frame,display;zell_monitor_layout(i,&frame,&display);
  if(rect_holds(&frame,(int)hit_x,(int)hit_y)){s->mouse_click=false;open_station(s,15+i);return true;}
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
 if(rect_holds(&l.minimize,s->mouse_x,s->mouse_y)){s->terminal_minimized=true;s->mode=ZELL_MODE_WALK;window_note_terminal(s);return true;}
 if(rect_holds(&l.maximize,s->mouse_x,s->mouse_y)){s->terminal_maximized=!s->terminal_maximized;window_note_terminal(s);return true;}
 return false;
}

/* L3 replaces zell_room_view_identity with the fit in src/desktop_command.inc. */
void zell_room_view_identity(zell_scene *s){
 s->room_view.logical_x=0;
 s->room_view.logical_y=0;
 s->room_view.logical_w=ZELL_WORLD_W;
 s->room_view.logical_h=ZELL_WORLD_H;
 s->room_view.viewport_x=0;
 s->room_view.viewport_y=0;
 s->room_view.viewport_w=s->width;
 s->room_view.viewport_h=s->height;
 s->room_view.scale=1.f;
 s->room_view.offset_x=0;
 s->room_view.offset_y=0;
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
