/* Included after the Neuronte drawing adapters, so artwork and text use them. */
#include "desktop.h"
#include "desktop_assets.h"
#include <time.h>
static void desktop_art(sr_canvas *c,int id,int x,int foot,int height){
 const zell_art *a=zell_art_get(id);if(!a)return;sr_canvas src;sr_canvas_wrap(&src,a->pixels,a->width,a->height);
 int width=height*a->width/a->height;sr_blit_scaled(c,&src,x-width/2,foot-height,width,height,1.0f);
}
static void desktop_label(sr_canvas *c,int x,int y,const char *text,uint32_t color){
 int w=sr_text_width_in(SR_FONT_FIXED_8X16,text,1);
 sr_fill_rect(c,(float)(x-w/2-6),(float)(y-3),(float)(w+12),22,0x101f26u,.92f);
 sr_text(c,(float)(x-w/2),(float)y,text,color,1,1);
}
static void desktop_shell_switch(sr_canvas *c,int x,int y,bool near){
 /* A wall-mounted brass lever: fixed world pixels, independent of pane size. */
 sr_fill_rect(c,(float)(x-18),(float)(y-24),36,48,0x071219u,.85f);
 sr_fill_rect(c,(float)(x-15),(float)(y-22),30,44,0x294347u,1);
 sr_stroke_rect(c,(float)(x-15),(float)(y-22),30,44,2,near?0xe8d6a0u:0x9a8659u,1);
 sr_fill_rect(c,(float)(x-3),(float)(y-11),6,26,0x0b191du,1);
 sr_fill_circle(c,(float)x,(float)(y+10),6,0xb5a16bu,1);
 sr_fill_rect(c,(float)(x-2),(float)(y-8),4,18,0xe0ce91u,1);
 sr_fill_rect(c,(float)(x-10),(float)(y-13),20,8,near?0xa6ffe1u:0x56c5b6u,1);
 sr_fill_circle(c,(float)(x+9),(float)(y+15),2,0x83f6ceu,1);
}
static void desktop_hatch(sr_canvas *c,int x,int y,bool selected,const char *label){
 sr_fill_ellipse(c,(float)x,(float)(y+10),42,18,0x061419u,.6f);
 sr_fill_rect(c,(float)(x-36),(float)(y-25),72,43,0x132228u,1);
 sr_stroke_rect(c,(float)(x-36),(float)(y-25),72,43,3,selected?0x99edcbu:0x81734fu,1);
 for(int i=0;i<4;i++)sr_fill_rect(c,(float)(x-29),(float)(y-19+i*9),58,5,0x53605cu,1);
 sr_fill_rect(c,(float)(x+19),(float)(y-9),10,5,0xe1c67fu,1);
 if(selected)desktop_label(c,x,y+24,label,0xa8f3d5u);
}
static void desktop_room(const zell_scene *s,sr_canvas *c,int cx,int cy){
 sr_clear(c,s->room==1?0x0b1822u:0x192323u);
 int left=(int)(ZELL_WORLD_W*.11f)-cx,top=(int)(ZELL_WORLD_H*.27f)-cy;
 sr_fill_rect(c,(float)left,(float)top,ZELL_WORLD_W*.78f,ZELL_WORLD_H*.68f,0x22343bu,1);
 for(int y=0;y<ZELL_WORLD_H;y+=48)for(int x=0;x<ZELL_WORLD_W;x+=64){
  int xx=x-cx,yy=y-cy;
  sr_stroke_rect(c,(float)xx,(float)yy,64,48,1,s->room==1?0x243c48u:0x354744u,.7f);
 }
 sr_fill_rect(c,(float)left,(float)(top-90),ZELL_WORLD_W*.78f,90,0x101f2bu,1);
 sr_fill_rect(c,(float)left,(float)(top-4),ZELL_WORLD_W*.78f,4,0x68b8b4u,.7f);
 desktop_label(c,ZELL_WORLD_W/2-cx,top-55,s->room==1?"COMMAND CENTER / FIVE CONNECTED SESSIONS":"LAUNDRY / MOUNTED DRIVES",0xe1c78fu);
 int near=zell_desktop_near(s);
 for(int i=15;i<ZELL_STATIONS;i++){
  if(!zell_desktop_station_visible(s,i))continue;
  int x=(int)(zell_stations[i].x*ZELL_WORLD_W)-cx,y=(int)(zell_stations[i].y*ZELL_WORLD_H)-cy;
  if(i==28){desktop_hatch(c,x,y,i==near,"Up to the cave");continue;}
  if(i==29||i==30){
   zell_actor_pose staff=zell_desktop_pose(s,i==29?0:2);
   desktop_art(c,staff.art,x+staff.offset_x,y+staff.offset_y,112);
   desktop_label(c,x,y+15,zell_stations[i].name,0xe9d49fu);
   if(i==near)desktop_label(c,x,y+40,i==29?"Enter: projects and Git":"Enter: coding fleets",0xa8f3d5u);
   continue;
  }
  if(s->room==1){
   sr_fill_rect(c,(float)(x-82),(float)(y-95),164,98,0x030b11u,1);
   sr_stroke_rect(c,(float)(x-82),(float)(y-95),164,98,3,i==near?0xa2f5d5u:0x426878u,1);
   sr_fill_rect(c,(float)(x-74),(float)(y-86),148,74,i==15?0x163b41u:0x102a38u,1);
   char name[32];snprintf(name,sizeof name,i==15?"CONTROLLER":"WORKER %d",i-15);
   sr_text(c,(float)(x-65),(float)(y-78),name,0xb5e9d7u,1,1);
   sr_text(c,(float)(x-65),(float)(y-52),"GPT-6 Astra",0x849faeu,1,1);
   sr_text(c,(float)(x-65),(float)(y-29),">_ Kilix tab",0xdbc787u,1,1);
   sr_fill_rect(c,(float)(x-8),(float)(y+4),16,19,0x47616au,1);
   sr_fill_rect(c,(float)(x-44),(float)(y+22),88,8,0x54717bu,1);
   /* Rack panels below the monitors, with quiet activity lamps. */
   sr_fill_rect(c,(float)(x-74),(float)(y+42),148,55,0x0c1922u,1);
   for(int slot=0;slot<3;slot++){
    sr_stroke_rect(c,(float)(x-68),(float)(y+48+slot*14),136,11,1,0x36525eu,1);
    sr_fill_circle(c,(float)(x+57),(float)(y+53+slot*14),2,0x72d2b0u,.8f);
   }
  }else{
   int bin=i-20,percent=s->disk_used[bin];
   uint32_t color=percent<0?0x7b8990u:percent>=90?0xe18c70u:percent>=75?0xd6b867u:0x6cc9b7u;
   sr_fill_ellipse(c,(float)x,(float)(y+7),63,14,0x071518u,.6f);
   sr_fill_rect(c,(float)(x-58),(float)(y-88),116,88,0x182b32u,1);
   int fill=percent<0?0:72*percent/100;
   sr_fill_rect(c,(float)(x-50),(float)(y-8-fill),100,(float)fill,color,.65f);
   for(int rib=0;rib<5;rib++)sr_fill_rect(c,(float)(x-49+rib*24),(float)(y-80),3,72,0x74918du,.45f);
   sr_stroke_rect(c,(float)(x-58),(float)(y-88),116,88,3,i==near?0xb5efd1u:0x57716fu,1);
   sr_fill_ellipse(c,(float)x,(float)(y-88),58,12,0x6b8d88u,1);
   sr_fill_ellipse(c,(float)x,(float)(y-89),49,6,0x0e2026u,1);
   char value[24];if(percent<0)snprintf(value,sizeof value,"Unknown");else snprintf(value,sizeof value,"%d%% full",percent);
   desktop_label(c,x,y-45,value,0xe0eee3u);
   char label[22];snprintf(label,sizeof label,"%.21s",s->disk_labels[bin]);desktop_label(c,x,y+16,label,color);
  }
  if(i==near)desktop_label(c,x,y+38,s->room==1?"Enter: open session":"Enter: dig through drive",0xa8f3d5u);
 }
 if(s->room==2&&s->disk_count==0)desktop_label(c,ZELL_WORLD_W/2-cx,ZELL_WORLD_H/2-cy,s->disks_ready?"No eligible mounted disks found":"Reading mounted drives...",0xc9d8d2u);
}
static void desktop_vitals(const zell_scene *s,sr_canvas *c){
 int span=c->w-24;if(span>420)span=420;int width=(span-16)/3;
 if(width<32)return;
 int values[3]={s->hp,s->mana,s->fatigue};const char *names[3]={"HEALTH","MANA","FATIGUE"};
 uint32_t colors[3]={0xde8a79u,0x8ea6efu,0x87c5a4u};
 for(int i=0;i<3;i++){
  int x=12+i*(width+8);char label[40];
  if(values[i]<0)snprintf(label,sizeof label,"%s ?",names[i]);else snprintf(label,sizeof label,"%s %d%s",width<110?(i==0?"HP":i==1?"MP":"FT"):names[i],values[i],i==1&&s->mana_age>=3600?"*":"%");
  sr_fill_rect(c,(float)x,39,(float)width,38,0x0c1921u,.95f);
  sr_text(c,(float)(x+4),42,label,colors[i],1,1);
  sr_fill_rect(c,(float)(x+4),64,(float)(width-8),7,0x263940u,1);
  if(values[i]>0)sr_fill_rect(c,(float)(x+4),64,(float)(width-8)*(float)values[i]/100,7,colors[i],1);
 }
 if(s->hp>=0&&s->hp<100&&s->room==0&&s->mode==ZELL_MODE_WALK)sr_text(c,12,83,s->dirty_files>0?"Health low: hover bar for details":"Admin: OS update due",0xe5ad87u,1,1);
 if(s->mouse_valid&&s->mouse_x>=12&&s->mouse_x<12+width&&s->mouse_y>=39&&s->mouse_y<77){
  char tip[600];
  char os[240],git[200];
  if(s->update_age_days<0)snprintf(os,sizeof os,"OS upgrade history is unknown; no age penalty is assumed.");
  else if(s->os_health>=100)snprintf(os,sizeof os,"OS health is full: upgraded less than 24 hours ago.");
  else snprintf(os,sizeof os,"OS upgrade was %d full days ago: OS health %d%%. Age costs 10 points/day from day two, floor 10%%.",s->update_age_days,s->os_health);
  if(s->dirty_files>0)snprintf(git,sizeof git,"%d dirty files in watched Git projects: minus %d points (combined floor 1%%). Visit Git Wizard downstairs.",s->dirty_files,s->dirty_files);
  else if(s->dirty_files<0)snprintf(git,sizeof git,"Some watched repositories are unavailable. Open Git Wizard for errors and known penalties.");
  else snprintf(git,sizeof git,"No known dirty-file penalty. Register watches with Git Wizard in Command Center.");
  snprintf(tip,sizeof tip,"%s %s Administrator > Update OS restores OS health. Movement speed: %.0f%%.",os,git,100*zell_desktop_speed_factor(s));
  int tw=c->w-32;if(tw>430)tw=430;int lines=wrap_count(tip,tw-24),height=lines*18+24;
  sr_fill_rect(c,12,105,(float)tw,(float)height,0x10232cu,.98f);sr_stroke_rect(c,12,105,(float)tw,(float)height,1,0xd6ba87u,1);
  draw_wrapped(c,24,117,tw-24,tip,0xf0e5cdu,0,lines);
 }
}
static void desktop_base(const zell_scene *s,sr_canvas *c){
 static sr_canvas background; const zell_art *a=zell_art_get(0);
 if(!background.px){if(sr_canvas_init(&background,ZELL_WORLD_W,ZELL_WORLD_H)&&a){sr_canvas src;sr_canvas_wrap(&src,a->pixels,a->width,a->height);sr_blit_scaled(&background,&src,0,0,ZELL_WORLD_W,ZELL_WORLD_H,1);}}
 int cx,cy;zell_desktop_camera(s,&cx,&cy);sr_clear(c,0x081319u);
 if(background.px){for(int y=0;y<c->h&&y+cy<ZELL_WORLD_H;y++){int w=c->w;if(w+cx>ZELL_WORLD_W)w=ZELL_WORLD_W-cx;memcpy(c->px+(size_t)y*(size_t)c->w,background.px+(size_t)(y+cy)*ZELL_WORLD_W+(size_t)cx,(size_t)w*4);}}
 if(s->room)desktop_room(s,c,cx,cy);
 int near=zell_desktop_near(s);int height=112;
 for(int i=0;i<ZELL_CAVE_STATIONS&&s->room==0;i++){
  const zell_station *p=&zell_stations[i];int x=(int)(p->x*(float)ZELL_WORLD_W),y=(int)(p->y*(float)ZELL_WORLD_H);
  x-=cx;y-=cy;
  int h=i<6?height:height*3/4;
  if(i==near){sr_fill_ellipse(c,(float)x,(float)y, (float)h*.42f,6,0x80e5c4u,.65f);}
  if(i<6){zell_actor_pose pose=zell_desktop_pose(s,i);sr_fill_ellipse(c,(float)x,(float)y,23,7,0x071719u,.45f);desktop_art(c,pose.art,x+pose.offset_x,y+pose.offset_y,h);}
  if(i==13||i==14){desktop_hatch(c,x,y,i==near,i==13?"Command center":"Laundry room");continue;}
  if(i==12){desktop_shell_switch(c,x,y,i==near);if(i!=near)desktop_label(c,x,y+28,"N-SHELL",0x91e9d8u);}
  if(i==6||i==7)desktop_label(c,x,y+8,i==6?"TUI":"SHELL",0x91e9d8u);
  if(i==near)desktop_label(c,x,y+(i==12?28:8),p->name,0xb9f8dbu);
 }
 if(s->mode==ZELL_MODE_WALK&&(s->running||s->run_charge>0)){
  float px=s->x-(float)cx,py=s->y-(float)cy;
  if(s->running){
   float dx=s->direction==2?-1.0f:s->direction==3?1.0f:0;
   float dy=s->direction==1?-1.0f:s->direction==0?1.0f:0;
   for(int dust=0;dust<3;dust++){
    float age=(float)((s->flame_tick+4*dust)%12),back=8+age*2;
    sr_fill_ellipse(c,px-dx*back+(dy!=0?(float)(dust-1)*5:0),py-dy*back-2,2+age*.3f,1+age*.15f,0xd6c5a2u,(12-age)/24);
   }
  }else{
   sr_fill_rect(c,px-19,py+8,38,4,0x11282cu,.9f);
   sr_fill_rect(c,px-18,py+9,36*(float)s->run_charge/ZELL_RUN_CHARGE_TICKS,2,0xa2e7c3u,1);
  }
 }
 zell_actor_pose pose=zell_desktop_pose(s,-1);sr_fill_ellipse(c,s->x-(float)cx,s->y-(float)cy,17,5,0x071719u,.45f);desktop_art(c,pose.art,(int)s->x-cx+pose.offset_x,(int)s->y-cy+pose.offset_y,height*3/4);
 if(c->w>=500&&s->mode==ZELL_MODE_WALK){
  int mx=c->w-140,my=42;sr_fill_rect(c,(float)mx,(float)my,128,88,0x0c1921u,.9f);
  sr_stroke_rect(c,(float)mx,(float)my,128,88,1,0x4b7777u,1);
  for(int i=0;i<ZELL_STATIONS;i++)if(zell_desktop_station_visible(s,i))sr_fill_circle(c,(float)(mx+4)+zell_stations[i].x*120,(float)(my+4)+zell_stations[i].y*80,2,0xd9ba79u,1);
  sr_stroke_rect(c,(float)(mx+4)+((float)cx/ZELL_WORLD_W)*120,(float)(my+4)+((float)cy/ZELL_WORLD_H)*80,(float)c->w/ZELL_WORLD_W*120,(float)c->h/ZELL_WORLD_H*80,1,0x407b85u,1);
  sr_fill_circle(c,(float)(mx+4)+s->x/ZELL_WORLD_W*120,(float)(my+4)+s->y/ZELL_WORLD_H*80,3,0x8af8d4u,1);
 }
 sr_fill_rect(c,0,0,(float)c->w,30,0x0c1921u,.97f);
 sr_text(c,12,7,s->room==1?"ZELL / COMMAND":s->room==2?"ZELL / LAUNDRY":"ZELL / 0.1.0",0xf0d69bu,1,1);
 char status[100];time_t now=time(NULL);struct tm local;localtime_r(&now,&local);
 snprintf(status,sizeof status,"%02d:%02d  %d fps",local.tm_hour,local.tm_min,s->frame_rate);
 int sw=sr_text_width_in(SR_FONT_FIXED_8X16,status,1);sr_text(c,(float)(c->w-sw-12),7,status,0xa6d9ccu,1,1);
 sr_fill_rect(c,0,(float)(c->h-30),(float)c->w,30,0x0c1921u,.97f);
 const char *hint=s->mode==ZELL_MODE_WALK&&s->running?"Dashing | Release Shift to stop":s->mode==ZELL_MODE_WALK&&s->run_charge>0?"Charging dash... | Release Shift to cancel":s->mode==ZELL_MODE_SERVICE?"Station window | Esc back":near>=0?"Enter interact | Tab stations":c->w<500?"WASD | Shift run | Tab":"WASD walk | Hold Shift run | Tab stations";
 sr_text(c,12,(float)(c->h-22),hint,0xc7d8dau,1,1);
 desktop_vitals(s,c);
}
static void desktop_text(sr_canvas *c,int x,int y,const char *text,uint32_t color,int width){
 char line[160];int cap=width/8;if(cap<1)return;if(cap>159)cap=159;
 size_t n=strlen(text);if(n>(size_t)cap){memcpy(line,text,(size_t)cap);if(cap>3)memcpy(line+cap-3,"...",3);line[cap]=0;}else{memcpy(line,text,n+1);}
 sr_text(c,(float)x,(float)y,line,color,1,1);
}
static void desktop_panel(const zell_scene *s,sr_canvas *c){
 int launch=s->mode==ZELL_MODE_LAUNCHER;
 int count=launch?zell_desktop_launcher_count(s):zell_desktop_actions(s->station);
 int width=c->w-48;if(width>640)width=640;int x=(c->w-width)/2;
 int rows=(c->h-150)/26;if(rows<1)rows=1;if(rows>count)rows=count;
 int first=s->menu_index>=rows?s->menu_index-rows+1:0;
 int height=88+rows*26;int y=(c->h-height)/2;
 sr_fill_rect(c,0,30,(float)c->w,(float)(c->h-60),0x061017u,.68f);
 sr_fill_rect(c,(float)x,(float)y,(float)width,(float)height,0x10232cu,.98f);
 sr_stroke_rect(c,(float)x,(float)y,(float)width,(float)height,1,0x5b9989u,1);
 desktop_text(c,x+18,y+14,launch?"THE CAVE / QUICK LAUNCH":zell_stations[s->station].name,0xf0d69bu,width-36);
 char warning[120];snprintf(warning,sizeof warning,"OS upgrade overdue: health %d%%. Choose Update OS.",s->hp);
 desktop_text(c,x+18,y+36,(!launch&&s->station==2&&s->hp>=0&&s->hp<100)?warning:launch?"Every station, without the walk":zell_stations[s->station].role,0x89aaa9u,width-36);
 for(int r=0;r<rows;r++){int i=r+first,yy=y+56+r*26;bool selected=i==s->menu_index;
 if(selected)sr_fill_rect(c,(float)(x+10),(float)(yy-4),(float)(width-20),27,0x254b50u,1);
 desktop_text(c,x+20,yy,launch?zell_stations[zell_desktop_launcher_station(s,i)].name:zell_desktop_action(s->station,i),selected?0xe8ffe9u:0xb5c9cdu,width-40);
 }
 desktop_text(c,x+18,y+height-25,width<400?"Enter open | Esc back":"Up/Down choose  Enter open  Esc back",0x89aaa9u,width-36);
}

static void desktop_dialogue(const zell_scene *s,sr_canvas *c) {
 int left=20,top=44,width=c->w-40;
 sr_fill_rect(c,8,36,(float)(c->w-16),(float)(c->h-72),0x10232cu,.98f);
 if(s->mode==ZELL_MODE_ANSWER){
  int y,rows,span;answer_layout(c->w,c->h-42,&y,&rows,&span);y=72;
  desktop_text(c,left,top,s->answerer,0xf0d69bu,width);
  draw_wrapped(c,left,y,span,s->answer,0xe3efe8u,s->answer_scroll,rows);
  desktop_text(c,left,c->h-54,"Up/Down scroll | Enter close",0x89aaa9u,width);
  return;
 }
 if(s->mode==ZELL_MODE_WAIT){
  desktop_text(c,left,top,"The wizard is thinking...",0xf0d69bu,width);
  desktop_text(c,left,top+26,s->answerer,0x9edecbu,width);
  desktop_text(c,left,c->h-54,"Esc cancels the question",0x89aaa9u,width);
  return;
 }
 desktop_text(c,left,top,"WIZARD / ASK",0xf0d69bu,width);
 int visible=(width-16)/8;if(visible<1)visible=1;
 int offset=s->question_len>visible?s->question_len-visible:0;
 input_field(c,left,top+24,width,s->question+offset,s->question_len-offset);
 if(s->mode==ZELL_MODE_QUESTION){
  int count=zell_scene_choice_rows(s)+1;
  int rows=(c->h-154)/22;if(rows<1)rows=1;if(rows>count)rows=count;
  int first=s->menu_index>=rows?s->menu_index-rows+1:0;
  for(int r=0;r<rows;r++){
   int i=first+r,y=top+56+r*22;
   if(i==s->menu_index)sr_fill_rect(c,(float)(left-4),(float)(y-2),(float)(width+8),21,0x254b50u,1);
   const char *label=i==count-1?(s->history_on?"History [ON] - toggle":"History [OFF] - toggle"):zell_scene_choice(s,i);
   desktop_text(c,left,y,label,i==s->menu_index?0xf5dda7u:0xc7d8dau,width);
  }
 }
 desktop_text(c,left,c->h-54,"Enter ask | Esc back",0x89aaa9u,width);
}

/* The scene keeps animating beneath every station conversation. */
static void desktop_service(const zell_scene *s,sr_canvas *c){
 if(s->service_phase==5){
  int w=s->terminal_width,h=s->terminal_height,x=(c->w-w)/2,y=(c->h-h)/2;
  sr_fill_rect(c,0,30,(float)c->w,(float)(c->h-60),0x041017u,.35f);
  if(s->terminal_pixels&&w>0&&h>0){
   sr_fill_rect(c,(float)(x-8),(float)(y-30),(float)(w+16),(float)(h+58),0x10232cu,1);
   sr_stroke_rect(c,(float)(x-8),(float)(y-30),(float)(w+16),(float)(h+58),2,0x74b8a9u,1);
   sr_canvas frame;sr_canvas_wrap(&frame,s->terminal_pixels,w,h);sr_blit_scaled(c,&frame,x,y,w,h,1);
   desktop_text(c,x,y-23,"ADMIN / INTERACTIVE NEURONTE TERMINAL",0xe5cf98u,w);
   desktop_text(c,x,y+h+7,"Type here for sudo | Ctrl-C interrupts command",0xabc8c0u,w);
  }else desktop_label(c,c->w/2,c->h/2,"Opening Neuronte terminal...",0xc9e4d5u);
  return;
 }

 if(c->w<240||c->h<220){
  sr_fill_rect(c,0,30,(float)c->w,(float)(c->h-60),0x10232cu,1);
  desktop_text(c,8,40,"Enlarge the pane",0xf0d69bu,c->w-16);
  desktop_text(c,8,62,"for station windows",0xc7d8dau,c->w-16);
  return;
 }
 int width=c->w-32;if(width>900)width=900;if(width<64)width=64;
 int x=(c->w-width)/2,top=44,bottom=c->h-44;
 sr_fill_rect(c,0,30,(float)c->w,(float)(c->h-60),0x061017u,.58f);
 sr_fill_rect(c,(float)x,36,(float)width,(float)(c->h-72),0x10232cu,.98f);
 sr_stroke_rect(c,(float)x,36,(float)width,(float)(c->h-72),1,0x5b9989u,1);
 desktop_text(c,x+12,top,zell_stations[s->station].name,0xf0d69bu,width-24);
 if(s->service_phase==2){
  desktop_text(c,x+12,top+24,s->service_title,0x9edecbu,width-24);
  int rows=(bottom-top-74)/24;if(rows<1)rows=1;if(rows>s->service_count)rows=s->service_count;
  int first=s->service_index>=rows?s->service_index-rows+1:0;
  for(int r=0;r<rows;r++){
   int index=first+r,y=top+50+r*24;bool selected=index==s->service_index;
   if(selected)sr_fill_rect(c,(float)(x+6),(float)(y-3),(float)(width-12),24,0x254b50u,1);
   desktop_text(c,x+14,y,s->service_choices[index],selected?0xe8ffe9u:0xb5c9cdu,width-28);
  }
 }else{
  int reserved=s->service_phase==1?100:32;
  int rows=(bottom-top-28-reserved)/18;if(rows<1)rows=1;
  draw_wrapped(c,x+12,top+28,width-24,s->service_text,0xe3efe8u,s->service_scroll,rows);
  if(s->service_phase==1){
   desktop_text(c,x+12,bottom-80,s->service_title,0x9edecbu,width-24);
   int visible=(width-32)/8;if(visible<1)visible=1;
   int length=(int)strlen(s->service_input),offset=length>visible?length-visible:0;
   input_field(c,x+12,bottom-54,width-24,s->service_input+offset,length-offset);
  }
 }
 const char *hint=s->service_phase==0?"Working... | Esc cancel":s->service_phase==2?"Up/Down choose | Enter select | Esc back":s->service_phase==1?"Enter submit | Up/Down scroll | Esc back":s->service_phase==4?"Up/Down scroll | Enter return to station":"Up/Down scroll | Enter continue | Esc back";
 desktop_text(c,x+12,bottom-18,hint,0x89aaa9u,width-24);
}
