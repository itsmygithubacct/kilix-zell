/* Screen-space controls stay in the same place as the room underneath changes. */
static void hud_button(sr_canvas *c,zell_rect r,const char *label,bool active){
 if(r.w<1||r.h<1)return;
 cell_fill(c,r.x,r.y,r.w,r.h,active?0x86bca8u:0x476267u);
 cell_fill(c,r.x+1,r.y+1,r.w-2,r.h-2,active?0x243e36u:0x18292fu);
 int width=(int)strlen(label)*8;
 desktop_text(c,r.x+(width<=r.w-8?(r.w-width)/2:4),r.y+(r.h-16)/2,label,active?0xddf4e4u:0xc5d9d6u,r.w-6);
}
/* Wrap at spaces, retaining the full message across lines. */
static int hud_wrap(sr_canvas *c,int x,int y,int w,int end,const char *p,uint32_t color){
 int cols=w/8;
 if(cols<1)return y;
 if(cols>122)cols=122;
 while(*p&&y+16<=end){
  size_t n=strlen(p);
  if(n>(size_t)cols){
   n=(size_t)cols;
   while(n&&((unsigned char)p[n]&0xc0u)==0x80u)n--;
   size_t space=n;while(space&&p[space]!=' ')space--;
   if(space)n=space;
  }
  if(!n)break;
  char line[128];memcpy(line,p,n);line[n]=0;
  sr_text(c,(float)x,(float)y,line,color,1,1);
  p+=n;while(*p==' ')p++;
  y+=17;
 }
 return y;
}
static void desktop_speech(const zell_scene *s,sr_canvas *c){
 if(!s->bubble[0]||s->bubble_until<=zell_notice_clock_ms()||s->mode!=ZELL_MODE_WALK||s->utility_panel)return;
 float fx,fy;
 if(ZELL_ROOM_IS_PANES(s->room)){fx=s->x;fy=s->y-ZELL_PANES_KILIX_H;}
 else zell_world_to_screen(s,s->x,s->y-88,&fx,&fy);
 int w=c->w-12;if(w>320)w=320;if(w<56)return;
 int cols=(w-20)/8;
 int lines=1,used=0;
 for(const char *p=s->bubble;*p;){
  while(*p==' ')p++;
  size_t len=strcspn(p," ");if(!len)break;
  if(used&&used+1+(int)len>cols){lines++;used=0;}
  used+=(used?1:0)+(int)len;p+=len;
 }
 int h=lines*17+14,x=(int)fx-w/2,y=(int)fy-h-12;
 if(x<5)x=5;
 if(x+w>c->w-5)x=c->w-w-5;
 bool below=y<32;
 if(below)y=(int)fy+(ZELL_ROOM_IS_PANES(s->room)?ZELL_PANES_KILIX_H:70)+8;
 if(y+h>c->h-32)y=c->h-h-32;
 int tip=(int)fx;if(tip<x+10)tip=x+10;if(tip>x+w-10)tip=x+w-10;
 cell_fill(c,x,y,w,h,0x799b93u);
 cell_fill(c,x+1,y+1,w-2,h-2,0x15262au);
 for(int j=0;j<7;j++)cell_fill(c,tip-6+j,below?y-1-j:y+h+j,13-2*j,1,0x799b93u);
 hud_wrap(c,x+10,y+7,w-20,y+h,s->bubble,0xe4ece8u);
}
static void desktop_utilities(const zell_scene *s,sr_canvas *c){
 if(zell_theater_fullscreen())return;
 zell_utility_layout l;zell_utility_rects(s,&l);
 desktop_speech(s,c);
 hud_button(c,l.messages,"Messages",s->utility_panel==ZELL_MESSAGES_PANEL);
 hud_button(c,l.updates,"Updates",s->utility_panel==ZELL_UPDATES_PANEL);
 if(!s->utility_panel)return;
 zell_rect b=l.box;
 if(b.w<40||b.h<30)return;
 cell_fill(c,b.x,b.y,b.w,b.h,0x6d8c87u);
 cell_fill(c,b.x+1,b.y+1,b.w-2,b.h-2,0x101d24u);
 cell_fill(c,b.x+1,b.y+1,b.w-2,28,0x22363cu);
 desktop_text(c,b.x+12,b.y+7,s->utility_panel==ZELL_MESSAGES_PANEL?"Messages":"Updates",0xf0dbadu,b.w-45);
 hud_button(c,l.close,"X",false);
 if(s->utility_panel==ZELL_MESSAGES_PANEL){
  hud_button(c,l.older,"Older",false);hud_button(c,l.newer,"Newer",false);
  hud_button(c,l.all,"All",!s->stamina_messages_only);
  hud_button(c,l.stamina,"Stamina",s->stamina_messages_only);
  int y=b.y+65,skip=s->message_scroll;
  bool any=false;
  for(int i=0;i<s->message_count&&y+36<b.y+b.h;i++){
   const zell_message *m=&s->messages[i];
   if(s->stamina_messages_only&&!m->stamina)continue;
   if(skip>0){skip--;continue;}
   any=true;time_t stamp=(time_t)m->time;
   char when[32]="",head[80];struct tm local;
   if(localtime_r(&stamp,&local))strftime(when,sizeof when,"%b %d %H:%M",&local);
   snprintf(head,sizeof head,"%s   %s%s",when,m->source,m->stamina?" (stamina)":"");
   desktop_text(c,b.x+12,y,head,0x8bc5b0u,b.w-24);
   y=hud_wrap(c,b.x+12,y+18,b.w-24,b.y+b.h-5,m->text,0xe1e6e2u)+10;
  }
  if(!any)desktop_text(c,b.x+12,y,s->stamina_messages_only?"No stamina remarks yet":"No messages yet",0xaabdc0u,b.w-24);
  return;
 }
 int x=b.x+12,y=b.y+34;
 char health[140],age[90];
 if(s->upgraded_at){
  int64_t hours=((int64_t)time(NULL)-s->upgraded_at)/3600;if(hours<0)hours=0;
  snprintf(age,sizeof age,"Last upgrade  %lldd %lldh ago",(long long)(hours/24),(long long)(hours%24));
 }else snprintf(age,sizeof age,"Last upgrade  unknown");
 if(s->hp>=0)snprintf(health,sizeof health,"Health %d%%   %s",s->hp,age);
 else snprintf(health,sizeof health,"Health unknown   %s",age);
 desktop_text(c,x,y,health,0xdbe9dfu,b.w-24);
 desktop_text(c,x,y+17,b.w<450?"OS: -10 HP/day (minimum 10)":"OS health loses 10 HP per full day, down to 10.",0xaac0c1u,b.w-24);
 if(s->height>=330){
  char penalty[32],extra[116];
  if(s->dirty_files>=0)snprintf(penalty,sizeof penalty,"-%d HP",s->dirty_files);
  else snprintf(penalty,sizeof penalty,"unknown");
  snprintf(extra,sizeof extra,"Git %s   Restart cap %s",penalty,s->restart_needed?"85%":"inactive");
  desktop_text(c,x,y+35,extra,0xaac0c1u,b.w-24);
 }
 static const char *const actions[]={"OS upgrade   sudo apt update && sudo apt -y upgrade","Clean package cache   sudo apt clean",
     "Clean obsolete packages   sudo apt autoclean","Rotate due logs   sudo logrotate /etc/logrotate.conf","Browse user cache"};
 static const char *const compact[]={"OS upgrade","APT clean","APT autoclean","Rotate logs","Browse cache"};
 for(int i=0;i<ZELL_MAINT_ACTIONS;i++){
  zell_rect r=l.action[i];if(r.w<1)continue;
  bool active=s->utility_term==i+1;
  cell_fill(c,r.x,r.y,r.w,r.h,active?0x25473du:0x21343bu);
  desktop_text(c,r.x+8,r.y+(r.h-16)/2,active?"v":">",0xa6d9c4u,12);
  desktop_text(c,r.x+26,r.y+(r.h-16)/2,r.w<320?compact[i]:actions[i],0xd5e2dcu,r.w-34);
 }
 if(s->utility_term&&l.body.h>=16){
  cell_fill(c,l.body.x-2,l.body.y-2,l.body.w+4,l.body.h+4,COMMAND_GLASS);
  desktop_cells(c,ZELL_COMMAND_TERMS+s->utility_term-1,l.body,true);
 }
}
