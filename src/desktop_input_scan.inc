/* What the outer terminal sends Zell, split three ways: SGR pointer reports
 * (CSI < b ; x ; y M or m), bracketed pastes (CSI 200 ~ ... CSI 201 ~), and
 * everything else, which is keys. A pointer report or paste marker cut
 * between two reads is held until the rest comes; an escape held longer
 * than ZELL_INPUT_ESCAPE_MS goes on as keys. */
static const char zi_paste_end[]="\033[201~";

static void zi_keys(const zell_input_sinks *sinks,const char *bytes,size_t count){
    if(count&&sinks->keys)sinks->keys(sinks->user,bytes,count);
}
static void zi_paste_byte(zell_input *in,char ch){
    if(in->paste_over)return;
    if(in->paste_n==in->paste_cap){
        size_t cap=in->paste_cap?in->paste_cap*2:4096;
        if(cap>ZELL_INPUT_PASTE_MAX)cap=ZELL_INPUT_PASTE_MAX;
        char *grown=cap>in->paste_cap?realloc(in->paste,cap):NULL;
        if(!grown){in->paste_over=true;return;}
        in->paste=grown;in->paste_cap=cap;
    }
    in->paste[in->paste_n++]=ch;
}
/* Inside a paste: everything up to the end marker is the paste's. A paste
 * longer than ZELL_INPUT_PASTE_MAX is dropped whole, never cut. */
static void zi_pasting(zell_input *in,const zell_input_sinks *sinks,char ch){
    if(ch==zi_paste_end[in->end_match]){
        if(++in->end_match<sizeof zi_paste_end-1)return;
        in->pasting=false;in->end_match=0;
        if(!in->paste_over&&sinks->paste)sinks->paste(sinks->user,in->paste,in->paste_n);
        in->paste_n=0;in->paste_over=false;
        return;
    }
    /* A partial marker was text after all. */
    for(size_t i=0;i<in->end_match;i++)zi_paste_byte(in,zi_paste_end[i]);
    in->end_match=0;
    if(ch==zi_paste_end[0]){in->end_match=1;return;}
    zi_paste_byte(in,ch);
}
static void zi_pointer(zell_input *in,const zell_input_sinks *sinks){
    unsigned buttons,x,y;char end;
    in->pending[in->used]=0;
    if(sscanf(in->pending,"\033[<%u;%u;%u%c",&buttons,&x,&y,&end)==4&&x>0&&y>0&&x<100000&&y<100000&&buttons<1024&&sinks->pointer)
        sinks->pointer(sinks->user,buttons,(int)x,(int)y,end=='m');
}
void zell_input_feed(zell_input *in,const zell_input_sinks *sinks,const char *bytes,size_t count,int64_t now){
    for(size_t i=0;i<count;i++){
        char ch=bytes[i];
        if(in->pasting){zi_pasting(in,sinks,ch);continue;}
        if(!in->used&&ch!='\033'){
            /* A run of plain keys goes on together. */
            size_t j=i;
            while(j<count&&bytes[j]!='\033')j++;
            zi_keys(sinks,bytes+i,j-i);
            i=j-1;
            continue;
        }
        in->pending[in->used++]=ch;in->changed=now;
        size_t n=in->used;const char *p=in->pending;
        bool pointer=n>=3&&p[2]=='<',marker=n>=3&&p[2]=='2';
        if((n==2&&p[1]!='[')||(n==3&&!pointer&&!marker)){zi_keys(sinks,p,n);in->used=0;continue;}
        if(pointer&&n>=4&&(ch=='M'||ch=='m')){zi_pointer(in,sinks);in->used=0;continue;}
        /* At its final byte: the paste marker, or another key that starts
         * CSI 2 (Insert, F9 to F12, CSI 2... u). */
        if(marker&&n>3&&ch>=0x40&&ch<=0x7e){
            if(n==6&&!memcmp(p,"\033[200~",6)){in->used=0;in->pasting=true;in->paste_n=0;in->paste_over=false;in->end_match=0;continue;}
            zi_keys(sinks,p,n);in->used=0;continue;
        }
        if(n>=sizeof in->pending-1)in->used=0;
    }
}
void zell_input_idle(zell_input *in,const zell_input_sinks *sinks,int64_t now){
    if(in->used&&!in->pasting&&now-in->changed>ZELL_INPUT_ESCAPE_MS){
        /* Keys that stopped short go on as they are; half a pointer
         * report is dropped. */
        if(in->used<3||in->pending[2]!='<')zi_keys(sinks,in->pending,in->used);
        in->used=0;
    }
}
bool zell_input_pasting(const zell_input *in){return in->pasting;}
void zell_input_free(zell_input *in){free(in->paste);*in=(zell_input){0};}
