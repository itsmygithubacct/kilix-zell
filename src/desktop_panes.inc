/* Pane rooms, through the Command Center's lower corners: a Kilix tab of two
 * panes on the left and of four on the right, side by side, across the whole
 * Zell pane. Along the top is a walkway Kilix walks on, with buttons over
 * each pane that start a coding agent in it; under it each pane has a title
 * bar with Zoom and Restart, then its cells. A click on a pane gives it the
 * keys; a click on the walkway gives them back to Kilix.
 *
 * tools/zell_panes.py owns the shells and paints their cells, in its own
 * process group with its own pipes and map, as the theater's helper does.
 * The pump starts it the first time a pane room is shown, never the tick,
 * so tests walk in and out without a process. Its shells keep running while
 * Kilix is elsewhere; only the shown room is painted. Zell keeps which pane
 * is focused and zoomed and tells the helper. Pointer reports over a
 * pane's cells, pastes and the scrollbar go to the helper too; it says how
 * far back each pane is scrolled (B) and sends selected text (Y), which
 * main puts on the outer terminal's clipboard. The map starts with 64 bytes:
 * "ZPNE", version 1, width, height, sequence (odd while a frame is
 * written), room. */
#include <stdatomic.h>
#define PN_MAX_W ZELL_PANES_MAX_W
#define PN_MAX_H ZELL_PANES_MAX_H
#define PN_MAP_BYTES (64u+(size_t)PN_MAX_W*PN_MAX_H*4u)
#define PN_QUEUE 65536
/* Pasted bytes per line to the helper (twice that in hex). */
#define PN_PASTE_CHUNK 2048
/* Typed while a paste goes at most, in K lines. */
/* Under PN_QUEUE, so what waits always fits the queue once it drains. */
#define PN_AFTER_MAX 32768
/* Base64 of one copy, at most. */
#define PN_COPY_MAX (1u<<20)
/* The helper's pipe to Zell holds a whole copy (its base64 and line ends),
 * and Zell reads what is there each frame up to PN_READ_MAX: a large copy
 * neither stalls the helper, and so every pane, nor takes many frames. */
#define PN_PIPE_BYTES (2u<<20)
#define PN_READ_MAX (4u<<20)
#define PN_COPY_HEAD "\033]52;c;"
/* The kitty keyboard protocol's numbers (kitty_keyboard.h): Shift, the
 * lock modifiers, and the keys that scroll. */
#define PN_MOD_SHIFT 1u
#define PN_MOD_LOCKS (64u|128u)
#define PN_KEY_PAGE_UP 0xe00au
#define PN_KEY_PAGE_DOWN 0xe00bu
#define PN_KEY_HOME 0xe00cu
#define PN_KEY_END 0xe00du
#define PN_NOTICE_MS 4000
#define PN_CONFIRM_MS 3000

typedef struct { char id[16],label[24]; } pn_agent;
typedef struct {
    pid_t pid;
    int in_fd,out_fd,frame_fd;
    void *map;
    uint32_t *shown,*scratch;
    size_t cap;
    uint32_t seq;
    int frame_w,frame_h,frame_room;
    /* Last room and size sent: room 0 is hidden. */
    int sent_room,sent_cols,sent_rows;
    bool failed;
    char error[160];
    char queue[PN_QUEUE];
    size_t queued;
    char line[4096];
    size_t line_n;
    bool line_over;
    /* Who has the keys, and per room (left, right) the focused pane and
     * the zoomed one (-1 for none). */
    bool keys_pane;
    /* He has not moved since walking in: his place follows the room's
     * size, which grows past the Command Center's a frame later. */
    bool entering;
    int focus[2],zoom[2];
    /* The Hotkeys window is open over the room. */
    bool hotkeys;
    int hotkeys_top; /* its first line shown, when not all fit */
    char title[2][4][32];
    pn_agent agents[ZELL_PANES_AGENTS];
    int agent_count;
    char notice[96];
    int notice_room,notice_pane;
    int64_t notice_until;
    int confirm_room,confirm_pane;
    int64_t confirm_until;
    /* Per room and pane, as the helper said: rows kept, first shown, shown,
     * and whether its program wants pointer reports (1) and motion with no
     * button held (2). */
    uint64_t bar[2][4][3];
    unsigned bar_flags[2][4];
    bool bar_known[2][4];
    /* The pane a press in its cells holds until the button goes up, the
     * pane whose scrollbar is dragged, the pane whose scrollbar the pointer
     * is on (-1: none), and the last cell a bare motion was sent for. */
    int grab,bar_grab,bar_hover,motion_pane,motion_col,motion_row;
    /* A paste on its way to a pane, sent from paste_at. */
    char *paste;
    size_t paste_n,paste_at,paste_cap;
    int paste_room,paste_pane;
    /* Text to copy: the OSC 52 sequence, complete once ready. */
    char *copy;
    size_t copy_n;
    bool copy_ready,copy_bad;
    /* Lines queued while a paste goes (keys, focus, clicks, zoom,
     * restart), sent after its end line, in order. */
    char after[PN_AFTER_MAX];
    size_t after_n;
    /* Where he walked in: Enter presses nothing there until he moves. */
    int rest_room;
    float rest_x;
} pn_state;

#define PN_START {.in_fd=-1,.out_fd=-1,.frame_fd=-1,.zoom={-1,-1},.grab=-1,.bar_grab=-1,.bar_hover=-1,.motion_pane=-1}
static pn_state pn=PN_START;

static int pn_slot(int room){return room==ZELL_ROOM_PANES_LEFT?0:room==ZELL_ROOM_PANES_RIGHT?1:-1;}
int zell_panes_count(int room){return room==ZELL_ROOM_PANES_LEFT?2:room==ZELL_ROOM_PANES_RIGHT?4:0;}
/* The left room is entered from its right and is left the same way. */
int zell_panes_exit_side(int room){return room==ZELL_ROOM_PANES_LEFT?1:room==ZELL_ROOM_PANES_RIGHT?-1:0;}
void zell_panes_frame_limits(int room,int *max_w,int *max_h){
    if(ZELL_ROOM_IS_PANES(room)){*max_w=ZELL_PANES_MAX_W;*max_h=ZELL_PANES_MAX_H;}
    else {*max_w=1600;*max_h=1000;}
}
bool zell_panes_keys_to_pane(void){return pn.keys_pane;}
int zell_panes_route(bool press_key){return pn.keys_pane?ZELL_PANES_KEY_PANE:press_key?ZELL_PANES_KEY_PRESS:ZELL_PANES_KEY_NONE;}
int zell_panes_focus(int room){int slot=pn_slot(room);return slot<0?-1:pn.focus[slot];}
int zell_panes_zoom(int room){int slot=pn_slot(room);return slot<0?-1:pn.zoom[slot];}
const char *zell_panes_title(int room,int index){
    int slot=pn_slot(room);
    return slot<0||index<0||index>=zell_panes_count(room)?"":pn.title[slot][index];
}
const char *zell_panes_notice(int room,int index){
    if(!pn.notice[0]||pn.notice_room!=room||pn.notice_pane!=index)return NULL;
    if(zell_notice_clock_ms()>=pn.notice_until){pn.notice[0]=0;return NULL;}
    return pn.notice;
}
bool zell_panes_confirming(int room,int index){
    return pn.confirm_room==room&&pn.confirm_pane==index&&zell_notice_clock_ms()<pn.confirm_until;
}
int zell_panes_agent_count(void){return pn.agent_count;}
const char *zell_panes_agent_label(int agent){return agent>=0&&agent<pn.agent_count?pn.agents[agent].label:"";}

void zell_panes_exit_rect(const zell_scene *s,zell_rect *r){
    int side=zell_panes_exit_side(s->room),w=s->width>0?s->width:1;
    if(!side){*r=(zell_rect){0};return;}
    *r=(zell_rect){side>0?w-ZELL_PANES_EXIT_W:0,0,ZELL_PANES_EXIT_W,ZELL_PANES_EXIT_H};
}
static void pn_bounds(const zell_scene *s,float *x0,float *x1){
    int w=s->width>0?s->width:1;
    *x0=(float)ZELL_PANES_SIDE;*x1=(float)(w-ZELL_PANES_SIDE);
    if(*x1<*x0)*x1=*x0;
}
static void pn_stand(zell_scene *s){
    s->mode=ZELL_MODE_WALK;s->running=false;
    s->step=0;s->step_ticks=0;s->desktop_launch=false;
}
static void pn_place(zell_scene *s){
    float before=s->x;
    float x0,x1;
    pn_bounds(s,&x0,&x1);
    /* Just inside, beside the line he came in by, facing into the room. */
    if(zell_panes_exit_side(s->room)>0){s->x=x1-48.f;s->direction=2;}
    else {s->x=x0+48.f;s->direction=3;}
    if(s->x<x0)s->x=x0;
    if(s->x>x1)s->x=x1;
    s->y=(float)ZELL_PANES_FEET_Y;
    /* Placed again as the room grows while he walks in: where he stands is
     * still where he walked in. */
    if(pn.rest_room==s->room&&before==pn.rest_x)pn.rest_x=s->x;
}
void zell_panes_enter(zell_scene *s,int room){
    if(!zell_panes_count(room))return;
    s->room=room;pn_stand(s);
    /* He walked in: the keys are his. */
    pn.keys_pane=false;pn.confirm_room=0;pn.entering=true;pn.hotkeys=false;pn.hotkeys_top=0;
    pn.grab=pn.bar_grab=pn.bar_hover=pn.motion_pane=-1;
    pn_place(s);
    pn.rest_room=room;pn.rest_x=s->x;
}
void zell_panes_leave(zell_scene *s){
    int side=zell_panes_exit_side(s->room);
    if(!side)return;
    /* Back in the tunnel of the opening he left by, facing the room. */
    s->room=1;pn_stand(s);
    s->x=side>0?(float)ZELL_COMMAND_RETURN_X:(float)(ZELL_WORLD_W-ZELL_COMMAND_RETURN_X);
    s->y=ZELL_COMMAND_LANDING_Y;
    s->direction=side>0?3:2;
}
int zell_panes_tick(zell_scene *s,const zell_controls *c){
    float x0,x1,dx=pn.keys_pane?0.f:(float)c->move_x,dy=pn.keys_pane?0.f:(float)c->move_y;
    int side=zell_panes_exit_side(s->room);
    s->mode=ZELL_MODE_WALK;s->running=false;
    if(dx!=0.f)pn.entering=false;
    if(pn.entering){int direction=s->direction;pn_place(s);if(dy!=0.f)s->direction=direction;}
    pn_bounds(s,&x0,&x1);
    /* Along the walkway only: up and down turn him. Shift runs him, at
     * once: there is nothing on the walkway to run into. */
    if(dx!=0.f){
        s->running=c->run;
        float speed=(s->running?ZELL_RUN_SPEED:ZELL_WALK_SPEED)*zell_desktop_speed_factor(s);
        s->direction=dx<0?2:3;
        s->x+=dx>0?speed:-speed;
        s->step_ticks++;s->step=(s->step_ticks/(s->running?3:7))%2;
    }else{
        if(dy!=0.f)s->direction=dy<0?1:0;
        s->step=0;s->step_ticks=0;
    }
    if(s->x<x0)s->x=x0;
    if(s->x>x1)s->x=x1;
    s->y=(float)ZELL_PANES_FEET_Y;
    /* Out by walking into the line at the walkway's end. */
    if((side>0&&dx>0&&s->x>=x1)||(side<0&&dx<0&&s->x<=x0))zell_panes_leave(s);
    return 1;
}
void zell_panes_cells(int width,int height,int *cols,int *rows){
    int c=width/8,r=(height-ZELL_PANES_CELLS_Y)/16;
    if(c<20)c=20;
    if(c>PN_MAX_W/8)c=PN_MAX_W/8;
    if(r<8)r=8;
    if(r>PN_MAX_H/16)r=PN_MAX_H/16;
    *cols=c;*rows=r;
}
/* The helper's split: panes as even as can be, wider ones first, one
 * column of line between neighbours. */
static int pn_width(int cols,int count,int index){
    int room=cols-(count-1);
    return room/count+(index<room%count);
}
static int pn_left(int cols,int count,int index){
    int x=0;
    for(int i=0;i<index;i++)x+=pn_width(cols,count,i)+1;
    return x;
}
void zell_panes_pane_rect(const zell_scene *s,int index,zell_rect *r){
    int count=zell_panes_count(s->room),slot=pn_slot(s->room),cols,rows;
    *r=(zell_rect){0};
    if(index<0||index>=count)return;
    zell_panes_cells(s->width>0?s->width:1,s->height>0?s->height:1,&cols,&rows);
    int zoom=pn.zoom[slot];
    if(zoom>=0){
        if(index==zoom)*r=(zell_rect){0,ZELL_PANES_CELLS_Y,cols*8,rows*16};
        return;
    }
    *r=(zell_rect){pn_left(cols,count,index)*8,ZELL_PANES_CELLS_Y,pn_width(cols,count,index)*8,rows*16};
}
void zell_panes_chrome_rect(const zell_scene *s,int index,zell_rect *r){
    zell_panes_pane_rect(s,index,r);
    if(r->w){r->y=ZELL_PANES_WALK_H;r->h=ZELL_PANES_CHROME_H;}
}
/* Right-aligned in the title bar: End (only while scrolled back), Zoom,
 * then Restart. They go when the bar is too narrow for them and a title. */
void zell_panes_chrome_button(const zell_scene *s,int index,int button,zell_rect *r){
    zell_rect bar;zell_panes_chrome_rect(s,index,&bar);
    *r=(zell_rect){0};
    if(!bar.w||bar.w<68+60+12+56||button<ZELL_PANES_ZOOM||button>ZELL_PANES_END)return;
    int restart_x=bar.x+bar.w-4-68,zoom_x=restart_x-4-60;
    if(button==ZELL_PANES_RESTART)*r=(zell_rect){restart_x,bar.y+1,68,bar.h-2};
    else if(button==ZELL_PANES_ZOOM)*r=(zell_rect){zoom_x,bar.y+1,60,bar.h-2};
    else if(bar.w>=68+60+44+16+56&&zell_panes_lines_back(s->room,index)>0)*r=(zell_rect){zoom_x-4-44,bar.y+1,44,bar.h-2};
}
bool zell_panes_scrollback(int room,int index,uint64_t out[3]){
    int slot=pn_slot(room);
    if(slot<0||index<0||index>=zell_panes_count(room)||!pn.bar_known[slot][index])return false;
    memcpy(out,pn.bar[slot][index],sizeof pn.bar[slot][index]);
    return true;
}
uint64_t zell_panes_lines_back(int room,int index){
    uint64_t b[3];
    if(!zell_panes_scrollback(room,index,b)||b[0]<b[1]+b[2])return 0;
    return b[0]-b[1]-b[2];
}
/* The right BAR_W pixels of the cells, while rows are kept out of view;
 * the thumb is as much of it as the rows shown are of those kept. */
void zell_panes_scrollbar_rect(const zell_scene *s,int index,zell_rect *track,zell_rect *thumb){
    zell_rect pane;uint64_t b[3];
    *track=*thumb=(zell_rect){0};
    zell_panes_pane_rect(s,index,&pane);
    if(pane.w<2*ZELL_PANES_BAR_W||pane.h<ZELL_PANES_BAR_MIN||!zell_panes_scrollback(s->room,index,b)||!b[2]||b[0]<=b[2])return;
    uint64_t last=b[0]-b[2],first=b[1]>last?last:b[1];
    int h=(int)((uint64_t)pane.h*b[2]/b[0]);
    if(h<ZELL_PANES_BAR_MIN)h=ZELL_PANES_BAR_MIN;
    *track=(zell_rect){pane.x+pane.w-ZELL_PANES_BAR_W,pane.y,ZELL_PANES_BAR_W,pane.h};
    *thumb=(zell_rect){track->x+2,pane.y+(int)((uint64_t)(pane.h-h)*first/last),ZELL_PANES_BAR_W-4,h};
}
int zell_panes_pane_at(const zell_scene *s,int x){
    int count=zell_panes_count(s->room),slot=pn_slot(s->room),cols,rows;
    if(!count||x<0)return -1;
    if(pn.zoom[slot]>=0)return pn.zoom[slot];
    zell_panes_cells(s->width>0?s->width:1,s->height>0?s->height:1,&cols,&rows);
    for(int i=0;i<count-1;i++)if(x<(pn_left(cols,count,i)+pn_width(cols,count,i)+1)*8)return i;
    return count-1;
}
/* The plaque in the middle of a pane's part of the walkway: Kilix standing
 * there and pressing gives the pane the keys. None on a pane narrower than
 * ZELL_PANES_PAD_MIN. */
/* The Hotkeys button: at the walkway's right end, 12 pixels in (104 when
 * the way back is there, so Kilix does not come in standing on it). */
void zell_panes_hotkeys_rect(const zell_scene *s,zell_rect *r){
    *r=(zell_rect){0};
    if(!zell_panes_count(s->room))return;
    int width=s->width>0?s->width:1,w=(int)strlen(ZELL_PANES_HOTKEYS_LABEL)*8+16;
    int x=width-(zell_panes_exit_side(s->room)>0?104:12)-w;
    if(x<12)return;
    *r=(zell_rect){x,ZELL_PANES_BUTTON_Y,w,ZELL_PANES_BUTTON_H};
}
bool zell_panes_hotkeys_open(void){return pn.hotkeys;}
void zell_panes_hotkeys_show(bool open){pn.hotkeys=open;pn.hotkeys_top=0;}
/* How many of its lines the Hotkeys window shows in a Zell `height` tall:
 * all, or as many as leave a line for saying there are more. */
int zell_panes_hotkeys_fit(int height){
    int room=height-ZELL_PANES_WALK_H-16-ZELL_PANES_HOTKEYS_EDGES;
    if(zell_panes_hotkeys_lines*ZELL_PANES_HOTKEYS_LINE_H<=room)return zell_panes_hotkeys_lines;
    int fit=room/ZELL_PANES_HOTKEYS_LINE_H-1;
    return fit<1?1:fit;
}
/* Its first line shown in a Zell `height` tall: where it was scrolled to,
 * or, scrolled in a shorter Zell, no further down than leaves it full. */
int zell_panes_hotkeys_top(int height){
    int last=zell_panes_hotkeys_lines-zell_panes_hotkeys_fit(height);
    return pn.hotkeys_top>last?last:pn.hotkeys_top;
}
void zell_panes_hotkeys_scroll(const zell_scene *s,int lines){
    int last=zell_panes_hotkeys_lines-zell_panes_hotkeys_fit(s->height);
    int top=zell_panes_hotkeys_top(s->height)+lines;
    pn.hotkeys_top=top>last?last:top;
    if(pn.hotkeys_top<0)pn.hotkeys_top=0;
}
void zell_panes_pad_rect(const zell_scene *s,int index,zell_rect *r){
    zell_rect pane,hotkeys;zell_panes_pane_rect(s,index,&pane);
    *r=(zell_rect){0};
    if(!pane.w||pane.w<ZELL_PANES_PAD_MIN)return;
    *r=(zell_rect){pane.x+pane.w/2-ZELL_PANES_PAD_W/2,ZELL_PANES_BUTTON_Y,ZELL_PANES_PAD_W,ZELL_PANES_BUTTON_H};
    /* Clear of the Hotkeys button: moved left of it, or not there. */
    zell_panes_hotkeys_rect(s,&hotkeys);
    if(hotkeys.w&&r->x<hotkeys.x+hotkeys.w&&r->x+r->w+6>hotkeys.x){
        int x=hotkeys.x-6-ZELL_PANES_PAD_W;
        if(x<pane.x+8){*r=(zell_rect){0};return;}
        r->x=x;
    }
}
/* Left to right from 12 pixels into the pane's part of the walkway (104
 * when the way back is at its left, so Kilix does not come in standing on
 * one), 6 apart, past the plaque in the middle; one that would pass 8
 * pixels short of its end is not shown, nor any after it. */
void zell_panes_button_rect(const zell_scene *s,int index,int agent,zell_rect *r){
    zell_rect pane,pad,hotkeys;zell_panes_pane_rect(s,index,&pane);
    *r=(zell_rect){0};
    if(!pane.w||agent<0||agent>=pn.agent_count)return;
    zell_panes_pad_rect(s,index,&pad);
    zell_panes_hotkeys_rect(s,&hotkeys);
    int x=pane.x+(pane.x==0&&zell_panes_exit_side(s->room)<0?104:12);
    for(int i=0;i<=agent;i++){
        int w=(int)strlen(pn.agents[i].label)*8+16;
        if(pad.w&&x<pad.x+pad.w+6&&x+w+6>pad.x)x=pad.x+pad.w+6;
        if(x+w>pane.x+pane.w-8)return;
        /* Nor one that would reach the Hotkeys button. */
        if(hotkeys.w&&x<hotkeys.x+hotkeys.w&&x+w+6>hotkeys.x)return;
        if(i==agent){*r=(zell_rect){x,ZELL_PANES_BUTTON_Y,w,ZELL_PANES_BUTTON_H};return;}
        x+=w+6;
    }
}
static bool pn_inside(zell_rect r,int x,int y){return r.w>0&&x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h;}
static void pn_notice(int room,int index,const char *text);
static int pn_grow_pipe(int fd);
static bool pn_send_now(const char *text,size_t n){
    if(n==0||pn.queued+n>=sizeof pn.queue)return false;
    memcpy(pn.queue+pn.queued,text,n);pn.queued+=n;pn.queue[pn.queued]=0;
    return true;
}
/* A line for the helper. While a paste is still being sent, it waits
 * behind the paste with the others, so the helper sees them in the order
 * they were made (keys typed then a click on another pane go where they
 * were typed). False when it could not be queued. */
static bool pn_send(const char *text,size_t n){
    if(!pn.paste_n&&!pn.after_n)return pn_send_now(text,n);
    if(n==0)return false;
    if(pn.after_n+n>sizeof pn.after){
        /* Dropped: said on the pane the paste goes to. */
        if(pn.paste_room)pn_notice(pn.paste_room,pn.paste_pane,"Input made while the paste is sent was dropped.");
        return false;
    }
    memcpy(pn.after+pn.after_n,text,n);pn.after_n+=n;
    return true;
}
/* One formatted line; ok says whether it was queued. Zell changes what
 * it shows only when the helper will be told. */
#define pn_sendf(ok,...) do{char pn_l[64];int pn_n=snprintf(pn_l,sizeof pn_l,__VA_ARGS__);(ok)=pn_n>0&&(size_t)pn_n<sizeof pn_l&&pn_send(pn_l,(size_t)pn_n);}while(0)
const char *zell_panes_pending(void){pn.queue[pn.queued<sizeof pn.queue?pn.queued:sizeof pn.queue-1]=0;return pn.queue;}
void zell_panes_key(const unsigned char *bytes,size_t count){
    static const char hex[]="0123456789abcdef";
    char line[1024];
    size_t at=0;
    /* One line per batch; a batch that cannot fit whole is dropped whole. */
    if(!bytes||!count||2u*count+3u>sizeof line)return;
    line[at++]='K';
    for(size_t i=0;i<count;i++){line[at++]=hex[bytes[i]>>4];line[at++]=hex[bytes[i]&15u];}
    line[at++]='\n';
    /* Typed after a paste that is still going: it follows the paste. */
    pn_send(line,at);
}
/* Pane index gets the keys, unless the helper cannot be told. */
static void pn_take(const zell_scene *s,int index){
    int slot=pn_slot(s->room);
    bool ok=true;
    if(pn.focus[slot]!=index){pn_sendf(ok,"F %d\n",index);if(!ok)return;pn.focus[slot]=index;}
    pn.keys_pane=true;
}
/* The agent starts in pane index and the pane gets the keys: both lines are
 * sent, or neither. */
static void pn_start_agent(const zell_scene *s,int index,int agent){
    int slot=pn_slot(s->room);
    char line[160];
    int n=pn.focus[slot]!=index?snprintf(line,sizeof line,"A %d %s\nF %d\n",index,pn.agents[agent].id,index)
                                 :snprintf(line,sizeof line,"A %d %s\n",index,pn.agents[agent].id);
    if(n<=0||(size_t)n>=sizeof line||!pn_send(line,(size_t)n))return;
    pn.focus[slot]=index;pn.keys_pane=true;
}
/* A title-bar button of pane index, clicked or pressed by Kilix. False when
 * the helper could not be told. */
static bool pn_chrome(const zell_scene *s,int index,int button){
    int slot=pn_slot(s->room);
    bool ok=true;
    if(button==ZELL_PANES_END){
        pn_sendf(ok,"W %d bottom\n",index);
    }else if(button==ZELL_PANES_ZOOM){
        int zoomed=pn.zoom[slot]==index?-1:index;
        pn_sendf(ok,"Z %d\n",zoomed);
        /* The helper gives a pane it zooms the focus too. */
        if(ok){pn.zoom[slot]=zoomed;if(zoomed>=0)pn.focus[slot]=zoomed;}
    }else if(button==ZELL_PANES_RESTART){
        /* Once arms it; again within 3 seconds restarts the pane. */
        if(zell_panes_confirming(s->room,index)){pn_sendf(ok,"X %d\n",index);if(ok)pn.confirm_room=0;}
        else {pn.confirm_room=s->room;pn.confirm_pane=index;pn.confirm_until=zell_notice_clock_ms()+PN_CONFIRM_MS;}
    }
    return ok;
}
static bool pn_under(zell_rect r,float x){return r.w>0&&x>=(float)r.x&&x<(float)(r.x+r.w);}
/* What Enter would press where Kilix stands: of the agent buttons on the
 * wall, the plaque and the title-bar buttons below him, the one whose
 * middle is nearest him (on a narrow pane they overlap, and each is still
 * reached at its middle); else the pane itself. */
static void pn_nearer(zell_rect r,float x,int kind,int index,int which,float *best,int *aim,int *best_index,int *best_which){
    if(!pn_under(r,x))return;
    float d=x-((float)r.x+(float)r.w/2.f);
    if(d<0)d=-d;
    if(d>=*best)return;
    *best=d;*aim=kind;*best_index=index;*best_which=which;
}
int zell_panes_aim(const zell_scene *s,int *index,int *which){
    int count=zell_panes_count(s->room);
    *index=*which=-1;
    if(!count||pn.keys_pane)return ZELL_PANES_AIM_NONE;
    /* Where he walked in nothing is aimed at (Restart can be there) until
     * he moves. */
    if(pn.rest_room==s->room&&s->x==pn.rest_x)return ZELL_PANES_AIM_NONE;
    pn.rest_room=0;
    int aim=ZELL_PANES_AIM_NONE;float best=1e9f;
    for(int i=0;i<count;i++)for(int agent=0;agent<pn.agent_count;agent++){
        zell_rect b;zell_panes_button_rect(s,i,agent,&b);
        pn_nearer(b,s->x,ZELL_PANES_AIM_AGENT,i,agent,&best,&aim,index,which);
    }
    zell_rect hotkeys;zell_panes_hotkeys_rect(s,&hotkeys);
    pn_nearer(hotkeys,s->x,ZELL_PANES_AIM_HOTKEYS,-1,-1,&best,&aim,index,which);
    int pane=zell_panes_pane_at(s,(int)s->x);
    if(pane>=0){
        zell_rect pad;zell_panes_pad_rect(s,pane,&pad);
        pn_nearer(pad,s->x,ZELL_PANES_AIM_PAD,pane,-1,&best,&aim,index,which);
        for(int button=ZELL_PANES_ZOOM;button<=ZELL_PANES_END;button++){
            zell_rect b;zell_panes_chrome_button(s,pane,button,&b);
            pn_nearer(b,s->x,ZELL_PANES_AIM_CHROME,pane,button,&best,&aim,index,which);
        }
    }
    if(aim!=ZELL_PANES_AIM_NONE)return aim;
    if(pane<0)return ZELL_PANES_AIM_NONE;
    *index=pane;
    return ZELL_PANES_AIM_PANE;
}
/* Kilix over the middle of pane index's part of the walkway, facing it. */
static void pn_stand_over(zell_scene *s,int index){
    zell_rect r;float x0,x1;
    zell_panes_pane_rect(s,index,&r);
    if(r.w<=0)return;
    pn_bounds(s,&x0,&x1);
    float x=(float)r.x+(float)r.w/2.f;
    s->x=x<x0?x0:x>x1?x1:x;
    s->direction=0;
    pn.entering=false;
}
bool zell_panes_move(zell_scene *s,int dx,int dy){
    if(!ZELL_ROOM_IS_PANES(s->room))return false;
    int slot=pn_slot(s->room),count=zell_panes_count(s->room);
    /* While one pane fills the room the others cannot be gone to. */
    bool zoomed=pn.zoom[slot]>=0;
    if(pn.keys_pane){
        int at=pn.focus[slot];
        if(dy<0){pn.keys_pane=false;pn_stand_over(s,at);}
        else if(dx&&!zoomed&&at+dx>=0&&at+dx<count)pn_take(s,at+dx);
        return true;
    }
    int under=zell_panes_pane_at(s,(int)s->x);
    if(under<0)return true;
    if(dy>0)pn_take(s,under);
    else if(dx&&!zoomed&&under+dx>=0&&under+dx<count)pn_stand_over(s,under+dx);
    return true;
}
/* Ctrl+Tab and Ctrl+Shift+Tab, as Kilix's next and previous window: from a
 * pane, the keys go round the panes (a zoomed room shows the one they go
 * to); from the walkway, to the pane below Kilix. */
bool zell_panes_cycle(zell_scene *s,int step){
    if(!ZELL_ROOM_IS_PANES(s->room))return false;
    int slot=pn_slot(s->room),count=zell_panes_count(s->room);
    int to=pn.keys_pane?((pn.focus[slot]+step)%count+count)%count:zell_panes_pane_at(s,(int)s->x);
    if(to<0)to=pn.focus[slot];
    if(pn.zoom[slot]>=0&&pn.zoom[slot]!=to&&!pn_chrome(s,to,ZELL_PANES_ZOOM))return true;
    pn_take(s,to);
    return true;
}
/* Ctrl+Alt+Z, as Kilix's stack layout: the pane with the keys (from the
 * walkway, the one below Kilix) fills the room, or no longer does. */
bool zell_panes_zoom_key(zell_scene *s){
    if(!ZELL_ROOM_IS_PANES(s->room))return false;
    int slot=pn_slot(s->room);
    int index=pn.keys_pane?pn.focus[slot]:zell_panes_pane_at(s,(int)s->x);
    if(index>=0)(void)pn_chrome(s,index,ZELL_PANES_ZOOM);
    return true;
}
/* What the Hotkeys window says: "KEYS|what they do"; "#" heads a part. */
const char *const zell_panes_hotkeys_text[]={
    "#In this room: Zell's keys, as Kilix's are for its windows",
    "Alt+Left / Alt+Right|the pane beside; on the walkway, Kilix steps over it",
    "Alt+Up|from a pane up to the walkway, Kilix over that pane",
    "Alt+Down|from the walkway into the pane below Kilix",
    "Ctrl+Tab / Ctrl+Shift+Tab|the next / previous pane",
    "Ctrl+Alt+Z|zoom the pane, or unzoom it",
    "Left / Right|walk Kilix along the walkway; hold Shift to run",
    "Enter or Space|press what Kilix stands at",
    "Shift+PgUp / PgDn|scroll the pane a page; Shift+Home / End: top, bottom",
    "F1|this window, from the walkway; Esc, Enter or a click closes it",
    "Mouse|click a pane to type in it, the walkway to give Kilix the keys;",
    "|the wheel scrolls; a drag selects and copies",
    "",
    "#Ctrl+Shift: Kilix's own keys, for its tabs and windows",
    "Ctrl+Shift+Left / Right|the previous / next Kilix tab",
    "Ctrl+Shift+PgUp / PgDn|move this tab left / right",
    "Ctrl+Shift+T / Q|a new tab / close this tab",
    "Ctrl+Shift+Enter|split this Kilix window",
    "Ctrl+Shift+N|a new Kilix window on the desktop",
    "Ctrl+Shift+C / V|copy Kilix's selection / paste, into the pane with the keys",
    "Ctrl+Shift+Up / Down|scroll Kilix's own window a line",
    "Ctrl+Shift+R|resize: W / N wider, narrower; T / S taller, shorter; R reset",
    "|(Ctrl doubles the step); Esc leaves it",
    "Ctrl+Shift+Home|Kilix's windows back to their sizes",
    "Ctrl+Shift+D|dictate",
    "Ctrl+Shift+B|Kilix's tmux-style prefix, then a key; Esc leaves it",
    "Ctrl+Shift+W|close this Kilix window, Zell with it",
    "Ctrl+Shift+] / [|Kilix's next / previous window, past Zell",
    "Ctrl+Shift+L|Kilix's next layout",
    "Ctrl+Shift+H|this window's scrollback, in a pager",
    "Ctrl+Shift+= / -|Kilix's font size up / down",
    "Ctrl+Shift+F11|full screen",
    "Ctrl+Shift+E / U|open a link by hints / type a Unicode character",
    "Ctrl+Shift+F2 / F5|edit / reload Kilix's settings",
    "",
    "Alt+arrows, Ctrl+Tab and Ctrl+Alt+Z reach Zell from a Kilix that hands",
    "them to it (0.2.4 on); an older Kilix moves between its own windows.",
    "If Zell is killed in this room, its window goes on handing them over:",
    "printf '\\033]1337;SetUserVar=kilix_app_keys\\007' there gives them back.",
};
const int zell_panes_hotkeys_lines=(int)(sizeof zell_panes_hotkeys_text/sizeof zell_panes_hotkeys_text[0]);
bool zell_panes_press(zell_scene *s){
    int index,which,aim=zell_panes_aim(s,&index,&which);
    if(aim==ZELL_PANES_AIM_HOTKEYS){pn.hotkeys=true;return true;}
    if(aim==ZELL_PANES_AIM_AGENT){pn_start_agent(s,index,which);return true;}
    if(aim==ZELL_PANES_AIM_CHROME){(void)pn_chrome(s,index,which);return true;}
    if(aim==ZELL_PANES_AIM_PAD||aim==ZELL_PANES_AIM_PANE){pn_take(s,index);return true;}
    return false;
}
bool zell_panes_click(zell_scene *s){
    if(!ZELL_ROOM_IS_PANES(s->room)||!s->mouse_click||!s->mouse_valid)return false;
    s->mouse_click=false;
    /* A click anywhere closes the Hotkeys window, and does nothing else. */
    if(pn.hotkeys){pn.hotkeys=false;return true;}
    int x=s->mouse_x,y=s->mouse_y,count=zell_panes_count(s->room);
    if(y<ZELL_PANES_WALK_H){
        zell_rect hotkeys;zell_panes_hotkeys_rect(s,&hotkeys);
        if(pn_inside(hotkeys,x,y)){pn.hotkeys=true;return true;}
        for(int index=0;index<count;index++){
            for(int agent=0;agent<pn.agent_count;agent++){
                zell_rect b;zell_panes_button_rect(s,index,agent,&b);
                if(pn_inside(b,x,y)){pn_start_agent(s,index,agent);return true;}
            }
            zell_rect pad;zell_panes_pad_rect(s,index,&pad);
            if(pn_inside(pad,x,y)){pn_take(s,index);return true;}
        }
        pn.keys_pane=false;
        return true;
    }
    int index=zell_panes_pane_at(s,x);
    if(index<0)return true;
    if(y<ZELL_PANES_CELLS_Y){
        for(int button=ZELL_PANES_ZOOM;button<=ZELL_PANES_END;button++){
            zell_rect b;zell_panes_chrome_button(s,index,button,&b);
            if(pn_inside(b,x,y)){
                if(!pn_chrome(s,index,button))return true;
                break;
            }
        }
    }
    pn_take(s,index);
    return true;
}
/* The thumb's middle follows y. */
static void pn_bar_to(const zell_scene *s,int index,int y){
    zell_rect track,thumb;uint64_t b[3];
    int slot=pn_slot(s->room);
    zell_panes_scrollbar_rect(s,index,&track,&thumb);
    if(!track.w||!zell_panes_scrollback(s->room,index,b))return;
    uint64_t last=b[0]-b[2];
    int usable=track.h-thumb.h,at=y-track.y-thumb.h/2;
    if(at<0)at=0;
    if(at>usable)at=usable;
    uint64_t row=usable>0?(uint64_t)at*last/(uint64_t)usable:last;
    bool ok;
    pn_sendf(ok,"V %d %llu\n",index,(unsigned long long)row);
    if(ok)pn.bar[slot][index][1]=row;
}
static void pn_send_pointer(int index,int action,int button,int mods,int px,int py){
    bool ok;
    pn_sendf(ok,"M %d %d %d %d %d %d\n",index,action,button,mods,px,py);
    (void)ok;
}
bool zell_panes_pointer(zell_scene *s,unsigned buttons,int x,int y,bool release){
    if(!ZELL_ROOM_IS_PANES(s->room))return false;
    /* Under the Hotkeys window the panes get no pointer. */
    if(pn.hotkeys){
        pn.grab=pn.bar_grab=-1;
        if((buttons&64u)&&(buttons&3u)<2)zell_panes_hotkeys_scroll(s,(buttons&3u)?3:-3);
        return true;
    }
    int slot=pn_slot(s->room);
    bool motion=(buttons&32u)!=0,wheel=(buttons&64u)!=0;
    unsigned low=buttons&3u;
    /* As Ghostty numbers them: Shift 1, Ctrl 2, Alt 4; left 1, right 2,
     * middle 3, the wheel 4 to 7, the rest from 8. */
    int mods=((buttons&4u)?1:0)|((buttons&16u)?2:0)|((buttons&8u)?4:0);
    int button=wheel?(int)(4u+low):(buttons&128u)?(int)(8u+low):low==0?1:low==1?3:low==2?2:0;
    int action=release?1:motion?2:0;
    zell_rect pane,track,thumb;
    /* A dragged scrollbar has the pointer until the button goes up. */
    if(pn.bar_grab>=0){
        if((motion&&button)||release){
            int index=pn.bar_grab;
            pn_bar_to(s,index,y);
            if(release){
                /* Let go, the bar stays lit only under the pointer. */
                pn.bar_grab=-1;
                zell_panes_scrollbar_rect(s,index,&track,&thumb);
                pn.bar_hover=pn_inside(track,x,y)?index:-1;
            }
            return true;
        }
        pn.bar_grab=-1;
    }
    /* So does a pane its button went down in: the drag and the release go
     * there, at the nearest pixel of its cells. */
    if(pn.grab>=0&&!wheel&&(release||(motion&&button))){
        int index=pn.grab;
        if(release)pn.grab=-1;
        zell_panes_pane_rect(s,index,&pane);
        if(!pane.w)return true;
        int px=x-pane.x,py=y-pane.y;
        if(px<0)px=0;
        if(px>=pane.w)px=pane.w-1;
        if(py<0)py=0;
        if(py>=pane.h)py=pane.h-1;
        pn_send_pointer(index,action,button,mods,px,py);
        return true;
    }
    if(motion&&!button)pn.grab=-1;
    int index=y>=ZELL_PANES_CELLS_Y?zell_panes_pane_at(s,x):-1;
    if(index>=0){zell_panes_pane_rect(s,index,&pane);if(!pn_inside(pane,x,y))index=-1;}
    if(index<0){pn.bar_hover=-1;return false;}
    zell_panes_scrollbar_rect(s,index,&track,&thumb);
    pn.bar_hover=pn_inside(track,x,y)?index:-1;
    if(pn.bar_hover>=0&&button==1&&!motion&&!release){pn.bar_grab=index;pn_bar_to(s,index,y);return true;}
    int px=x-pane.x,py=y-pane.y;
    if(motion&&!button){
        /* Bare motion only for a program that asked for it, once a cell. */
        if(!(pn.bar_flags[slot][index]&2u))return true;
        if(index==pn.motion_pane&&px/8==pn.motion_col&&py/16==pn.motion_row)return true;
        pn.motion_pane=index;pn.motion_col=px/8;pn.motion_row=py/16;
    }
    if(!wheel&&!motion&&!release)pn.grab=index;
    pn_send_pointer(index,action,button,mods,px,py);
    return true;
}
static void pn_notice(int room,int index,const char *text){
    snprintf(pn.notice,sizeof pn.notice,"%s",text);
    pn.notice_room=room;pn.notice_pane=index;pn.notice_until=zell_notice_clock_ms()+PN_NOTICE_MS;
}
void zell_panes_tell(const zell_scene *s,const char *text,int64_t ms){
    if(!ZELL_ROOM_IS_PANES(s->room))return;
    pn_notice(s->room,pn.focus[pn_slot(s->room)],text);
    pn.notice_until=zell_notice_clock_ms()+ms;
}
bool zell_panes_paste(const zell_scene *s,const char *bytes,size_t count){
    if(!ZELL_ROOM_IS_PANES(s->room)||!pn.keys_pane)return false;
    int slot=pn_slot(s->room),index=pn.focus[slot];
    /* No bytes and a count past the cap: one the input scanner dropped. */
    if(!bytes&&count==ZELL_INPUT_PASTE_UNENDED){
        pn_notice(s->room,index,"The paste never ended and was not sent.");
        return false;
    }
    if(!bytes&&count<=ZELL_PANES_PASTE_MAX)return false;
    if(!count)return true;
    /* One paste at a time; one too long is not cut but refused. */
    if(pn.paste_n&&(pn.paste_room!=s->room||pn.paste_pane!=index)){
        pn_notice(s->room,index,"Another pane's paste is still being sent. Paste again.");
        return false;
    }
    /* Lines made since the last paste still wait behind it: this one
     * would go before them. */
    if(pn.after_n){
        pn_notice(s->room,index,"The last paste is still being sent. Paste again.");
        return false;
    }
    if(!bytes||count>ZELL_PANES_PASTE_MAX||pn.paste_n>ZELL_PANES_PASTE_MAX-count){
        char text[64];
        snprintf(text,sizeof text,"The paste is over %u MiB and was not sent.",ZELL_PANES_PASTE_MAX>>20);
        pn_notice(s->room,index,text);
        return false;
    }
    if(pn.paste_n+count>pn.paste_cap){
        size_t cap=pn.paste_cap?pn.paste_cap:4096;
        while(cap<pn.paste_n+count)cap*=2;
        if(cap>ZELL_PANES_PASTE_MAX)cap=ZELL_PANES_PASTE_MAX;
        char *grown=realloc(pn.paste,cap);
        if(!grown)return false;
        pn.paste=grown;pn.paste_cap=cap;
    }
    memcpy(pn.paste+pn.paste_n,bytes,count);
    pn.paste_n+=count;pn.paste_room=s->room;pn.paste_pane=index;
    return true;
}
/* As much of the paste as the queue takes, PN_PASTE_CHUNK bytes a line, in
 * hex; then the line that ends it. */
static void pn_paste_drain(void){
    static const char hex[]="0123456789abcdef";
    static char line[32+2*PN_PASTE_CHUNK];
    while(pn.paste_n){
        int at=snprintf(line,sizeof line,"P %d %d ",pn.paste_room,pn.paste_pane);
        if(at<0)return;
        if(pn.paste_at==pn.paste_n){
            line[at++]='.';line[at++]='\n';
            if(!pn_send_now(line,(size_t)at))return;
            pn.paste_n=pn.paste_at=0;
            break;
        }
        size_t take=pn.paste_n-pn.paste_at;
        if(take>PN_PASTE_CHUNK)take=PN_PASTE_CHUNK;
        for(size_t i=0;i<take;i++){
            unsigned char ch=(unsigned char)pn.paste[pn.paste_at+i];
            line[at++]=hex[ch>>4];line[at++]=hex[ch&15u];
        }
        line[at++]='\n';
        if(!pn_send_now(line,(size_t)at))return;
        pn.paste_at+=take;
    }
    /* Then what was typed meanwhile, whole when the queue takes it. */
    if(pn.after_n&&pn_send_now(pn.after,pn.after_n))pn.after_n=0;
}
size_t zell_panes_paste_left(void){return pn.paste_n-pn.paste_at+(pn.paste_n?1:0)+pn.after_n;}
void zell_panes_test_drain(void){pn_paste_drain();}
bool zell_panes_scroll_key(const zell_scene *s,uint32_t key,uint32_t modifiers){
    if(!ZELL_ROOM_IS_PANES(s->room)||!pn.keys_pane||(modifiers&~PN_MOD_LOCKS)!=PN_MOD_SHIFT)return false;
    const char *how=key==PN_KEY_PAGE_UP?"up":key==PN_KEY_PAGE_DOWN?"down":key==PN_KEY_HOME?"top":key==PN_KEY_END?"bottom":NULL;
    if(!how)return false;
    bool ok;
    pn_sendf(ok,"W %d %s\n",pn.focus[pn_slot(s->room)],how);
    (void)ok;
    return true;
}
int zell_panes_bar_hover(void){return pn.bar_grab>=0?pn.bar_grab:pn.bar_hover;}
const char *zell_panes_copy_sequence(size_t *length){
    if(!pn.copy_ready||!pn.copy)return NULL;
    *length=pn.copy_n;
    return pn.copy;
}
void zell_panes_copy_written(void){pn.copy_ready=false;pn.copy_n=0;}
static void pn_close_fds(void){
    if(pn.map&&pn.map!=MAP_FAILED){munmap(pn.map,PN_MAP_BYTES);pn.map=NULL;}
    if(pn.frame_fd>=0){close(pn.frame_fd);pn.frame_fd=-1;}
    if(pn.in_fd>=0){close(pn.in_fd);pn.in_fd=-1;}
    if(pn.out_fd>=0){close(pn.out_fd);pn.out_fd=-1;}
}
static void pn_fail(const char *text){
    size_t n=0;
    while(text[n]&&n+1<sizeof pn.error){pn.error[n]=text[n];n++;}
    pn.error[n]=0;
    pn.failed=true;
}
/* Printable ASCII only, at most capacity-1 bytes; false if none. */
static bool pn_copy_text(char *out,size_t capacity,const char *text){
    size_t n=0;
    for(;text[n]&&n+1<capacity;n++){if(text[n]<32||text[n]>126)return false;out[n]=text[n];}
    if(text[n])return false;
    out[n]=0;
    return n>0;
}
/* One line from the helper: E failed, G an agent it can start, T what a
 * pane runs, N a notice about a pane. Anything else is ignored. */
static bool pn_base64(const char *text){
    for(;*text;text++)if(!((*text>='A'&&*text<='Z')||(*text>='a'&&*text<='z')||(*text>='0'&&*text<='9')||*text=='+'||*text=='/'||*text=='='))return false;
    return true;
}
/* Y: a piece of the base64 of text to copy, "Y ." when it is all sent. A
 * bad or too long one is dropped whole. */
static void pn_copy_line(const char *text){
    if(!strcmp(text,".")){
        /* Already ended: another end adds nothing. */
        if(pn.copy_ready)return;
        if(!pn.copy_bad&&pn.copy_n>sizeof PN_COPY_HEAD-1){pn.copy[pn.copy_n++]='\a';pn.copy_ready=true;}
        else pn.copy_n=0;
        pn.copy_bad=false;
        return;
    }
    /* A new copy before main wrote the last: the newer one wins. */
    if(pn.copy_ready){pn.copy_ready=false;pn.copy_n=0;}
    size_t n=strlen(text);
    if(pn.copy_bad)return;
    if(!n||!pn_base64(text)||(pn.copy_n?pn.copy_n:sizeof PN_COPY_HEAD-1)+n>PN_COPY_MAX+sizeof PN_COPY_HEAD-1){pn.copy_bad=true;pn.copy_n=0;return;}
    if(!pn.copy&&!(pn.copy=malloc(PN_COPY_MAX+sizeof PN_COPY_HEAD+1))){pn.copy_bad=true;return;}
    if(!pn.copy_n){memcpy(pn.copy,PN_COPY_HEAD,sizeof PN_COPY_HEAD-1);pn.copy_n=sizeof PN_COPY_HEAD-1;}
    memcpy(pn.copy+pn.copy_n,text,n);pn.copy_n+=n;
}
static void pn_line(const char *line){
    if(line[0]=='E'&&line[1]==' '){pn_fail(line+2);return;}
    if(line[0]=='Y'&&line[1]==' '){pn_copy_line(line+2);return;}
    if(line[0]=='B'&&line[1]==' '){
        int room=0,index=-1,used=0;unsigned long long kept,first,shown;unsigned flags;
        if(sscanf(line+2,"%d %d %llu %llu %llu %u%n",&room,&index,&kept,&first,&shown,&flags,&used)!=6||line[2+used])return;
        if(!ZELL_ROOM_IS_PANES(room)||index<0||index>=zell_panes_count(room)||!shown||first>kept||shown>kept-first||flags>3)return;
        int slot=pn_slot(room);
        pn.bar[slot][index][0]=kept;pn.bar[slot][index][1]=first;pn.bar[slot][index][2]=shown;
        pn.bar_flags[slot][index]=flags;pn.bar_known[slot][index]=true;
        return;
    }
    if(line[0]=='G'&&line[1]==' '){
        const char *id=line+2,*space=strchr(id,' ');
        size_t n=space?(size_t)(space-id):0;
        pn_agent agent;
        if(!n||n>=sizeof agent.id||pn.agent_count>=ZELL_PANES_AGENTS)return;
        for(size_t i=0;i<n;i++)if(!((id[i]>='a'&&id[i]<='z')||(id[i]>='0'&&id[i]<='9')||id[i]=='-'||id[i]=='_'))return;
        memcpy(agent.id,id,n);agent.id[n]=0;
        if(!pn_copy_text(agent.label,sizeof agent.label,space+1))return;
        for(int i=0;i<pn.agent_count;i++)if(!strcmp(pn.agents[i].id,agent.id))return;
        pn.agents[pn.agent_count++]=agent;
        return;
    }
    if((line[0]=='T'||line[0]=='N')&&line[1]==' '){
        int room=0,index=-1,used=0;
        if(sscanf(line+2,"%d %d %n",&room,&index,&used)<2||used<=0)return;
        if(!ZELL_ROOM_IS_PANES(room)||index<0||index>=zell_panes_count(room))return;
        if(line[0]=='T'){
            char title[32];
            if(pn_copy_text(title,sizeof title,line+2+used))memcpy(pn.title[pn_slot(room)][index],title,sizeof title);
        }else if(pn_copy_text(pn.notice,sizeof pn.notice,line+2+used)){
            pn.notice_room=room;pn.notice_pane=index;pn.notice_until=zell_notice_clock_ms()+PN_NOTICE_MS;
        }
    }
}
/* What the helper said goes with it: titles, notices and its agents. */
static void pn_forget_helper(void){
    memset(pn.title,0,sizeof pn.title);
    pn.agent_count=0;pn.notice[0]=0;
    memset(pn.bar_known,0,sizeof pn.bar_known);
    pn.paste_n=pn.paste_at=0;pn.after_n=0;pn.copy_n=0;pn.copy_ready=pn.copy_bad=false;
    pn.grab=pn.bar_grab=pn.bar_hover=pn.motion_pane=-1;
}
static bool pn_start(void){
    char script[4096],fd_text[24];
    int to_child[2],from_child[2];
    if(pn.pid>0)return true;
    if(!th_script(script,sizeof script,"zell_panes.py")){pn_fail("The panes need tools/zell_panes.py beside Zell.");return false;}
    if(pipe(to_child)!=0){pn_fail("Could not start the panes.");return false;}
    if(pipe(from_child)!=0){close(to_child[0]);close(to_child[1]);pn_fail("Could not start the panes.");return false;}
    (void)pn_grow_pipe(from_child[0]);
    pn.frame_fd=(int)syscall(SYS_memfd_create,"zell-panes",0);
    if(pn.frame_fd<0||ftruncate(pn.frame_fd,(off_t)PN_MAP_BYTES)!=0){
        close(to_child[0]);close(to_child[1]);close(from_child[0]);close(from_child[1]);
        pn_close_fds();pn_fail("Could not make the panes' picture.");return false;
    }
    pn.map=mmap(NULL,PN_MAP_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,pn.frame_fd,0);
    if(pn.map==MAP_FAILED){
        pn.map=NULL;
        close(to_child[0]);close(to_child[1]);close(from_child[0]);close(from_child[1]);
        pn_close_fds();pn_fail("Could not map the panes' picture.");return false;
    }
    memset(pn.map,0,64);
    pn.pid=fork();
    if(pn.pid==0){
        if(setsid()<0)_exit(127);
        if(dup2(to_child[0],STDIN_FILENO)<0||dup2(from_child[1],STDOUT_FILENO)<0)_exit(127);
        int nullfd=open("/dev/null",O_WRONLY);
        if(nullfd>=0){dup2(nullfd,STDERR_FILENO);if(nullfd>2)close(nullfd);}
        /* The map is fd 3 and nothing else of Zell's is open: the helper's
         * shells live long and must not hold Zell's other pipes. */
        if(pn.frame_fd!=3&&dup2(pn.frame_fd,3)<0)_exit(127);
        syscall(SYS_close_range,4u,~0u,0);
        snprintf(fd_text,sizeof fd_text,"%d",3);
        setenv("ZELL_PANES_FD",fd_text,1);
        signal(SIGINT,SIG_DFL);signal(SIGTERM,SIG_DFL);signal(SIGPIPE,SIG_DFL);signal(SIGHUP,SIG_DFL);signal(SIGXFSZ,SIG_DFL);
        execlp("python3","python3",script,(char *)NULL);
        _exit(127);
    }
    close(to_child[0]);close(from_child[1]);
    if(pn.pid<0){
        pn.pid=0;close(to_child[1]);close(from_child[0]);
        pn_close_fds();pn_fail("Could not start the panes.");return false;
    }
    pn.in_fd=to_child[1];pn.out_fd=from_child[0];
    fcntl(pn.frame_fd,F_SETFD,FD_CLOEXEC);
    fcntl(pn.in_fd,F_SETFL,fcntl(pn.in_fd,F_GETFL)|O_NONBLOCK);
    fcntl(pn.out_fd,F_SETFL,fcntl(pn.out_fd,F_GETFL)|O_NONBLOCK);
    fcntl(pn.in_fd,F_SETFD,FD_CLOEXEC);
    fcntl(pn.out_fd,F_SETFD,FD_CLOEXEC);
    pn.seq=0;pn.frame_w=pn.frame_h=pn.frame_room=0;pn.line_n=0;pn.line_over=false;
    pn_forget_helper();
    /* A new helper has no rooms yet: the next pump sends the shown one. */
    pn.sent_room=-1;pn.sent_cols=pn.sent_rows=0;
    return true;
}
static void pn_flush(void){
    while(pn.queued>0&&pn.in_fd>=0){
        ssize_t wrote=write(pn.in_fd,pn.queue,pn.queued);
        if(wrote<0){
            if(errno==EINTR)continue;
            if(errno!=EAGAIN&&errno!=EWOULDBLOCK)pn_fail("The panes' pipe closed.");
            return;
        }
        memmove(pn.queue,pn.queue+wrote,pn.queued-(size_t)wrote);
        pn.queued-=(size_t)wrote;pn.queue[pn.queued]=0;
    }
}
static void pn_read(void){
    static char bytes[65536];
    if(pn.out_fd<0)return;
    for(size_t taken=0;taken<PN_READ_MAX;){
        ssize_t count=read(pn.out_fd,bytes,sizeof bytes);
        if(count<0&&errno==EINTR)continue;
        if(count<=0)return;
        taken+=(size_t)count;
        for(ssize_t i=0;i<count;i++){
            if(bytes[i]!='\n'){
                if(pn.line_n+1<sizeof pn.line)pn.line[pn.line_n++]=bytes[i];
                else pn.line_over=true;
                continue;
            }
            pn.line[pn.line_n]=0;
            /* A line too long for the buffer is not taken cut short. */
            if(!pn.line_over)pn_line(pn.line);
            pn.line_n=0;pn.line_over=false;
        }
    }
}
static bool pn_ensure(size_t pixels){
    if(pn.cap>=pixels&&pn.shown&&pn.scratch)return true;
    uint32_t *a=calloc(pixels,4),*b=calloc(pixels,4);
    if(!a||!b){free(a);free(b);return false;}
    free(pn.shown);free(pn.scratch);
    pn.shown=a;pn.scratch=b;pn.cap=pixels;
    return true;
}
/* Sequence first, then the header and pixels, then the sequence again: a
 * frame is taken only if no write began or ended in between. */
static void pn_copy(void){
    const unsigned char *p=pn.map;
    if(!p)return;
    uint32_t seq=th_u32(p+16);
    atomic_thread_fence(memory_order_acquire);
    if(!seq||(seq&1u)||seq==pn.seq)return;
    if(memcmp(p,"ZPNE",4)!=0||th_u32(p+4)!=1)return;
    uint32_t w=th_u32(p+8),h=th_u32(p+12),room=th_u32(p+20);
    if(w<1||h<1||w>PN_MAX_W||h>PN_MAX_H||!zell_panes_count((int)room))return;
    if(!pn_ensure((size_t)w*h))return;
    memcpy(pn.scratch,p+64,(size_t)w*h*4u);
    atomic_thread_fence(memory_order_acquire);
    if(th_u32(p+16)!=seq)return;
    uint32_t *swap=pn.shown;pn.shown=pn.scratch;pn.scratch=swap;
    pn.seq=seq;pn.frame_w=(int)w;pn.frame_h=(int)h;pn.frame_room=(int)room;
}
static void pn_reap(void){
    int status=0;
    if(pn.pid<=0)return;
    if(waitpid(pn.pid,&status,WNOHANG)!=pn.pid)return;
    /* Its last picture goes too, so the room says what happened. */
    pn.pid=0;pn_close_fds();pn.queued=0;pn.frame_room=0;pn.seq=0;
    pn_forget_helper();
    if(!pn.failed)pn_fail("The panes stopped. Walk out and in again to start new shells.");
}
/* The line that shows a room (0 hides), with the focus and zoom Zell keeps
 * for it, which a new helper does not know. */
int zell_panes_room_line(int room,int cols,int rows,char *out,size_t capacity){
    int slot=pn_slot(room);
    if(slot<0)return snprintf(out,capacity,"H\n");
    /* Zoom before focus: a zoom the helper has that Zell does not (a line
     * that reached the room shown before) is undone first, so the focus,
     * which the helper refuses for a pane hidden by a zoom, is taken. */
    return snprintf(out,capacity,"S %d %d %d\nZ %d\nF %d\n",room,cols,rows,pn.zoom[slot],pn.focus[slot]);
}
/* Linux grows a pipe to its pipe-max-size, 1 MiB by default; elsewhere,
 * or refused, the pipe stays as it is. The size it has is returned. */
static int pn_grow_pipe(int fd){
#if defined(__linux__)
#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#define F_GETPIPE_SZ 1032
#endif
    for(unsigned bytes=PN_PIPE_BYTES;bytes>=(64u<<10);bytes/=2)if(fcntl(fd,F_SETPIPE_SZ,(int)bytes)>=0)break;
    return fcntl(fd,F_GETPIPE_SZ);
#else
    (void)fd;return 0;
#endif
}
static void pn_room_send(zell_scene *s);
void zell_panes_pump(zell_scene *s){
    bool shown=ZELL_ROOM_IS_PANES(s->room);
    /* A failed helper is tried again on the next walk in. */
    if(!shown&&pn.pid<=0)pn.failed=false;
    if(shown&&pn.pid<=0&&!pn.failed){pn.queued=0;pn_start();}
    if(pn.pid<=0)return;
    pn_read();
    pn_room_send(s);
    pn_paste_drain();
    pn_flush();
    pn_copy();
    pn_reap();
}
/* The helper is told the room shown and its size once each changes; a line
 * not queued (the queue or the lines waiting behind a paste are full) is
 * made again next time. */
static void pn_room_send(zell_scene *s){
    bool shown=ZELL_ROOM_IS_PANES(s->room);
    int room=shown?s->room:0,cols=0,rows=0;
    if(shown)zell_panes_cells(s->width,s->height,&cols,&rows);
    if(room!=pn.sent_room||cols!=pn.sent_cols||rows!=pn.sent_rows){
        char line[80];
        int n=zell_panes_room_line(room,cols,rows,line,sizeof line);
        /* After the lines already queued, which may be half written and
         * belong to the room he was in; until it fits it is tried again. */
        if(n>0&&(size_t)n<sizeof line&&pn.queued+(size_t)n<sizeof pn.queue&&pn_send(line,(size_t)n)){
            pn.sent_room=room;pn.sent_cols=cols;pn.sent_rows=rows;
        }
    }
}
const uint32_t *zell_panes_frame(int room,int *width,int *height){
    if(!pn.shown||pn.frame_room!=room||pn.frame_w<1||pn.frame_h<1)return NULL;
    *width=pn.frame_w;*height=pn.frame_h;
    return pn.shown;
}
const char *zell_panes_error(void){return pn.failed?pn.error:"";}
void zell_panes_shutdown(void){
    struct timespec delay={.tv_nsec=10000000};
    if(pn.pid>0){
        if(pn.in_fd>=0){ssize_t wrote=write(pn.in_fd,"Q\n",2);(void)wrote;}
        for(int64_t deadline=zell_notice_clock_ms()+700;pn.pid>0&&zell_notice_clock_ms()<deadline;){
            pid_t ended=waitpid(pn.pid,NULL,WNOHANG);
            if(ended==pn.pid||(ended<0&&errno==ECHILD))pn.pid=0;
            else nanosleep(&delay,NULL);
        }
        if(pn.pid>0){
            if(kill(-pn.pid,SIGTERM)<0)kill(pn.pid,SIGTERM);
            for(int64_t deadline=zell_notice_clock_ms()+500;pn.pid>0&&zell_notice_clock_ms()<deadline;){
                pid_t ended=waitpid(pn.pid,NULL,WNOHANG);
                if(ended==pn.pid||(ended<0&&errno==ECHILD))pn.pid=0;
                else nanosleep(&delay,NULL);
            }
        }
        if(pn.pid>0){
            if(kill(-pn.pid,SIGKILL)<0)kill(pn.pid,SIGKILL);
            while(waitpid(pn.pid,NULL,0)<0&&errno==EINTR){}
            pn.pid=0;
        }
    }
    pn_close_fds();
    free(pn.shown);free(pn.scratch);pn.shown=pn.scratch=NULL;pn.cap=0;
    free(pn.paste);pn.paste=NULL;pn.paste_n=pn.paste_at=pn.paste_cap=0;
    free(pn.copy);pn.copy=NULL;pn.copy_n=0;pn.copy_ready=false;
}
/* Tests hand the copy a map of their own (from mmap, PN_MAP_BYTES long, so
 * the reset can unmap it), feed helper lines and fail the helper without
 * starting one. */
void zell_panes_test_copy(void *map){if(!pn.map)pn.map=map;pn_copy();}
void zell_panes_test_line(const char *line){pn_line(line);}
void zell_panes_test_room(zell_scene *s){pn_room_send(s);}
int zell_panes_test_grow_pipe(int fd){return pn_grow_pipe(fd);}
/* Lines from a descriptor of the test's, as from the helper's pipe. */
void zell_panes_test_read(int fd){int old=pn.out_fd;pn.out_fd=fd;pn_read();pn.out_fd=old;}
void zell_panes_test_fail(const char *text){pn_fail(text);pn.frame_room=0;}
size_t zell_panes_map_bytes(void){return PN_MAP_BYTES;}
void zell_panes_test_reset(void){
    zell_panes_shutdown();
    pn=(pn_state)PN_START;
}
