#include "desktop.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "draw.h"
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
 {"Voice","Toggle the wizard's voice",.10f,.39f,16},
 {"Wall clock","Time and system health",.88f,.32f,13},
 {"Neuronte shell switch","Neural terminal / behind the shell computer",.90f,.42f,-1},
 {"Command center hatch","Five connected coding screens downstairs",.30f,.65f,-1},
 {"Laundry room hatch","Drive bins and storage explorer downstairs",.78f,.64f,-1},
 {"Command screen","Coordinate the four worker sessions",.20f,.37f,-1},
 {"Worker screen 1","Coding session",.35f,.37f,-1},
 {"Worker screen 2","Coding session",.50f,.37f,-1},
 {"Worker screen 3","Coding session",.65f,.37f,-1},
 {"Worker screen 4","Coding session",.80f,.37f,-1},
 {"Drive bin 1","Capacity and files",.22f,.46f,-1},
 {"Drive bin 2","Capacity and files",.40f,.46f,-1},
 {"Drive bin 3","Capacity and files",.58f,.46f,-1},
 {"Drive bin 4","Capacity and files",.76f,.46f,-1},
 {"Drive bin 5","Capacity and files",.22f,.72f,-1},
 {"Drive bin 6","Capacity and files",.40f,.72f,-1},
 {"Drive bin 7","Capacity and files",.58f,.72f,-1},
 {"Drive bin 8","Capacity and files",.76f,.72f,-1},
 {"Stairs to the cave","Return upstairs",.50f,.90f,-1},
 {"Git Wizard","Projects, changes and health watches",.30f,.65f,5},
 {"Pilot","Project fleets and session arrangements",.70f,.65f,7}
};
static const char *const actions[ZELL_STATIONS][9]={
 {"Ask the wizard",NULL},
 {"System journal","User service journal","Boot warnings","Browse agent rollouts","Browse Kilix transcripts","Search a log","Open another log",NULL},
 {"Kilix settings","Install a package","Installed packages","Services","Network status","GPU and model settings","Update OS in floating terminal","Health, fatigue and mana",NULL},
 {"System health report","Disk usage","Largest home folders","Preview and clean old thumbnails","Process monitor","Clean downloaded package cache",NULL},
 {"Open Kilix Amp","Play a file or URL","Playlists and creators","Follow a creator or playlist","Volume controls","Transcribe media","Transcripts and jobs","Music library on disk",NULL},
 {"Books and documents library","Find a document","OCR an image","OCR a PDF","Read text aloud","Document converter","Export a text audiobook","How to find and add files",NULL},
 {"Open Kilix TUI",NULL}, {"Open Kilix shell",NULL},
 {"Application launcher","Home folder","Downloads","Web browser",NULL},
 {"Read notebook","Add a note","Edit notebook",NULL},
 {"Toggle voice",NULL}, {"Time and calendar","System health report","Check coding-token mana",NULL},
 {"Switch to Neuronte shell",NULL},
 {"Go down to command center",NULL}, {"Go down to laundry room",NULL},
 {"Open controller screen",NULL}, {"Open worker screen",NULL}, {"Open worker screen",NULL}, {"Open worker screen",NULL}, {"Open worker screen",NULL},
 {"Dig through drive",NULL}, {"Dig through drive",NULL}, {"Dig through drive",NULL}, {"Dig through drive",NULL},
 {"Dig through drive",NULL}, {"Dig through drive",NULL}, {"Dig through drive",NULL}, {"Dig through drive",NULL},
 {"Return to the cave",NULL},
 {"Projects and Git health",NULL}, {"Launch a project fleet",NULL}
};
int zell_desktop_actions(int station){int n=0;
 if(station<0||station>=ZELL_STATIONS)return 0;
 while(n<8&&actions[station][n])n++;
 return n;}
const char *zell_desktop_action(int station,int index){return index>=0&&index<zell_desktop_actions(station)?actions[station][index]:"";}
void zell_desktop_init(zell_scene *s){s->desktop=true;s->x=(float)ZELL_WORLD_W*.51f;s->y=(float)ZELL_WORLD_H*.48f;s->station=0;s->desktop_launch=false;s->hp=100;s->max_hp=100;s->fatigue=100;s->mana=-1;s->update_age_days=-1;s->mana_age=-1;s->os_health=-1;s->dirty_files=0;}
bool zell_desktop_station_visible(const zell_scene *s,int station){
 if(s->room==0)return station<ZELL_CAVE_STATIONS;
 if(station==28)return true;
 return s->room==1?((station>=15&&station<=19)||station==29||station==30):(station>=20&&station<20+s->disk_count);
}
int zell_desktop_launcher_count(const zell_scene *s){
 int count=0;for(int i=0;i<ZELL_STATIONS;i++)if(zell_desktop_station_visible(s,i))count++;
 return count;
}
int zell_desktop_launcher_station(const zell_scene *s,int index){
 for(int i=0;i<ZELL_STATIONS;i++)if(zell_desktop_station_visible(s,i)&&index--==0)return i;
 return 0;
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
static void open_station(zell_scene *s,int n){s->running=false;s->run_charge=0;s->run_blocked=true;s->station=n;s->menu_index=0;s->menu_hold=0;
 if(n==13||n==14||n==28){
  if(n==28){int hatch=s->room==1?13:14;s->room=0;s->x=zell_stations[hatch].x*ZELL_WORLD_W;s->y=zell_stations[hatch].y*ZELL_WORLD_H+50;}
  else {s->room=n==13?1:2;s->x=ZELL_WORLD_W*.50f;s->y=ZELL_WORLD_H*.80f;}
  s->mode=ZELL_MODE_WALK;s->step_ticks=0;s->desktop_launch=false;return;
 }
 if(n==0){s->mode=ZELL_MODE_QUESTION;s->question_len=0;s->question[0]=0;}else if(n==10){s->voice_on=!s->voice_on;s->voice_changed=true;s->mode=ZELL_MODE_WALK;}else {s->mode=ZELL_MODE_DESK;if(n==12||n>=15)s->desktop_launch=true;}}
int zell_desktop_tick(zell_scene *s,const zell_controls *c){
 if(!s->desktop)return 0;
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
 if(c->launcher&&s->mode!=ZELL_MODE_WAIT){s->running=false;s->run_charge=0;s->run_blocked=c->run;s->mode=s->mode==ZELL_MODE_LAUNCHER?ZELL_MODE_WALK:ZELL_MODE_LAUNCHER;s->menu_index=0;s->menu_hold=0;return 1;}
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
    if(ny>.91f&&(s->room!=0||nx<.44f||nx>.56f)){s->y=(float)ZELL_WORLD_H*.91f;blocked=true;}
    for(int i=0;i<(s->room==0?6:ZELL_STATIONS);i++){
     if(s->room!=0&&(!zell_desktop_station_visible(s,i)||i==28))continue;
     float ax=s->x-zell_stations[i].x*ZELL_WORLD_W,ay=s->y-zell_stations[i].y*ZELL_WORLD_H;
     if(ax*ax+ay*ay<26*26){s->x=oldx;s->y=oldy;blocked=true;break;}
    }
    if(blocked)break;
    if(s->room==0&&s->y/(float)ZELL_WORLD_H>.975f)s->leave=true;
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
 int count=s->mode==ZELL_MODE_LAUNCHER?zell_desktop_launcher_count(s):zell_desktop_actions(s->station);
 if(c->cancel){s->mode=ZELL_MODE_WALK;return 1;}
 if(c->move_y&&s->menu_hold==0){s->menu_index=(s->menu_index+c->move_y+count)%count;s->menu_hold=9;}else if(s->menu_hold>0)s->menu_hold--;
 if(c->confirm){if(s->mode==ZELL_MODE_LAUNCHER)open_station(s,zell_desktop_launcher_station(s,s->menu_index));else s->desktop_launch=true;}
 return 1;
}

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
    if (actor >= 0 && actor < 6) {
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
 out->tab=(zell_rect){12,height-64,236,26};
}
void zell_terminal_restore(zell_scene *s){
 if(!s->terminal_minimized)return;
 s->terminal_minimized=false;s->mode=ZELL_MODE_SERVICE;
}
static bool rect_holds(const zell_rect *r,int x,int y){return x>=r->x&&x<r->x+r->w&&y>=r->y&&y<r->y+r->h;}
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
 if(rect_holds(&l.minimize,s->mouse_x,s->mouse_y)){s->terminal_minimized=true;s->mode=ZELL_MODE_WALK;return true;}
 if(rect_holds(&l.maximize,s->mouse_x,s->mouse_y)){s->terminal_maximized=!s->terminal_maximized;return true;}
 return false;
}
