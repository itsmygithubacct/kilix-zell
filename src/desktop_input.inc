/* Strip SGR pixel-mouse reports before feeding the existing keyboard decoder. */
typedef struct {char pending[96];size_t used;int64_t changed;} desktop_input;
static desktop_input pointer_input;
static void desktop_input_feed(zell_scene *s,const char *bytes,size_t count){
 for(size_t i=0;i<count;i++){
  char ch=bytes[i];
  if(!pointer_input.used&&ch!='\033'){kittykb_input_feed(&keyboard.input,&ch,1);continue;}
  pointer_input.pending[pointer_input.used++]=ch;pointer_input.changed=now_ms();
  size_t n=pointer_input.used;char *p=pointer_input.pending;
  if((n==2&&p[1]!='[')||(n==3&&p[2]!='<')){
   kittykb_input_feed(&keyboard.input,p,n);pointer_input.used=0;continue;
  }
  if(n>=4&&(ch=='M'||ch=='m')){
   p[n]=0;unsigned buttons,x,y;char end;
   if(sscanf(p,"\033[<%u;%u;%u%c",&buttons,&x,&y,&end)==4&&x>0&&y>0&&x<100000&&y<100000){
    s->mouse_x=(int)x-1-kittyfb_origin_x(&framebuffer);s->mouse_y=(int)y-1-kittyfb_origin_y(&framebuffer);s->mouse_valid=true;
   }
   pointer_input.used=0;
  }else if(n>=sizeof pointer_input.pending-1){pointer_input.used=0;}
 }
}
static int desktop_input_read(zell_scene *s){
 char bytes[1024];size_t room=KITTYKB_EVENT_QUEUE_CAPACITY-keyboard.input.queue_count;
 if(room<2)return 0;
 if(room>sizeof bytes)room=sizeof bytes;
 ssize_t n=read(STDIN_FILENO,bytes,room-1);
 if(n>0)desktop_input_feed(s,bytes,(size_t)n);
 else if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)return -1;
 if(pointer_input.used&&now_ms()-pointer_input.changed>30){
  if(pointer_input.used<=2)kittykb_input_feed(&keyboard.input,pointer_input.pending,pointer_input.used);
  pointer_input.used=0;
 }
 if(kittykb_input_has_pending_escape(&keyboard.input)&&now_ms()-pointer_input.changed>30)kittykb_input_flush_escape(&keyboard.input);
 return 0;
}
static void desktop_mouse_mode(bool enabled){
 const char *sequence=enabled?"\033[?1003h\033[?1006h\033[?1016h":"\033[?1016l\033[?1006l\033[?1003l";
 (void)write(STDOUT_FILENO,sequence,strlen(sequence));
}
