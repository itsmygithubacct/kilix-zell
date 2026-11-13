/* Included after the Neuronte drawing adapters, so artwork and text use them. */
#include "desktop.h"
#include "desktop_assets.h"
#include <time.h>
#include <stdlib.h>
#include <math.h>
static void desktop_art(sr_canvas *c,int id,int x,int foot,int height){
 const zell_art *a=zell_art_get(id);if(!a)return;sr_canvas src;sr_canvas_wrap(&src,a->pixels,a->width,a->height);
 int width=height*a->width/a->height;sr_blit_scaled(c,&src,x-width/2,foot-height,width,height,1.0f);
}
/* Draw one cutout mirrored left to right. The mirrored copy is built once. */
static void desktop_art_mirrored(sr_canvas *c,int id,int x,int foot,int height){
 static uint32_t *pixels;static int cached=-1;
 const zell_art *a=zell_art_get(id);if(!a)return;
 if(cached!=id){
  uint32_t *copy=realloc(pixels,(size_t)a->width*(size_t)a->height*sizeof *copy);if(!copy)return;
  pixels=copy;cached=id;
  for(int row=0;row<a->height;row++)for(int col=0;col<a->width;col++)
   pixels[row*a->width+col]=a->pixels[row*a->width+a->width-1-col];
 }
 sr_canvas src;sr_canvas_wrap(&src,pixels,a->width,a->height);
 int width=height*a->width/a->height;sr_blit_scaled(c,&src,x-width/2,foot-height,width,height,1.0f);
}
/* The stool sheet is the full room. Only its front rim and legs are opaque. */
static void desktop_stool_front(sr_canvas *c,int cx,int cy){
 const zell_art *a=zell_art_get(ZELL_ART_WORKSHOP_STOOL);if(!a)return;
 sr_canvas src;sr_canvas_wrap(&src,a->pixels,a->width,a->height);
 sr_blit_scaled(c,&src,-cx,-cy,a->width,a->height,1.f);
}
/* Full-canvas foreground. The asset compiler keeps the 1536x1024 anchor. */
static void desktop_registered_front(sr_canvas *c,int art,int cx,int cy){
 const zell_art *sheet=zell_art_get(art);if(!sheet)return;
 sr_canvas src;sr_canvas_wrap(&src,sheet->pixels,sheet->width,sheet->height);
 sr_blit_scaled(c,&src,-cx,-cy,sheet->width,sheet->height,1.f);
}
static int desktop_room_background(int room){
 if(room==1)return ZELL_ART_COMMAND_ROOM;
 if(room==2)return ZELL_ART_LAUNDRY_ROOM;
 if(room==3)return ZELL_ART_VAULT_ROOM;
 if(room==4)return ZELL_ART_WORKSHOP_ROOM;
 if(room==5)return ZELL_ART_GAME_ROOM;
 if(room==6)return ZELL_ART_THEATER_ROOM;
 if(room==7)return ZELL_ART_MEDITATION_ROOM;
 if(room==8)return ZELL_ART_LIBRARY_ROOM;
 return 0;
}
static int desktop_room_foreground(int room){
 if(room==5)return ZELL_ART_GAME_FOREGROUND;
 if(room==6)return ZELL_ART_THEATER_FOREGROUND;
 if(room==7)return ZELL_ART_MEDITATION_FOREGROUND;
 if(room==8)return ZELL_ART_LIBRARY_FOREGROUND;
 return -1;
}
static const char *desktop_room_title(int room){
 if(room==1)return "ZELL / COMMAND";
 if(room==2)return "ZELL / LAUNDRY";
 if(room==3)return "ZELL / VAULT";
 if(room==4)return "ZELL / WORKSHOP";
 if(room==5)return "ZELL / GAME";
 if(room==6)return "ZELL / THEATER";
 if(room==7)return "ZELL / MEDITATION";
 if(room==8)return "ZELL / LIBRARY";
 return "ZELL / " ZELL_VERSION;
}
static void desktop_label(sr_canvas *c,int x,int y,const char *text,uint32_t color){
 int w=sr_text_width_in(SR_FONT_FIXED_8X16,text,1);
 /* Keep labels for wall objects inside the view. */
 if(w+16<=c->w){if(x>c->w-w/2-8)x=c->w-w/2-8;if(x<w/2+8)x=w/2+8;}
 sr_fill_rect(c,(float)(x-w/2-6),(float)(y-3),(float)(w+12),22,0x101f26u,.92f);
 sr_text(c,(float)(x-w/2),(float)y,text,color,1,1);
}
static void desktop_text(sr_canvas *c,int x,int y,const char *text,uint32_t color,int width);
static void desktop_hatch(sr_canvas *c,int art,int x,int y,bool selected,const char *label){
 /* Each cutout is compiled at the height it is drawn at. */
 const zell_art *cut=zell_art_get(art);int height=cut?cut->height:76;
 if(selected)sr_fill_ellipse(c,(float)x,(float)(y+2),54,12,0x80e5c4u,.55f);
 desktop_art(c,art,x,y+14,height);
 if(selected)desktop_label(c,x,y+28,label,0xa8f3d5u);
}
static void desktop_monitor(const zell_scene *s,sr_canvas *c,int index,int cx,int cy,bool selected){
 zell_rect frame,display;zell_monitor_layout(index,&frame,&display);
 frame.x-=cx;frame.y-=cy;display.x-=cx;display.y-=cy;
 const zell_art *a=zell_art_get(index==0?ZELL_ART_CONTROLLER:ZELL_ART_WORKER);
 if(a){sr_canvas src;sr_canvas_wrap(&src,a->pixels,a->width,a->height);sr_blit_scaled(c,&src,frame.x,frame.y,frame.w,frame.h,1);}
 sr_fill_rect(c,(float)display.x,(float)display.y,(float)display.w,(float)display.h,0x091820u,1);
 if(selected)sr_stroke_rect(c,(float)display.x,(float)display.y,(float)display.w,(float)display.h,2,0x96ead0u,1);
 char title[32];snprintf(title,sizeof title,index==0?"CONTROLLER":"WORKER %d",index);
 desktop_text(c,display.x+8,display.y+7,title,0xe9d49fu,display.w-16);
 desktop_text(c,display.x+8,display.y+27,s->screens_ready?s->screen_status[index]:"Reading sessions...",0x8fd5c6u,display.w-16);
 const char *body=s->screen_preview[index];
 if(!*body)body=s->screens_ready?"Click to open this screen":"Session previews refresh here";
 int lines=(display.h-59)/17;
 for(int row=0;row<lines&&*body;row++){
  const char *end=strchr(body,'\n');size_t n=end?(size_t)(end-body):strlen(body);
  char line[200];if(n>=sizeof line)n=sizeof line-1;memcpy(line,body,n);line[n]=0;
  desktop_text(c,display.x+8,display.y+49+row*17,line,0xc9dcd8u,display.w-16);
  if(!end)break;
  body=end+1;
 }
 if(selected)desktop_label(c,frame.x+frame.w/2,frame.y+frame.h+8,"Enter / click: session & tasks",0xa8f3d5u);
}
/* Copy one rectangle out of a cell, then scale that copy. The cell stays put. */
static void floor_blit(sr_canvas *c,const zell_art *art,int sx,int sy,int sw,int sh,int dx,int dy,int dw,int dh){
 static uint32_t *pixels;
 static size_t capacity;
 if(!art||sw<1||sh<1||dw<1||dh<1)return;
 if(sx<0||sy<0||sw>art->width-sx||sh>art->height-sy)return;
 size_t count=(size_t)sw*(size_t)sh;
 if(capacity<count){
  uint32_t *grown=realloc(pixels,count*sizeof *grown);
  if(!grown)return;
  pixels=grown;
  capacity=count;
 }
 for(int row=0;row<sh;row++)
  memcpy(pixels+(size_t)row*(size_t)sw,art->pixels+(size_t)(sy+row)*(size_t)art->width+(size_t)sx,(size_t)sw*sizeof *pixels);
 sr_canvas src;sr_canvas_wrap(&src,pixels,sw,sh);
 sr_blit_scaled(c,&src,dx,dy,dw,dh,1.0f);
}
/* World placement. desktop_present_room applies the one screen transform. */
static void panel_view(const zell_scene *s,const zell_rect *world,int cx,int cy,zell_rect *view){
 (void)s;
 *view=(zell_rect){world->x-cx,world->y-cy,world->w,world->h};
}
/* Three plates in the flagstones. The off cell supplies the bronze rim for
 * every state, so a confirmed read only recolors the glass. */
static void desktop_floor_panels(const zell_scene *s,sr_canvas *c,int cx,int cy){
 if(s->room!=1)return;
 const zell_art *plate=zell_art_get(ZELL_ART_FLOOR_OFF);
 for(int index=0;index<3;index++){
  zell_rect frame,glass,view,pane;
  zell_floor_panel_layout(index,&frame,&glass);
  panel_view(s,&frame,cx,cy,&view);
  panel_view(s,&glass,cx,cy,&pane);
  if(view.w<1||view.h<1||pane.w<1||pane.h<1)continue;
  floor_blit(c,plate,42,217,332,153,view.x,view.y,view.w,view.h);
  uint32_t fill=0x121816u;
  const char *title="SPARE",*detail="INACTIVE";
  char count[32];
  int icon=ZELL_ART_FLOOR_SPARE;
  zell_rect crop=(zell_rect){50,102,251,272};
  bool bar=false;
  uint32_t detail_ink=0xf4f1e8u;
  if(index==0){
   title="YOLO";
   icon=ZELL_ART_FLOOR_YOLO_ICON;
   crop=(zell_rect){78,87,244,299};
   /* A stale coding_yolo value stays a reading until Y confirms it. */
   if(!s->coding_yolo_ready){detail="READING";fill=0x2c3834u;}
   else if(s->coding_yolo){detail="ENABLED";fill=0xd06a22u;}
   else {detail="DISABLED";fill=0x1a8f86u;}
  }else if(index==1){
   title="AGENTS";
   fill=0x163430u;
   icon=ZELL_ART_FLOOR_AGENTS;
   crop=(zell_rect){48,99,322,273};
   /* Progress comes from the U frame. A client's age is not an update offer. */
   if(s->update_job_active&&strcmp(s->update_state,"running")==0){
    snprintf(count,sizeof count,"%d/%d",s->update_done,s->update_total);
    detail=count;detail_ink=0xff4ad2u;bar=s->update_total>0;
   }else if(s->update_job_active&&strcmp(s->update_state,"done")==0)detail="DONE";
   else if(s->update_job_active&&strcmp(s->update_state,"failed")==0)detail="FAILED";
   else if(s->update_job_active&&strcmp(s->update_state,"cancelled")==0)detail="CANCELLED";
   else detail="IDLE";
  }
  sr_canvas_set_clip(c,pane.x,pane.y,pane.w,pane.h);
  sr_fill_rect(c,(float)pane.x,(float)pane.y,(float)pane.w,(float)pane.h,fill,1.0f);
  int ih=pane.h-16;if(ih>34)ih=34;if(ih<8)ih=8;
  int iw=crop.h>0?crop.w*ih/crop.h:ih;
  if(iw>26){iw=26;ih=crop.w>0?crop.h*iw/crop.w:ih;}
  if(iw>pane.w-8)iw=pane.w-8;
  if(ih>pane.h-8)ih=pane.h-8;
  if(iw>4&&ih>4)floor_blit(c,zell_art_get(icon),crop.x,crop.y,crop.w,crop.h,pane.x+4,pane.y+(pane.h-ih)/2,iw,ih);
  int text_x=pane.x+34,text_w=pane.x+pane.w-6-text_x;
  if(text_w>8){
   desktop_text(c,text_x,pane.y+2,title,0xe9d49fu,text_w);
   desktop_text(c,text_x,pane.y+20,detail,detail_ink,text_w);
  }
  if(bar){
   int bw=pane.w-8,bx=pane.x+4,by=pane.y+pane.h-8;
   if(bw>8&&by>=pane.y){
    sr_fill_rect(c,(float)bx,(float)by,(float)bw,5.0f,0x10241fu,1.0f);
    int filled=bw*s->update_done/s->update_total;
    if(filled>bw)filled=bw;
    if(filled>0)sr_fill_rect(c,(float)bx,(float)by,(float)filled,5.0f,0x7dcea0u,1.0f);
   }
  }
  sr_canvas_reset_clip(c);
 }
}
/* Drawn twice: once for what stands behind Kilix, once for what stands in front. */
/* L3 calls zell_command_layout. */
/* One row per provider: name, a bar of what is left, and the number. */
static void desktop_mana_rows(const zell_scene *s,sr_canvas *c,int x,int y,int w,int row,int scale){
 static const char *const names[3]={"CODEX","CLAUDE","GROK"};
 for(int i=0;i<3;i++){
  int value=s->provider_mana_ready?s->provider_mana[i]:-1,yy=y+i*row;
  uint32_t ink=value==-2?0xf0b35eu:value<0?0x7b8990u:value<10?0xe18c70u:value<30?0xd6b867u:0x6cc9b7u;
  char text[24];if(value==-2)snprintf(text,sizeof text,"ASK");else if(value<0)snprintf(text,sizeof text,"?");else snprintf(text,sizeof text,"%d%%",value);
  int name_w=56*scale,value_w=40*scale,bar=w-name_w-value_w-8;
  sr_text(c,(float)x,(float)yy,names[i],0xc9dcd8u,(float)scale,1);
  if(bar>8){
   sr_fill_rect(c,(float)(x+name_w),(float)(yy+3*scale),(float)bar,(float)(9*scale),0x13262eu,1);
   if(value>0)sr_fill_rect(c,(float)(x+name_w),(float)(yy+3*scale),(float)(bar*value)/100,(float)(9*scale),ink,1);
  }
  sr_text(c,(float)(x+w-value_w+4),(float)yy,text,ink,(float)scale,1);
 }
}
static void desktop_mana_console(const zell_scene *s,sr_canvas *c,int cx,int cy,bool selected){
 zell_rect frame,display;zell_mana_console_layout(&frame,&display);
 frame.x-=cx;frame.y-=cy;display.x-=cx;display.y-=cy;
 const zell_art *a=zell_art_get(ZELL_ART_WORKER);
 if(a){sr_canvas src;sr_canvas_wrap(&src,a->pixels,a->width,a->height);sr_blit_scaled(c,&src,frame.x,frame.y,frame.w,frame.h,1);}
 sr_fill_rect(c,(float)display.x,(float)display.y,(float)display.w,(float)display.h,0x07141au,1);
 if(selected)sr_stroke_rect(c,(float)display.x,(float)display.y,(float)display.w,(float)display.h,2,0x96ead0u,1);
 desktop_text(c,display.x+6,display.y+4,"MANA",0xe9d49fu,display.w-12);
 desktop_mana_rows(s,c,display.x+6,display.y+24,display.w-12,17,1);
 if(selected)desktop_label(c,frame.x+frame.w/2,frame.y+frame.h+8,"Enter: balances & usage pages",0xa8f3d5u);
}
/* The console screen grows to fill the view before its browser tab opens. */
static void desktop_zoom(const zell_scene *s,sr_canvas *c){
 zell_rect frame,display;zell_mana_console_layout(&frame,&display);
 float x0,y0,x1,y1,dw,dh,t,x,y,w,h;
 zell_world_to_screen(s,(float)display.x,(float)display.y,&x0,&y0);
 zell_world_to_screen(s,(float)(display.x+display.w),(float)(display.y+display.h),&x1,&y1);
 dw=x1-x0;dh=y1-y0;
 t=(float)s->zoom_ticks/(float)ZELL_ZOOM_TICKS;if(t>1)t=1;t=t*t*(3-2*t);
 x=x0*(1-t);y=y0*(1-t);
 w=dw+((float)c->w-dw)*t;h=dh+((float)c->h-dh)*t;
 sr_fill_rect(c,x,y,w,h,0x07141au,1);
 sr_stroke_rect(c,x,y,w,h,2,0x96ead0u,1);
 int scale=t>.6f&&w>520?2:1;
 if(w>220&&h>120){
  sr_text(c,x+18,y+14,"MANA CONSOLE",0xe9d49fu,(float)scale,1);
  desktop_mana_rows(s,c,(int)x+18,(int)y+14+26*scale,(int)w-36,22*scale,scale);
  static const char *const pages[4]={"usage dashboard","Codex usage page","Claude usage page","Grok usage page"};
  char line[80];snprintf(line,sizeof line,"> opening the %s in a browser tab...",pages[s->menu_index>=0&&s->menu_index<4?s->menu_index:0]);
  sr_text(c,x+18,y+h-30*(float)scale,line,0x85f7b2u,(float)scale,1);
 }
}
static void desktop_fleet_board(const zell_scene *s,sr_canvas *c,int cx,int cy){
 zell_rect board;zell_fleet_board_layout(&board);
 board.x-=cx;board.y-=cy;
 if(board.x+board.w<0||board.y+board.h<0||board.x>=c->w||board.y>=c->h)return;
 sr_fill_rect(c,(float)board.x,(float)board.y,(float)board.w,(float)board.h,0x101f26u,1);
 sr_stroke_rect(c,(float)board.x,(float)board.y,(float)board.w,(float)board.h,1,0xd6ba87u,1);
 desktop_text(c,board.x+8,board.y+6,"FLEET BOARD",0xe9d49fu,board.w-16);
 if(s->fleet_count<=0){desktop_text(c,board.x+8,board.y+28,"No saved fleets",0x8fd5c6u,board.w-16);return;}
 int rows=s->fleet_count;if(rows>4)rows=4;
 for(int i=0;i<rows;i++){
  char line[160];
  if(!zell_fleet_board_line(s,i,line,sizeof line))continue;
  desktop_text(c,board.x+8,board.y+28+i*18,line,0xc9dcd8u,board.w-16);
 }
}
static void desktop_room(const zell_scene *s,sr_canvas *c,int cx,int cy,bool front){
 int near=zell_desktop_near(s);
 /* Floor pass, before Kilix and before anything that stands over him. */
 if(!front)desktop_floor_panels(s,c,cx,cy);
 for(int i=15;i<ZELL_STATIONS;i++){
  if(!zell_desktop_station_visible(s,i)||zell_desktop_station_in_front(s,i)!=front)continue;
  if(i>=ZELL_WORKSHOP_COMPUTER)continue;
  int x=(int)(zell_stations[i].x*ZELL_WORLD_W)-cx,y=(int)(zell_stations[i].y*ZELL_WORLD_H)-cy;
  if(i==ZELL_MANA_CONSOLE){desktop_mana_console(s,c,cx,cy,i==near);continue;}
  if(i>=46)continue;
  /* Floor panels replace the old lever boxes. Rooms 1 and 2 also paint stairs. */
  if(i==28){if(s->room<3)desktop_hatch(c,s->room==1?ZELL_ART_COMMAND_STAIRS:ZELL_ART_LAUNDRY_STAIRS,x,y,false,"");continue;}
  if(i>=29){
   zell_actor_pose staff=i<32?zell_desktop_pose(s,i-23):(zell_actor_pose){zell_stations[i].art+(s->flame_tick/20+i)%4,0,0};
   sr_fill_ellipse(c,(float)x,(float)y,24,7,0x071719u,.45f);
   desktop_art(c,staff.art,x+staff.offset_x,y+staff.offset_y,128);
   desktop_label(c,x,y+15,zell_stations[i].name,0xe9d49fu);
   if(i==near)desktop_label(c,x,y+40,i==29?"Enter: projects and Git":i==30?"Enter: coding fleets":i==31?"Enter: encrypted secret vault":"Enter: tasks",0xa8f3d5u);
   continue;
  }
  if(s->room==1){desktop_monitor(s,c,i-15,cx,cy,i==near);continue;}
  int bin=i-20,percent=s->disk_used[bin];
  uint32_t color=percent<0?0x7b8990u:percent>=90?0xe18c70u:percent>=75?0xd6b867u:0x6cc9b7u;
  desktop_art(c,ZELL_ART_BIN,x,y,164);
  /* The generated bin's dark front window remains a live capacity gauge. */
  int width=70,top=y-94,height=28;
  sr_fill_rect(c,(float)(x-width/2),(float)top,(float)width,(float)height,0x071318u,1);
  if(percent>0)sr_fill_rect(c,(float)(x-width/2),(float)top,(float)(width*percent)/100,(float)height,color,.7f);
  char value[24];if(percent<0)snprintf(value,sizeof value,"?");else snprintf(value,sizeof value,"%d%%",percent);
  sr_text(c,(float)(x-sr_text_width_in(SR_FONT_FIXED_8X16,value,1)/2),(float)(top+6),value,0xf3eee0u,1,1);
  if(i==near)sr_stroke_rect(c,(float)(x-70),(float)(y-164),140,166,2,0x96ead0u,1);
  char label[28];snprintf(label,sizeof label,"%.27s",s->disk_labels[bin]);desktop_label(c,x,y+16,label,color);
  if(i==near)desktop_label(c,x,y+40,"Enter: dig through drive",0xa8f3d5u);
 }
 if(!front&&s->room==2&&s->disk_count==0)desktop_label(c,ZELL_WORLD_W/2-cx,ZELL_WORLD_H/2-cy,s->disks_ready?"No eligible mounted disks found":"Reading mounted drives...",0xc9d8d2u);
 if(!front&&(s->room==1||s->room==2)){
  float x0=ZELL_STAIRS_LEFT*(float)ZELL_WORLD_W-(float)cx;
  float x1=ZELL_STAIRS_RIGHT*(float)ZELL_WORLD_W-(float)cx;
  float y=0.93f*(float)ZELL_WORLD_H-(float)cy,mid=(x0+x1)*0.5f;
  sr_fill_rect(c,x0,y,x1-x0,16,0x14201cu,.88f);
  sr_fill_triangle(c,mid-14.f,y+2.f,mid+14.f,y+2.f,mid,y+22.f,0xd7b56au,.95f);
 }
 if(front&&s->room==1)desktop_fleet_board(s,c,cx,cy);
}
static void desktop_vitals(const zell_scene *s,sr_canvas *c){
 int span=c->w-24;if(span>420)span=420;int width=(span-16)/3;
 if(width<32)return;
 int values[3]={s->hp,s->mana,s->fatigue};const char *names[3]={"HEALTH","MANA","FATIGUE"};
 uint32_t colors[3]={0xde8a79u,0x8ea6efu,0x87c5a4u};
 for(int i=0;i<3;i++){
  int x=12+i*(width+8);char label[40];
  if(values[i]<0)snprintf(label,sizeof label,"%s ?",names[i]);else snprintf(label,sizeof label,"%s %d%s",width<110?(i==0?"HP":i==1?"MP":"FT"):names[i],values[i],i==1&&s->mana_age>=3600?"*":"%");
  sr_fill_rect(c,(float)x,ZELL_ROOM_VITALS_Y,(float)width,ZELL_ROOM_VITALS_H,0x0c1921u,.95f);
  sr_text(c,(float)(x+4),ZELL_ROOM_VITALS_Y+3,label,colors[i],1,1);
  sr_fill_rect(c,(float)(x+4),ZELL_ROOM_VITALS_Y+25,(float)(width-8),7,0x263940u,1);
  if(values[i]>0)sr_fill_rect(c,(float)(x+4),ZELL_ROOM_VITALS_Y+25,(float)(width-8)*(float)values[i]/100,7,colors[i],1);
 }
 if(s->hp>=0&&s->hp<100&&s->room==0&&s->mode==ZELL_MODE_WALK)sr_text(c,12,83,s->restart_needed?"Restart needed: visit Gatekeeper":s->dirty_files>0?"Health low: hover bar for details":"Admin: OS update due",0xe5ad87u,1,1);
 if(s->mouse_valid&&s->mouse_x>=12&&s->mouse_x<12+width&&s->mouse_y>=ZELL_ROOM_VITALS_Y&&s->mouse_y<ZELL_ROOM_HUD_BOTTOM){
  char tip[600];
  char os[240],git[200];
  if(s->update_age_days<0)snprintf(os,sizeof os,"OS upgrade history is unknown; no age penalty is assumed.");
  else if(s->os_health>=100)snprintf(os,sizeof os,"OS health is full: upgraded less than 24 hours ago.");
  else snprintf(os,sizeof os,"OS upgrade was %d full days ago: OS health %d%%. Age costs 10 points/day from day two, floor 10%%.",s->update_age_days,s->os_health);
  if(s->dirty_files>0)snprintf(git,sizeof git,"%d dirty files in watched Git projects: minus %d points (combined floor 1%%). Visit Git Wizard downstairs.",s->dirty_files,s->dirty_files);
  else if(s->dirty_files<0)snprintf(git,sizeof git,"Some watched repositories are unavailable. Open Git Wizard for errors and known penalties.");
  else snprintf(git,sizeof git,"No known dirty-file penalty. Register watches with Git Wizard in Command Center.");
  snprintf(tip,sizeof tip,"%s %s %s Movement speed: %.0f%%.",os,git,s->restart_needed?"Restart needed: Gatekeeper shows installed kernel/driver changes.":"Administrator > Update OS restores OS health.",100*zell_desktop_speed_factor(s));
  int tw=c->w-32;if(tw>430)tw=430;int lines=wrap_count(tip,tw-24),height=lines*18+24;
  sr_fill_rect(c,12,105,(float)tw,(float)height,0x10232cu,.98f);sr_stroke_rect(c,12,105,(float)tw,(float)height,1,0xd6ba87u,1);
  draw_wrapped(c,24,117,tw-24,tip,0xf0e5cdu,0,lines);
 }
}
static void desktop_weather(const zell_scene *s,sr_canvas *c,int cx,int cy){
 if(s->room!=0||!s->weather_state)return;
 int x=157-cx,y=244-cy;
 int art=s->weather_icon>=0&&s->weather_icon<8?s->weather_icon:7;
 sr_fill_ellipse(c,(float)x,(float)(y-40),54,39,0x133a43u,.35f);
 desktop_art(c,ZELL_ART_WEATHER+art,x,y,s->weather_state==1?88:64);
 char label[96];
 if(s->weather_state==1)snprintf(label,sizeof label,"%s / %s",s->weather_temperature,s->weather_condition);
 else snprintf(label,sizeof label,"%s",s->weather_condition);
 desktop_label(c,x,y+6,label,0xffe7a6u);
}
static void desktop_cave(const zell_scene *s,sr_canvas *c,int cx,int cy,bool front){
 int near=zell_desktop_near(s);int height=112;
 for(int i=0;i<ZELL_STATIONS&&s->room==0;i++){
  if(!zell_desktop_station_visible(s,i)||zell_desktop_station_in_front(s,i)!=front)continue;
  const zell_station *p=&zell_stations[i];int x=(int)(p->x*(float)ZELL_WORLD_W)-cx,y=(int)(p->y*(float)ZELL_WORLD_H)-cy;
  if(i>=ZELL_GAME_HATCH&&i<=ZELL_LIBRARY_HATCH){
   int art=p->art+(i==near?(ZELL_ART_GAME_HATCH_OPEN-ZELL_ART_GAME_HATCH):0);
   const char *label=i==ZELL_GAME_HATCH?"Game room":i==ZELL_THEATER_HATCH?"Movie theater":i==ZELL_MEDITATION_HATCH?"Meditation":"Library";
   desktop_hatch(c,art,x,y,i==near,label);
   continue;
  }
  if(i>=ZELL_WORKSHOP_COMPUTER)continue;
  int h=(i<6||(i>=32&&i<42))?height:height*3/4;
  if(i==12){x+=ZELL_SWITCH_WALL_DX;y+=ZELL_SWITCH_WALL_DY;}
  /* A floor glow under a wall fitting would hang in the air; its label marks it. */
  if(i==near&&i!=12){sr_fill_ellipse(c,(float)x,(float)y, (float)h*.42f,6,0x80e5c4u,.65f);}
  if(i<6){zell_actor_pose pose=zell_desktop_pose(s,i);sr_fill_ellipse(c,(float)x,(float)y,23,7,0x071719u,.45f);desktop_art(c,pose.art,x+pose.offset_x,y+pose.offset_y,h);}
  if(i>=32&&i<42){desktop_art(c,p->art+(s->flame_tick/20+i)%4,x,y,h);desktop_label(c,x,y+10,p->name,0xe9d49fu);continue;}
  if(i==13||i==14||i==42||i==43){desktop_hatch(c,p->art,x,y,i==near,i==13?"Command center":i==14?"Laundry room":i==42?"Vault":"Workshop");continue;}
  /* The lever hangs on the east wall beside the shell desk, mirrored to face
   * the room. Its interaction point stays on the reachable floor edge. */
  if(i==12){desktop_art_mirrored(c,ZELL_ART_SWITCH,x,y+22,62);if(i!=near)desktop_label(c,x,y+28,"N-SHELL",0x91e9d8u);}
  if(i==6||i==7)desktop_label(c,x,y+8,i==6?"TUI":"SHELL",0x91e9d8u);
  if(i==near)desktop_label(c,x,y+(i==12?28:8),p->name,0xb9f8dbu);
 }
}
static int desktop_view_identity(const zell_scene *s){
 float x,y;
 if(s->room_view.scale!=1.f)return 0;
 zell_world_to_screen(s,0.f,0.f,&x,&y);
 return x==0.f&&y==0.f;
}
/* Copy one world-pixel room through the fitted view. HUD is drawn later. */
static void desktop_present_room(const zell_scene *s,sr_canvas *dst,const sr_canvas *world){
 sr_clear(dst,0x081319u);
 if(!world->px)return;
 if(desktop_view_identity(s)&&dst->w==world->w&&dst->h==world->h){
  memcpy(dst->px,world->px,(size_t)dst->w*(size_t)dst->h*4u);
  return;
 }
 for(int sy=0;sy<dst->h;sy++)for(int sx=0;sx<dst->w;sx++){
  float wx,wy;int ix,iy;
  zell_screen_to_world(s,(float)sx,(float)sy,&wx,&wy);
  ix=(int)floorf(wx);iy=(int)floorf(wy);
  if(ix<0||iy<0||ix>=world->w||iy>=world->h)continue;
  dst->px[(size_t)sy*(size_t)dst->w+(size_t)sx]=world->px[(size_t)iy*(size_t)world->w+(size_t)ix];
 }
}
/* Right side of the bottom strip. Hints stay on the left. Elapsed time, not frames. */
static void zell_notice_show(const zell_scene *s,sr_canvas *c,const char *hint){
 char line[160];
 int hint_w,left,right,room,width;
 size_t len;
 if(!s->notice_text[0]||s->notice_until_ms<=zell_notice_clock_ms())return;
 hint_w=hint&&hint[0]?sr_text_width_in(SR_FONT_FIXED_8X16,hint,1):0;
 if(hint_w<0)hint_w=0;
 left=12+hint_w+16;
 right=c->w-12;
 if(right-left<8)return;
 room=right-left;
 len=strlen(s->notice_text);
 if(len>159)len=159;
 memcpy(line,s->notice_text,len);
 line[len]=0;
 while(len>0&&sr_text_width_in(SR_FONT_FIXED_8X16,line,1)>room){
  len--;
  line[len]=0;
  if(len>=3)memcpy(line+len-3,"...",3);
 }
 if(!line[0])return;
 width=sr_text_width_in(SR_FONT_FIXED_8X16,line,1);
 if(width<1||width>room)return;
 sr_text(c,(float)(right-width),(float)(c->h-22),line,0xe9d49fu,1,1);
}
static void desktop_world_layer(const zell_scene *s,sr_canvas *c,int cx,int cy){
 static sr_canvas backgrounds[9];
 int room=s->room>=0&&s->room<9?s->room:0;
 sr_canvas *background=&backgrounds[room];
 const zell_art *a=zell_art_get(desktop_room_background(room));
 if(!background->px&&a){if(sr_canvas_init(background,ZELL_WORLD_W,ZELL_WORLD_H)){sr_canvas src;sr_canvas_wrap(&src,a->pixels,a->width,a->height);sr_blit_scaled(background,&src,0,0,ZELL_WORLD_W,ZELL_WORLD_H,1);}}
 sr_clear(c,0x081319u);
 if(background->px){for(int y=0;y<c->h&&y+cy<ZELL_WORLD_H;y++){int w=c->w;if(w+cx>ZELL_WORLD_W)w=ZELL_WORLD_W-cx;memcpy(c->px+(size_t)y*(size_t)c->w,background->px+(size_t)(y+cy)*ZELL_WORLD_W+(size_t)cx,(size_t)w*4);}}
 if(s->room)desktop_room(s,c,cx,cy,false);
 desktop_weather(s,c,cx,cy);
 int height=112;
 desktop_cave(s,c,cx,cy,false);
 if(s->room==0){
  bool enabled[3]={s->voice_on,s->dictation_on,!s->music_muted};
  const char *names[3]={"VOICE","DICTATE","MUSIC"};
  for(int i=0;i<3;i++){
   int x=98+21*i-cx,y=349-cy;
   float mwx,mwy;int mx=s->mouse_x,my=s->mouse_y;
   if(s->mouse_valid){zell_screen_to_world(s,(float)s->mouse_x,(float)s->mouse_y,&mwx,&mwy);mx=(int)floorf(mwx)-cx;my=(int)floorf(mwy)-cy;}
   sr_fill_circle(c,(float)x,(float)y,5,enabled[i]?0x85f7b2u:0x775446u,1);
   if(s->mouse_valid&&mx>=x-10&&mx<x+11&&my>=298-cy&&my<375-cy){
    char label[64];snprintf(label,sizeof label,"%s / %s",names[i],i==2?(s->music_muted?"PAUSED":s->dj_active?"DJ":"ON"):(enabled[i]?"ON":"OFF"));desktop_label(c,120-cx,386-cy,label,0xe9d49fu);
   }
  }
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
 zell_actor_pose pose=zell_desktop_pose(s,-1);
 int seat_dy=zell_seat_draw_dy(s);
 float shadow=1.f;
 if(s->seat_ticks>0&&s->seat_ticks<ZELL_SEAT_HOP_TICKS){
  int lift=zell_seat_lift(s->seat_ticks);
  if(lift>0)shadow=1.f-0.55f*(float)lift/(float)ZELL_SEAT_LIFT_PEAK;
 }
 /* The shadow stays on the floor line and shrinks as the hop lifts him. */
 sr_fill_ellipse(c,s->x-(float)cx,s->y-(float)cy,17.f*shadow,5.f*shadow,0x071719u,.45f);
 desktop_art(c,pose.art,(int)s->x-cx+pose.offset_x,(int)s->y-cy+pose.offset_y+seat_dy,height*3/4);
 if(s->room==4&&s->seated&&s->seat_ticks==ZELL_SEAT_HOP_TICKS)desktop_stool_front(c,cx,cy);
 if(desktop_room_foreground(s->room)>=0)desktop_registered_front(c,desktop_room_foreground(s->room),cx,cy);
 if(s->room)desktop_room(s,c,cx,cy,true);else desktop_cave(s,c,cx,cy,true);
 if(s->debug_blocks[0].enabled){
  int blocks=zell_desktop_block_count(s);
  for(int i=0;i<blocks;i++){
   float x0,y0,x1,y1;
   zell_desktop_block_rect(s,i,&x0,&y0,&x1,&y1);
   sr_stroke_rect(c,x0-(float)cx,y0-(float)cy,x1-x0,y1-y0,2.f,0xff3040u,1.f);
  }
 }
}
static void desktop_chrome(const zell_scene *s,sr_canvas *c,int cx,int cy){
 int near=zell_desktop_near(s);
 (void)cx;(void)cy;
 if(c->w>=500&&s->mode==ZELL_MODE_WALK){
  int mx=c->w-140,my=42;sr_fill_rect(c,(float)mx,(float)my,128,88,0x0c1921u,.9f);
  sr_stroke_rect(c,(float)mx,(float)my,128,88,1,0x4b7777u,1);
  for(int i=0;i<ZELL_STATIONS;i++)if((i<ZELL_WORKSHOP_COMPUTER||(i>=ZELL_GAME_HATCH&&i<=ZELL_LIBRARY_HATCH))&&zell_desktop_station_visible(s,i))sr_fill_circle(c,(float)(mx+4)+zell_stations[i].x*120,(float)(my+4)+zell_stations[i].y*80,2,0xd9ba79u,1);
  {
   float x0,y0,x1,y1;
   zell_screen_to_world(s,0.f,0.f,&x0,&y0);
   zell_screen_to_world(s,(float)c->w,(float)c->h,&x1,&y1);
   sr_stroke_rect(c,(float)(mx+4)+(x0/(float)ZELL_WORLD_W)*120,(float)(my+4)+(y0/(float)ZELL_WORLD_H)*80,((x1-x0)/(float)ZELL_WORLD_W)*120,((y1-y0)/(float)ZELL_WORLD_H)*80,1,0x407b85u,1);
  }
  sr_fill_circle(c,(float)(mx+4)+s->x/ZELL_WORLD_W*120,(float)(my+4)+s->y/ZELL_WORLD_H*80,3,0x8af8d4u,1);
 }
 sr_fill_rect(c,0,0,(float)c->w,30,0x0c1921u,.97f);
 sr_text(c,12,7,desktop_room_title(s->room),0xf0d69bu,1,1);
 if(s->dictation_on)desktop_label(c,c->w/2,82,"Listening / click dictation to finish",0xb7f3d5u);
 char status[100];time_t now=time(NULL);struct tm local;localtime_r(&now,&local);
 snprintf(status,sizeof status,"%02d:%02d  %d fps",local.tm_hour,local.tm_min,s->frame_rate);
 int sw=sr_text_width_in(SR_FONT_FIXED_8X16,status,1);sr_text(c,(float)(c->w-sw-12),7,status,0xa6d9ccu,1,1);
 sr_fill_rect(c,0,(float)(c->h-ZELL_ROOM_STATUS_STRIP),(float)c->w,ZELL_ROOM_STATUS_STRIP,0x0c1921u,.97f);
 const char *hint=s->mode==ZELL_MODE_WALK&&s->running?"Dashing | Release Shift to stop":s->mode==ZELL_MODE_WALK&&s->run_charge>0?"Charging dash... | Release Shift to cancel":s->mode==ZELL_MODE_SERVICE?"Station window | Esc back":near>=0?"Enter interact | Tab stations":c->w<500?"WASD | Shift run | Tab":"WASD walk | Hold Shift run | Tab stations";
 sr_text(c,12,(float)(c->h-22),hint,0xc7d8dau,1,1);
 /* L8 calls zell_notice_show. */
 zell_notice_show(s,c,hint);
 desktop_vitals(s,c);
}
static void desktop_base(const zell_scene *s,sr_canvas *c){
 int cx,cy;zell_desktop_camera(s,&cx,&cy);
 static sr_canvas world;
 if(!world.px&&!sr_canvas_init(&world,ZELL_WORLD_W,ZELL_WORLD_H)){
  desktop_world_layer(s,c,cx,cy);
  desktop_chrome(s,c,cx,cy);
  zell_theater_draw(s,c);
  return;
 }
 /* Every room is drawn in world pixels, then placed with one transform. */
 desktop_world_layer(s,&world,0,0);
 desktop_present_room(s,c,&world);
 desktop_chrome(s,c,cx,cy);
 zell_theater_draw(s,c);
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
 desktop_text(c,x+18,y+14,launch?"ZELL / STATIONS AND ACTIONS":zell_stations[s->station].name,0xf0d69bu,width-36);
 char warning[120];snprintf(warning,sizeof warning,"OS upgrade overdue: health %d%%. Choose Update OS.",s->hp);
 desktop_text(c,x+18,y+36,(!launch&&s->station==2&&s->hp>=0&&s->hp<100)?warning:launch?(s->launcher_query[0]?s->launcher_query:"Type to search every action in every room"):zell_stations[s->station].role,0x89aaa9u,width-36);
 for(int r=0;r<rows;r++){int i=r+first,yy=y+56+r*26;bool selected=i==s->menu_index;
 if(selected)sr_fill_rect(c,(float)(x+10),(float)(yy-4),(float)(width-20),27,0x254b50u,1);
 char label[240];if(launch)zell_desktop_launcher_label(s,i,label,sizeof label);
 desktop_text(c,x+20,yy,launch?label:zell_desktop_action(s->station,i),selected?0xe8ffe9u:0xb5c9cdu,width-40);
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

/* Window buttons: 0 minimize (bar at the foot), 1 maximize (one frame),
 * 2 restore (two overlapped frames). Drawn shapes, so no font glyph is needed. */
static void desktop_button(const zell_scene *s,sr_canvas *c,const zell_rect *r,int kind){
 bool hover=s->mouse_valid&&s->mouse_x>=r->x&&s->mouse_x<r->x+r->w&&s->mouse_y>=r->y&&s->mouse_y<r->y+r->h;
 uint32_t ink=hover?0xf4ffe9u:0xabc8c0u;
 sr_fill_rect(c,(float)r->x,(float)r->y,(float)r->w,(float)r->h,hover?0x2f5f63u:0x1b3a42u,1);
 sr_stroke_rect(c,(float)r->x,(float)r->y,(float)r->w,(float)r->h,1,0x74b8a9u,1);
 int cx=r->x+r->w/2,cy=r->y+r->h/2;
 if(kind==0)sr_fill_rect(c,(float)(cx-6),(float)(cy+4),12,2,ink,1);
 else if(kind==1)sr_stroke_rect(c,(float)(cx-6),(float)(cy-5),12,10,2,ink,1);
 else if(kind==3){sr_line(c,(float)(cx-5),(float)(cy-5),(float)(cx+5),(float)(cy+5),2,ink,1,0,0);sr_line(c,(float)(cx-5),(float)(cy+5),(float)(cx+5),(float)(cy-5),2,ink,1,0,0);}
 else {sr_stroke_rect(c,(float)(cx-3),(float)(cy-6),10,8,1,ink,1);sr_fill_rect(c,(float)(cx-7),(float)(cy-2),10,8,hover?0x2f5f63u:0x1b3a42u,1);sr_stroke_rect(c,(float)(cx-7),(float)(cy-2),10,8,1,ink,1);}
}
/* The minimized terminal: a button on the bottom bar, in every mode. */
static void desktop_terminal_tab(const zell_scene *s,sr_canvas *c){
 if(!s->terminal_minimized||s->service_phase!=5)return;
 zell_terminal_layout l;zell_terminal_layout_for(s,c->w,c->h,&l);
 bool hover=s->mouse_valid&&s->mouse_x>=l.tab.x&&s->mouse_x<l.tab.x+l.tab.w&&s->mouse_y>=l.tab.y&&s->mouse_y<l.tab.y+l.tab.h;
 sr_fill_rect(c,(float)l.tab.x,(float)l.tab.y,(float)l.tab.w,(float)l.tab.h,hover?0x2f5f63u:0x10232cu,.97f);
 sr_stroke_rect(c,(float)l.tab.x,(float)l.tab.y,(float)l.tab.w,(float)l.tab.h,2,0x74b8a9u,1);
 /* A blinking lamp says the command is still running behind the button. */
 sr_fill_rect(c,(float)(l.tab.x+8),(float)(l.tab.y+9),8,8,(s->flame_tick/16)%2?0x7bd88fu:0x2f6b45u,1);
 char title[120];snprintf(title,sizeof title,"%s",s->terminal_title[0]?s->terminal_title:"Neuronte terminal");
 desktop_text(c,l.tab.x+24,l.tab.y+5,title,hover?0xf4ffe9u:0xe5cf98u,l.tab.w-30);
}
/* The scene keeps animating beneath every station conversation. */
static void desktop_service(const zell_scene *s,sr_canvas *c){
 if(s->service_phase==5){
  zell_terminal_layout l;zell_terminal_layout_for(s,c->w,c->h,&l);
  int w=l.image.w,h=l.image.h,x=l.image.x,y=l.image.y;
  sr_fill_rect(c,0,30,(float)c->w,(float)(c->h-60),0x041017u,.35f);
  if(s->terminal_pixels&&w>0&&h>0){
   sr_fill_rect(c,(float)l.frame.x,(float)l.frame.y,(float)l.frame.w,(float)l.frame.h,0x10232cu,1);
   sr_stroke_rect(c,(float)l.frame.x,(float)l.frame.y,(float)l.frame.w,(float)l.frame.h,2,0x74b8a9u,1);
   sr_canvas frame;sr_canvas_wrap(&frame,s->terminal_pixels,w,h);sr_blit_scaled(c,&frame,x,y,w,h,1);
   char title[160];snprintf(title,sizeof title,"%s / %s",s->terminal_title[0]?s->terminal_title:"ZELL",s->terminal_private?"PRIVATE TERMINAL":"INTERACTIVE TERMINAL");
   desktop_text(c,x,y-23,title,0xe5cf98u,w-104);
   desktop_button(s,c,&l.minimize,0);
   desktop_button(s,c,&l.maximize,s->terminal_maximized?2:1);
   desktop_button(s,c,&l.close,3);
   desktop_text(c,x,y+h+7,s->terminal_private?"Hidden secret input | Ctrl-C cancels":"Type here for sudo | Ctrl-C interrupts command",0xabc8c0u,w);
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
/* Theater chrome. These live in the draw unit so the Neuronte macros apply. */
void zell_ui_fill(struct sr_canvas *canvas,int x,int y,int w,int h,uint32_t color,float alpha){
 if(!canvas||w<1||h<1)return;
 sr_fill_rect(canvas,(float)x,(float)y,(float)w,(float)h,color,alpha);
}
void zell_ui_stroke(struct sr_canvas *canvas,int x,int y,int w,int h,float thickness,uint32_t color,float alpha){
 if(!canvas||w<1||h<1)return;
 sr_stroke_rect(canvas,(float)x,(float)y,(float)w,(float)h,thickness,color,alpha);
}
static int ui_utf8_len(const char *s){
 unsigned char c;int n,i;
 if(!s||!s[0])return 0;
 c=(unsigned char)s[0];
 if((c&0x80u)==0)return 1;
 if((c&0xe0u)==0xc0u)n=2;
 else if((c&0xf0u)==0xe0u)n=3;
 else if((c&0xf8u)==0xf0u)n=4;
 else return 1;
 for(i=1;i<n;i++)if(((unsigned char)s[i]&0xc0u)!=0x80u)return 1;
 return n;
}
void zell_ui_text(struct sr_canvas *canvas,int x,int y,const char *text,uint32_t color,int width){
 char line[512];int cap,fit,out;const char *p;
 if(!canvas||!text)return;
 cap=width/8;if(cap<1)return;if(cap>511)cap=511;
 fit=0;
 for(p=text;*p&&*p!='\n';){
  int n=ui_utf8_len(p);
  if(fit+n>cap)break;
  fit+=n;p+=n;
 }
 if(*p==0||*p=='\n'){memcpy(line,text,(size_t)fit);line[fit]=0;}
 else{
  int budget=cap>3?cap-3:cap;
  out=0;
  for(p=text;*p&&*p!='\n';){
   int n=ui_utf8_len(p);
   if(out+n>budget||out+n>511)break;
   memcpy(line+out,p,(size_t)n);out+=n;p+=n;
  }
  if(cap>3&&out+3<=511){memcpy(line+out,"...",3);out+=3;}
  line[out]=0;
 }
 sr_text(canvas,(float)x,(float)y,line,color,1.f,1);
}
void zell_ui_blit(struct sr_canvas *canvas,int dx,int dy,int dw,int dh,const uint32_t *pixels,int src_w,int src_h){
 sr_canvas src;int x,y,w,h;
 if(!canvas||!pixels||dw<1||dh<1||src_w<1||src_h<1)return;
 sr_fill_rect(canvas,(float)dx,(float)dy,(float)dw,(float)dh,0x000000u,1.f);
 zell_theater_contain(src_w,src_h,dw,dh,&x,&y,&w,&h);
 if(w<1||h<1)return;
 sr_canvas_wrap(&src,(uint32_t *)pixels,src_w,src_h);
 sr_blit_scaled(canvas,&src,dx+x,dy+y,w,h,1.f);
}
