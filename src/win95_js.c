/**
 * @file win95_js.c
 * @brief Minimal JavaScript interpreter — recursive descent, single-pass eval.
 *        Covers the subset needed for static/early-dynamic web pages:
 *        primitives, arithmetic, string ops, if/else, for/while, functions,
 *        document.write/writeln, document.title, alert, window.location,
 *        Math, String methods, typeof, parseInt, parseFloat.
 */
#include "win95_js.h"
#include "win95_html.h"
#include "tal_api.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* -------------------------------------------------------------------------
 * Limits
 * ---------------------------------------------------------------------- */
#define NV   64    /* max variables          */
#define NF   24    /* max functions          */
#define NM   40    /* name/identifier max    */
#define NS   512   /* string value max chars */
#define NIT  4000  /* max loop iterations    */
#define NCD  8     /* max call depth         */

/* -------------------------------------------------------------------------
 * Value types
 * ---------------------------------------------------------------------- */
typedef enum { VT_U=0, VT_N, VT_B, VT_D, VT_S, VT_FN, VT_ARR, VT_OBJ } VT;
/* U=undefined, N=null, B=bool, D=number(double), S=string, FN=function,
   ARR=array (d=pool index), OBJ=object (d=pool index) */

typedef struct { VT t; double d; char s[NS]; } JV;

/* ---- compact item for array/object pools (shorter string) ---- */
#define JVI_SMAX 48
typedef struct { uint8_t t; double d; char s[JVI_SMAX]; } JVI;

#define JA_N  8   /* max simultaneous arrays  */
#define JA_M  24  /* max items per array      */
typedef struct { JVI items[JA_M]; unsigned len; } JA;

#define JO_N  8   /* max simultaneous objects */
#define JO_M  12  /* max properties           */
typedef struct { char keys[JO_M][NM]; JVI vals[JO_M]; unsigned len; } JO;

static JA     s_arr_pool[JA_N];
static JO     s_obj_pool[JO_N];
static uint8_t s_arr_used[JA_N];
static uint8_t s_obj_used[JO_N];

static int  pool_arr_alloc(void) { for(int i=0;i<JA_N;i++) if(!s_arr_used[i]){s_arr_used[i]=1;s_arr_pool[i].len=0;return i;} return 0; }
static void pool_arr_free(int i)  { if(i>=0&&i<JA_N) s_arr_used[i]=0; }
static int  pool_obj_alloc(void)  { for(int i=0;i<JO_N;i++) if(!s_obj_used[i]){s_obj_used[i]=1;s_obj_pool[i].len=0;return i;} return 0; }
static void pool_obj_free(int i)  { if(i>=0&&i<JO_N) s_obj_used[i]=0; }

static JVI jvi_from_jv(const JV*v) {
    JVI r; r.t=(uint8_t)v->t; r.d=v->d;
    if(v->t==VT_S||v->t==VT_FN){ strncpy(r.s,v->s,JVI_SMAX-1); r.s[JVI_SMAX-1]=0; }
    else { r.s[0]=0; }
    return r;
}
static JV jv_from_jvi(const JVI*v) {
    JV r; r.t=(VT)v->t; r.d=v->d; r.s[0]=0;
    if(v->t==VT_S||v->t==VT_FN){ strncpy(r.s,v->s,NS-1); r.s[NS-1]=0; }
    return r;
}
static JV jvarr(int idx) { JV v; v.t=VT_ARR; v.d=(double)idx; v.s[0]=0; return v; }
static JV jvobj(int idx) { JV v; v.t=VT_OBJ; v.d=(double)idx; v.s[0]=0; return v; }

/* ---- value constructors ---- */
static JV jvu(void)         { JV v; v.t=VT_U; v.d=0; v.s[0]=0; return v; }
static JV jvn(void)         { JV v; v.t=VT_N; v.d=0; v.s[0]=0; return v; }
static JV jvb(int b)        { JV v; v.t=VT_B; v.d=b?1.0:0.0; v.s[0]=0; return v; }
static JV jvd(double d)     { JV v; v.t=VT_D; v.d=d; v.s[0]=0; return v; }
static JV jvs(const char*s) { JV v; v.t=VT_S; v.d=0; strncpy(v.s,s,NS-1); v.s[NS-1]=0; return v; }
static JV jvfn(const char*n){ JV v; v.t=VT_FN; v.d=0; strncpy(v.s,n,NS-1); v.s[NS-1]=0; return v; }

static int is_dom_ref(const JV *v)
{
    return v && v->t == VT_S && !strncmp(v->s, "[dom:", 5);
}

static int dom_ref_id(const JV *v, char *out, unsigned cap)
{
    if (!is_dom_ref(v) || !out || cap < 2) return 0;
    const char *p = v->s + 5;
    unsigned n = 0;
    while (*p && *p != ']' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    return n > 0;
}

static JV jvdom(const char *id)
{
    char buf[NS];
    snprintf(buf, sizeof(buf), "[dom:%s]", id ? id : "");
    return jvs(buf);
}

/* ---- truthiness & coercion ---- */
static int jvtruthy(const JV*v) {
    if (v->t==VT_U||v->t==VT_N) return 0;
    if (v->t==VT_B||v->t==VT_D) return v->d!=0.0;
    if (v->t==VT_FN) return 1;
    return v->s[0]!=0 && !(v->s[0]=='0'&&v->s[1]==0);
}
static double jvtod(const JV*v) {
    if (v->t==VT_D||v->t==VT_B) return v->d;
    if (v->t==VT_S) return atof(v->s);
    return 0.0;
}
static void jvtos(const JV*v, char*out, unsigned cap) {
    if (!cap) return;
    switch (v->t) {
    case VT_U:  snprintf(out,cap,"undefined"); break;
    case VT_N:  snprintf(out,cap,"null"); break;
    case VT_B:  snprintf(out,cap,"%s",v->d?"true":"false"); break;
    case VT_D:  if (v->d==(long long)v->d) snprintf(out,cap,"%lld",(long long)v->d);
                else snprintf(out,cap,"%g",v->d); break;
    case VT_S:
    case VT_FN: snprintf(out,cap,"%s",v->s); break;
    case VT_ARR: {
        int idx=(int)v->d; JA*a=(idx>=0&&idx<JA_N)?&s_arr_pool[idx]:NULL;
        if (!a||!a->len){snprintf(out,cap,"");break;}
        unsigned pos=0;
        for (unsigned i=0;i<a->len&&pos+2<cap;i++) {
            JV tmp=jv_from_jvi(&a->items[i]);
            char tb[JVI_SMAX+4]; jvtos(&tmp,tb,sizeof(tb));
            unsigned tl=(unsigned)strlen(tb);
            if (pos+tl+2>=cap) tl=cap-pos-2;
            memcpy(out+pos,tb,tl); pos+=tl;
            if (i+1<a->len&&pos+2<cap) { out[pos++]=','; }
        }
        out[pos]=0; break;
    }
    case VT_OBJ: snprintf(out,cap,"[object Object]"); break;
    }
}

/* -------------------------------------------------------------------------
 * Function record
 * ---------------------------------------------------------------------- */
typedef struct {
    char     name[NM];
    unsigned bs;        /* source offset of '{' */
    char     pn[8][NM]; /* param names */
    unsigned np;
} JFN;

/* -------------------------------------------------------------------------
 * Interpreter state
 * ---------------------------------------------------------------------- */
typedef struct {
    const char *src;
    unsigned    len, pos;
    char vn[NV][NM]; JV vv[NV]; unsigned nv;
    JFN fn[NF]; unsigned nf;
    unsigned call_depth;
    int ret_f, brk_f, cont_f;
    JV  ret_v;
    unsigned anon_cnt;
    WIN95_JS_RESULT_T *res;
} JSX;

/* -------------------------------------------------------------------------
 * Scanner helpers
 * ---------------------------------------------------------------------- */
static int  jsx_end(JSX*x)  { return x->pos>=x->len; }
static char jsx_p(JSX*x)    { return jsx_end(x)?0:x->src[x->pos]; }
static char jsx_p2(JSX*x)   { return (x->pos+1<x->len)?x->src[x->pos+1]:0; }
static char jsx_eat(JSX*x)  { return jsx_end(x)?0:x->src[x->pos++]; }

static void jsx_skip_line_comment(JSX*x) { while (!jsx_end(x)&&jsx_p(x)!='\n') jsx_eat(x); }
static void jsx_skip_block_comment(JSX*x) {
    jsx_eat(x); jsx_eat(x); /* consume '/' '*' */
    while (!jsx_end(x)) {
        if (jsx_p(x)=='*'&&jsx_p2(x)=='/') { jsx_eat(x); jsx_eat(x); return; }
        jsx_eat(x);
    }
}
static void jsx_ws(JSX*x) {
    while (!jsx_end(x)) {
        char c=jsx_p(x);
        if (c==' '||c=='\t'||c=='\n'||c=='\r') { jsx_eat(x); continue; }
        if (c=='/'&&jsx_p2(x)=='/') { jsx_skip_line_comment(x); continue; }
        if (c=='/'&&jsx_p2(x)=='*') { jsx_skip_block_comment(x); continue; }
        break;
    }
}
static int  is_id_s(char c) { return isalpha((unsigned char)c)||c=='_'||c=='$'; }
static int  is_id(char c)   { return isalnum((unsigned char)c)||c=='_'||c=='$'; }

static unsigned read_id(JSX*x, char*buf, unsigned cap) {
    unsigned n=0;
    while (!jsx_end(x)&&is_id(jsx_p(x))&&n+1<cap) buf[n++]=jsx_eat(x);
    buf[n]=0; return n;
}
static void skip_str_body(JSX*x, char q) {
    while (!jsx_end(x)) {
        char c=jsx_eat(x);
        if (c=='\\') { if (!jsx_end(x)) jsx_eat(x); continue; }
        if (c==q) return;
    }
}
static unsigned read_str(JSX*x, char*buf, unsigned cap) {
    char q=jsx_eat(x); unsigned n=0;
    while (!jsx_end(x)) {
        char c=jsx_eat(x);
        if (c==q) break;
        if (c=='\\') {
            if (jsx_end(x)) break;
            char e=jsx_eat(x), out=e;
            if (e=='n') out='\n'; else if (e=='t') out='\t';
            else if (e=='r') out='\r'; else if (e=='\\') out='\\';
            if (n+1<cap) buf[n++]=out; continue;
        }
        if (n+1<cap) buf[n++]=c;
    }
    buf[n]=0; return n;
}
/* Skip balanced { } handling strings and comments */
static void skip_block(JSX*x) {
    jsx_ws(x);
    if (jsx_p(x)=='{') {
        jsx_eat(x); int d=1;
        while (!jsx_end(x)&&d>0) {
            char c=jsx_p(x);
            if (c=='"'||c=='\''||c=='`') { jsx_eat(x); skip_str_body(x,c); continue; }
            if (c=='/'&&jsx_p2(x)=='/') { jsx_skip_line_comment(x); continue; }
            if (c=='/'&&jsx_p2(x)=='*') { jsx_skip_block_comment(x); continue; }
            jsx_eat(x);
            if (c=='{') d++; else if (c=='}') d--;
        }
    } else { /* single stmt */
        int d=0;
        while (!jsx_end(x)) {
            char c=jsx_p(x);
            if (c=='"'||c=='\''||c=='`') { jsx_eat(x); skip_str_body(x,c); continue; }
            if ((c=='('||c=='[')) { d++; jsx_eat(x); continue; }
            if ((c==')'||c==']')&&d>0) { d--; jsx_eat(x); continue; }
            if (c==';'&&d==0) { jsx_eat(x); return; }
            if ((c=='\n'||c=='}')&&d==0) return;
            jsx_eat(x);
        }
    }
}

/* -------------------------------------------------------------------------
 * Variable management
 * ---------------------------------------------------------------------- */
static JV* var_ptr(JSX*x, const char*n) {
    for (unsigned i=0;i<x->nv;i++) if (!strcmp(x->vn[i],n)) return &x->vv[i];
    return NULL;
}
static JV  var_get(JSX*x, const char*n) { JV*p=var_ptr(x,n); return p?*p:jvu(); }
static void var_set(JSX*x, const char*n, JV v) {
    JV*p=var_ptr(x,n);
    if (p) { *p=v; return; }
    if (x->nv<NV) { strncpy(x->vn[x->nv],n,NM-1); x->vn[x->nv][NM-1]=0; x->vv[x->nv]=v; x->nv++; }
}

/* -------------------------------------------------------------------------
 * JS write helpers
 * ---------------------------------------------------------------------- */
static void js_write(JSX*x, const char*s) {
    if (!x->res||!s) return;
    if (!x->res->write_buf) {
        x->res->write_buf=(char*)tal_malloc(JS_WBUF_MAX);
        if (!x->res->write_buf) return;
        x->res->write_buf[0]=0; x->res->write_len=0;
    }
    unsigned sl=(unsigned)strlen(s);
    if (x->res->write_len+sl>=JS_WBUF_MAX) sl=JS_WBUF_MAX-x->res->write_len-1;
    if (!sl) return;
    memcpy(x->res->write_buf+x->res->write_len,s,sl);
    x->res->write_len+=sl;
    x->res->write_buf[x->res->write_len]=0;
}
static void js_write_val(JSX*x, const JV*v) { char t[NS]; jvtos(v,t,sizeof(t)); js_write(x,t); }

/* -------------------------------------------------------------------------
 * Built-in Math
 * ---------------------------------------------------------------------- */
static JV bmath(const char*m, JV*a, unsigned n) {
    double v1=n>0?jvtod(&a[0]):0.0, v2=n>1?jvtod(&a[1]):0.0;
    if (!strcmp(m,"floor")) { double r=(double)(long long)v1; if(v1<0&&r>v1) r-=1.0; return jvd(r); }
    if (!strcmp(m,"ceil"))  { double r=(double)(long long)v1; if(v1>0&&r<v1) r+=1.0; return jvd(r); }
    if (!strcmp(m,"round")) { return jvd((double)(long long)(v1>=0?v1+0.5:v1-0.5)); }
    if (!strcmp(m,"abs"))   return jvd(v1<0?-v1:v1);
    if (!strcmp(m,"max"))   return jvd(v1>v2?v1:v2);
    if (!strcmp(m,"min"))   return jvd(v1<v2?v1:v2);
    if (!strcmp(m,"pow"))   { double r=1.0; long long e=(long long)v2; if(e>=0){for(long long i=0;i<e;i++) r*=v1;} return jvd(r); }
    if (!strcmp(m,"random"))return jvd((double)(rand()&0x7FFF)/32767.0);
    if (!strcmp(m,"sqrt"))  { if(v1<=0) return jvd(0); double r=v1/2; for(int i=0;i<20;i++) r=(r+v1/r)/2; return jvd(r); }
    if (!strcmp(m,"PI"))    return jvd(3.14159265358979);
    return jvd(0.0);
}

/* -------------------------------------------------------------------------
 * String methods
 * ---------------------------------------------------------------------- */
static JV bstr(const JV*self, const char*m, JV*a, unsigned n) {
    char t[NS]; jvtos(self,t,sizeof(t)); unsigned sl=(unsigned)strlen(t);
    if (!strcmp(m,"length"))        return jvd((double)sl);
    if (!strcmp(m,"toString")||!strcmp(m,"valueOf")) return *self;
    if (!strcmp(m,"toLowerCase")||!strcmp(m,"toLocaleLowerCase")) {
        for(unsigned i=0;i<sl;i++) if(t[i]>='A'&&t[i]<='Z') t[i]+=32; return jvs(t);
    }
    if (!strcmp(m,"toUpperCase")||!strcmp(m,"toLocaleUpperCase")) {
        for(unsigned i=0;i<sl;i++) if(t[i]>='a'&&t[i]<='z') t[i]-=32; return jvs(t);
    }
    if (!strcmp(m,"trim")) {
        unsigned s0=0,e=sl;
        while(s0<e&&(t[s0]==' '||t[s0]=='\t'||t[s0]=='\n')) s0++;
        while(e>s0&&(t[e-1]==' '||t[e-1]=='\t'||t[e-1]=='\n')) e--;
        t[e]=0; return jvs(t+s0);
    }
    if (!strcmp(m,"charAt")) { unsigned i=n>0?(unsigned)jvtod(&a[0]):0; char c[2]={0,0}; if(i<sl)c[0]=t[i]; return jvs(c); }
    if (!strcmp(m,"charCodeAt")) { unsigned i=n>0?(unsigned)jvtod(&a[0]):0; return jvd(i<sl?(double)(unsigned char)t[i]:0.0); }
    if (!strcmp(m,"indexOf")) {
        if (!n) return jvd(-1.0); char nd[NS]; jvtos(&a[0],nd,sizeof(nd));
        char*p=strstr(t,nd); return jvd(p?(double)(p-t):-1.0);
    }
    if (!strcmp(m,"lastIndexOf")) {
        if (!n) return jvd(-1.0); char nd[NS]; jvtos(&a[0],nd,sizeof(nd));
        unsigned nl=(unsigned)strlen(nd); int last=-1;
        for(unsigned i=0;i+nl<=sl;i++) if(!strncmp(t+i,nd,nl)) last=(int)i;
        return jvd((double)last);
    }
    if (!strcmp(m,"substring")||!strcmp(m,"substr")) {
        unsigned si=n>0?(unsigned)jvtod(&a[0]):0; unsigned ln=n>1?(unsigned)jvtod(&a[1]):sl;
        if(si>sl) si=sl; if(si+ln>sl) ln=sl-si;
        char out[NS]; if(ln<NS){memcpy(out,t+si,ln);out[ln]=0;} else out[0]=0; return jvs(out);
    }
    if (!strcmp(m,"slice")) {
        int si=n>0?(int)jvtod(&a[0]):0, ei=n>1?(int)jvtod(&a[1]):(int)sl;
        if(si<0) si=(int)sl+si; if(si<0) si=0;
        if(ei<0) ei=(int)sl+ei; if(ei<0) ei=0;
        if(si>(int)sl) si=(int)sl; if(ei>(int)sl) ei=(int)sl; if(ei<si) ei=si;
        unsigned ln=(unsigned)(ei-si); char out[NS];
        if(ln<NS){memcpy(out,t+si,ln);out[ln]=0;}else out[0]=0; return jvs(out);
    }
    if (!strcmp(m,"replace")) {
        if (n<2) return *self;
        char nd[NS],rp[NS]; jvtos(&a[0],nd,sizeof(nd)); jvtos(&a[1],rp,sizeof(rp));
        char*p=strstr(t,nd); if(!p) return jvs(t);
        unsigned nl=(unsigned)strlen(nd),rl=(unsigned)strlen(rp),ofs=(unsigned)(p-t);
        char out[NS*2]; if(ofs+rl+sl-nl>=sizeof(out)) return jvs(t);
        memcpy(out,t,ofs); memcpy(out+ofs,rp,rl); strcpy(out+ofs+rl,p+nl); return jvs(out);
    }
    if (!strcmp(m,"includes")) {
        if(!n) return jvb(0); char nd[NS]; jvtos(&a[0],nd,sizeof(nd)); return jvb(strstr(t,nd)?1:0);
    }
    if (!strcmp(m,"startsWith")) {
        if(!n) return jvb(0); char nd[NS]; jvtos(&a[0],nd,sizeof(nd)); return jvb(!strncmp(t,nd,strlen(nd)));
    }
    if (!strcmp(m,"endsWith")) {
        if(!n) return jvb(0); char nd[NS]; jvtos(&a[0],nd,sizeof(nd)); unsigned nl=(unsigned)strlen(nd);
        if(nl>sl) return jvb(0); return jvb(!strcmp(t+sl-nl,nd));
    }
    if (!strcmp(m,"repeat")) {
        unsigned cnt=n>0?(unsigned)jvtod(&a[0]):0; char out[NS]; out[0]=0; unsigned ol=0;
        for(unsigned i=0;i<cnt&&ol+sl<NS-1;i++){memcpy(out+ol,t,sl);ol+=sl;} out[ol]=0; return jvs(out);
    }
    if (!strcmp(m,"padStart")) {
        unsigned tgt=n>0?(unsigned)jvtod(&a[0]):sl; char pad[NS]=" ";
        if(n>1) jvtos(&a[1],pad,sizeof(pad)); unsigned pl=(unsigned)strlen(pad);
        if(sl>=tgt||!pl) return jvs(t);
        char out[NS]; unsigned ol=0;
        while(ol+sl<tgt&&ol+1<NS){ unsigned need=tgt-sl-ol,cp=need<pl?need:pl; if(ol+cp>=NS)break; memcpy(out+ol,pad,cp);ol+=cp; }
        if(ol+sl<NS){memcpy(out+ol,t,sl);out[ol+sl]=0;}else out[0]=0; return jvs(out);
    }
    if (!strcmp(m,"split")) {
        char sep[NS]=""; if(n>0) jvtos(&a[0],sep,sizeof(sep));
        unsigned sepl=(unsigned)strlen(sep);
        int ai=pool_arr_alloc(); JA*ja=&s_arr_pool[ai]; ja->len=0;
        if (!sepl) {
            /* split each char */
            for(unsigned i=0;i<sl&&ja->len<JA_M;i++){
                char ch[2]={t[i],0}; JVI it; it.t=VT_S; it.d=0;
                strncpy(it.s,ch,JVI_SMAX-1); it.s[JVI_SMAX-1]=0;
                ja->items[ja->len++]=it;
            }
        } else {
            const char *p=t, *end=t+sl;
            while(p<=end&&ja->len<JA_M) {
                const char *found=(p<=end-sepl)?strstr(p,sep):NULL;
                unsigned clen=found?(unsigned)(found-p):(unsigned)(end-p);
                JVI it; it.t=VT_S; it.d=0;
                strncpy(it.s,p,clen<JVI_SMAX-1?clen:JVI_SMAX-1); it.s[clen<JVI_SMAX-1?clen:JVI_SMAX-1]=0;
                ja->items[ja->len++]=it;
                if (!found) break;
                p=found+sepl;
            }
        }
        return jvarr(ai);
    }
    return jvu();
}

/* Forward declarations for array callback methods */
static JV call_fn(JSX*x, unsigned fi, JV*args, unsigned nargs);
static int find_fn(JSX*x, const char*name);

/* -------------------------------------------------------------------------
 * Array methods
 * ---------------------------------------------------------------------- */
static JV barr(JSX*x, JV*self, const char*m, JV*a, unsigned n) {
    int idx=(int)self->d;
    JA*ja=(idx>=0&&idx<JA_N)?&s_arr_pool[idx]:NULL;
    if (!ja) return jvu();

    if (!strcmp(m,"length")) return jvd((double)(ja->len));

    if (!strcmp(m,"push")) {
        for(unsigned i=0;i<n&&ja->len<JA_M;i++) ja->items[ja->len++]=jvi_from_jv(&a[i]);
        return jvd((double)ja->len);
    }
    if (!strcmp(m,"pop")) {
        if (!ja->len) return jvu();
        JVI it=ja->items[--ja->len]; return jv_from_jvi(&it);
    }
    if (!strcmp(m,"shift")) {
        if (!ja->len) return jvu();
        JVI it=ja->items[0];
        for(unsigned i=1;i<ja->len;i++) ja->items[i-1]=ja->items[i];
        ja->len--;
        return jv_from_jvi(&it);
    }
    if (!strcmp(m,"join")) {
        char sep[JVI_SMAX]=","; if(n>0){JV sv=a[0];jvtos(&sv,sep,sizeof(sep));}
        unsigned sepl=(unsigned)strlen(sep);
        char out[NS]; unsigned pos=0;
        for(unsigned i=0;i<ja->len&&pos+2<NS;i++) {
            JV tmp=jv_from_jvi(&ja->items[i]);
            char tb[JVI_SMAX+4]; jvtos(&tmp,tb,sizeof(tb));
            unsigned tl=(unsigned)strlen(tb); if(pos+tl>=NS-1) tl=NS-1-pos;
            memcpy(out+pos,tb,tl); pos+=tl;
            if(i+1<ja->len&&pos+sepl<NS-1){memcpy(out+pos,sep,sepl);pos+=sepl;}
        }
        out[pos]=0; return jvs(out);
    }
    if (!strcmp(m,"reverse")) {
        for(unsigned i=0,j=ja->len-1;i<j;i++,j--){JVI t=ja->items[i];ja->items[i]=ja->items[j];ja->items[j]=t;}
        return *self;
    }
    if (!strcmp(m,"indexOf")) {
        if(!n) return jvd(-1);
        char nd[JVI_SMAX]; JV av=a[0]; jvtos(&av,nd,sizeof(nd));
        for(unsigned i=0;i<ja->len;i++){
            JV tmp=jv_from_jvi(&ja->items[i]); char tb[JVI_SMAX]; jvtos(&tmp,tb,sizeof(tb));
            if(!strcmp(tb,nd)) return jvd((double)i);
        }
        return jvd(-1);
    }
    if (!strcmp(m,"toString")) {
        char out[NS]; unsigned pos=0;
        for(unsigned i=0;i<ja->len&&pos+2<NS;i++){
            JV tmp=jv_from_jvi(&ja->items[i]);
            char tb[JVI_SMAX+4]; jvtos(&tmp,tb,sizeof(tb));
            unsigned tl=(unsigned)strlen(tb); if(pos+tl>=NS-1) tl=NS-1-pos;
            memcpy(out+pos,tb,tl); pos+=tl;
            if(i+1<ja->len&&pos+1<NS) out[pos++]=',';
        }
        out[pos]=0; return jvs(out);
    }
    if (!strcmp(m,"includes")) {
        if(!n) return jvb(0);
        char nd[JVI_SMAX]; jvtos(&a[0],nd,sizeof(nd));
        for(unsigned i=0;i<ja->len;i++){JV tmp=jv_from_jvi(&ja->items[i]);char tb[JVI_SMAX];jvtos(&tmp,tb,sizeof(tb));if(!strcmp(tb,nd))return jvb(1);}
        return jvb(0);
    }
    if (!strcmp(m,"concat")) {
        int ri=pool_arr_alloc(); JA*ra=&s_arr_pool[ri]; ra->len=0;
        for(unsigned i=0;i<ja->len&&ra->len<JA_M;i++) ra->items[ra->len++]=ja->items[i];
        for(unsigned ci=0;ci<n;ci++){
            if(a[ci].t==VT_ARR){int si2=(int)a[ci].d;JA*sa=(si2>=0&&si2<JA_N)?&s_arr_pool[si2]:NULL;
                if(sa) for(unsigned i=0;i<sa->len&&ra->len<JA_M;i++) ra->items[ra->len++]=sa->items[i];
            } else { if(ra->len<JA_M) ra->items[ra->len++]=jvi_from_jv(&a[ci]); }
        }
        return jvarr(ri);
    }
    if (!strcmp(m,"slice")) {
        int si=n>0?(int)jvtod(&a[0]):0,ei=n>1?(int)jvtod(&a[1]):(int)ja->len;
        if(si<0)si=(int)ja->len+si;if(si<0)si=0;if(ei<0)ei=(int)ja->len+ei;if(ei<0)ei=0;
        if(si>(int)ja->len)si=(int)ja->len;if(ei>(int)ja->len)ei=(int)ja->len;if(ei<si)ei=si;
        int ri=pool_arr_alloc(); JA*ra=&s_arr_pool[ri]; ra->len=0;
        for(int ii=si;ii<ei&&ra->len<JA_M;ii++) ra->items[ra->len++]=ja->items[ii];
        return jvarr(ri);
    }
    if (!strcmp(m,"splice")) {
        int si=n>0?(int)jvtod(&a[0]):0,cnt=n>1?(int)jvtod(&a[1]):(int)ja->len;
        if(si<0)si=(int)ja->len+si;if(si<0)si=0;if(si>(int)ja->len)si=(int)ja->len;
        if(cnt<0)cnt=0;if(si+cnt>(int)ja->len)cnt=(int)ja->len-si;
        int ri=pool_arr_alloc(); JA*ra=&s_arr_pool[ri]; ra->len=0;
        for(int ii=si;ii<si+cnt;ii++) ra->items[ra->len++]=ja->items[ii];
        for(int ii=si;ii+(int)cnt<(int)ja->len;ii++) ja->items[ii]=ja->items[ii+cnt];
        ja->len-=(unsigned)cnt;
        return jvarr(ri);
    }
    if (!strcmp(m,"unshift")) {
        if(n>0&&ja->len<JA_M){for(int ii=(int)ja->len;ii>0;ii--)ja->items[ii]=ja->items[ii-1];ja->items[0]=jvi_from_jv(&a[0]);ja->len++;}
        return jvd((double)ja->len);
    }
    if (!strcmp(m,"flat")) {
        int ri=pool_arr_alloc(); JA*ra=&s_arr_pool[ri]; ra->len=0;
        for(unsigned i=0;i<ja->len&&ra->len<JA_M;i++){
            JV tmp=jv_from_jvi(&ja->items[i]);
            if(tmp.t==VT_ARR){int si2=(int)tmp.d;JA*sa=(si2>=0&&si2<JA_N)?&s_arr_pool[si2]:NULL;
                if(sa) for(unsigned j=0;j<sa->len&&ra->len<JA_M;j++) ra->items[ra->len++]=sa->items[j];
            } else { ra->items[ra->len++]=ja->items[i]; }
        }
        return jvarr(ri);
    }
    if (!strcmp(m,"fill")) {
        JV fv=n>0?a[0]:jvu();
        int si=n>1?(int)jvtod(&a[1]):0,ei=n>2?(int)jvtod(&a[2]):(int)ja->len;
        if(si<0)si=(int)ja->len+si;if(si<0)si=0;if(ei<0)ei=(int)ja->len+ei;if(ei<0)ei=0;
        if(si>(int)ja->len)si=(int)ja->len;if(ei>(int)ja->len)ei=(int)ja->len;
        for(int ii=si;ii<ei;ii++) ja->items[ii]=jvi_from_jv(&fv);
        return *self;
    }
    if (!strcmp(m,"sort")) {
        for(unsigned i=0;i<ja->len;i++) for(unsigned j=i+1;j<ja->len;j++){
            JV va=jv_from_jvi(&ja->items[i]),vb=jv_from_jvi(&ja->items[j]);
            char sa2[JVI_SMAX],sb2[JVI_SMAX]; jvtos(&va,sa2,sizeof(sa2)); jvtos(&vb,sb2,sizeof(sb2));
            if(strcmp(sa2,sb2)>0){JVI t2=ja->items[i];ja->items[i]=ja->items[j];ja->items[j]=t2;}
        }
        return *self;
    }
    if (!strcmp(m,"map")) {
        int ri=pool_arr_alloc(); JA*ra=&s_arr_pool[ri]; ra->len=0;
        if(!n||a[0].t!=VT_FN) return jvarr(ri);
        int fi=find_fn(x,a[0].s); if(fi<0) return jvarr(ri);
        for(unsigned i=0;i<ja->len&&ra->len<JA_M;i++){
            JV elem=jv_from_jvi(&ja->items[i]); JV iv=jvd((double)i);
            JV ca[2]={elem,iv}; JV res=call_fn(x,(unsigned)fi,ca,2);
            ra->items[ra->len++]=jvi_from_jv(&res);
        }
        return jvarr(ri);
    }
    if (!strcmp(m,"filter")) {
        int ri=pool_arr_alloc(); JA*ra=&s_arr_pool[ri]; ra->len=0;
        if(!n||a[0].t!=VT_FN) return jvarr(ri);
        int fi=find_fn(x,a[0].s); if(fi<0) return jvarr(ri);
        for(unsigned i=0;i<ja->len&&ra->len<JA_M;i++){
            JV elem=jv_from_jvi(&ja->items[i]); JV iv=jvd((double)i);
            JV ca[2]={elem,iv}; JV res=call_fn(x,(unsigned)fi,ca,2);
            if(jvtruthy(&res)) ra->items[ra->len++]=jvi_from_jv(&elem);
        }
        return jvarr(ri);
    }
    if (!strcmp(m,"reduce")) {
        if(!n||a[0].t!=VT_FN) return jvu();
        int fi=find_fn(x,a[0].s); if(fi<0) return jvu();
        JV acc=n>1?a[1]:(ja->len>0?jv_from_jvi(&ja->items[0]):jvu());
        unsigned st=n>1?0:1;
        for(unsigned i=st;i<ja->len;i++){
            JV elem=jv_from_jvi(&ja->items[i]); JV iv=jvd((double)i);
            JV ca[3]={acc,elem,iv}; acc=call_fn(x,(unsigned)fi,ca,3);
        }
        return acc;
    }
    if (!strcmp(m,"reduceRight")) {
        if(!n||a[0].t!=VT_FN) return jvu();
        int fi=find_fn(x,a[0].s); if(fi<0) return jvu();
        unsigned last=ja->len>0?ja->len-1:0;
        JV acc=n>1?a[1]:(ja->len>0?jv_from_jvi(&ja->items[last]):jvu());
        int st=n>1?(int)ja->len-1:(int)ja->len-2;
        for(int i=st;i>=0;i--){
            JV elem=jv_from_jvi(&ja->items[i]); JV iv=jvd((double)i);
            JV ca[3]={acc,elem,iv}; acc=call_fn(x,(unsigned)fi,ca,3);
        }
        return acc;
    }
    if (!strcmp(m,"forEach")) {
        if(!n||a[0].t!=VT_FN) return jvu();
        int fi=find_fn(x,a[0].s); if(fi<0) return jvu();
        for(unsigned i=0;i<ja->len;i++){
            JV elem=jv_from_jvi(&ja->items[i]); JV iv=jvd((double)i);
            JV ca[2]={elem,iv}; call_fn(x,(unsigned)fi,ca,2);
        }
        return jvu();
    }
    if (!strcmp(m,"every")) {
        if(!n||a[0].t!=VT_FN) return jvb(1);
        int fi=find_fn(x,a[0].s); if(fi<0) return jvb(1);
        for(unsigned i=0;i<ja->len;i++){
            JV elem=jv_from_jvi(&ja->items[i]); JV iv=jvd((double)i);
            JV ca[2]={elem,iv}; JV res=call_fn(x,(unsigned)fi,ca,2);
            if(!jvtruthy(&res)) return jvb(0);
        }
        return jvb(1);
    }
    if (!strcmp(m,"some")) {
        if(!n||a[0].t!=VT_FN) return jvb(0);
        int fi=find_fn(x,a[0].s); if(fi<0) return jvb(0);
        for(unsigned i=0;i<ja->len;i++){
            JV elem=jv_from_jvi(&ja->items[i]); JV iv=jvd((double)i);
            JV ca[2]={elem,iv}; JV res=call_fn(x,(unsigned)fi,ca,2);
            if(jvtruthy(&res)) return jvb(1);
        }
        return jvb(0);
    }
    if (!strcmp(m,"findIndex")) {
        if(!n||a[0].t!=VT_FN) return jvd(-1);
        int fi=find_fn(x,a[0].s); if(fi<0) return jvd(-1);
        for(unsigned i=0;i<ja->len;i++){
            JV elem=jv_from_jvi(&ja->items[i]); JV iv=jvd((double)i);
            JV ca[2]={elem,iv}; JV res=call_fn(x,(unsigned)fi,ca,2);
            if(jvtruthy(&res)) return jvd((double)i);
        }
        return jvd(-1);
    }
    if (!strcmp(m,"find")) {
        if(!n||a[0].t!=VT_FN) return jvu();
        int fi=find_fn(x,a[0].s); if(fi<0) return jvu();
        for(unsigned i=0;i<ja->len;i++){
            JV elem=jv_from_jvi(&ja->items[i]); JV iv=jvd((double)i);
            JV ca[2]={elem,iv}; JV res=call_fn(x,(unsigned)fi,ca,2);
            if(jvtruthy(&res)) return elem;
        }
        return jvu();
    }
    return jvu();
}

/* -------------------------------------------------------------------------
 * Object property helpers
 * ---------------------------------------------------------------------- */
static JV bobj_get(JV*self, const char*key) {
    int idx=(int)self->d;
    JO*jo=(idx>=0&&idx<JO_N)?&s_obj_pool[idx]:NULL;
    if (!jo) return jvu();
    for(unsigned i=0;i<jo->len;i++) if(!strcmp(jo->keys[i],key)) return jv_from_jvi(&jo->vals[i]);
    return jvu();
}
static void bobj_set(JV*self, const char*key, JV val) {
    int idx=(int)self->d;
    JO*jo=(idx>=0&&idx<JO_N)?&s_obj_pool[idx]:NULL;
    if (!jo) return;
    for(unsigned i=0;i<jo->len;i++) if(!strcmp(jo->keys[i],key)){jo->vals[i]=jvi_from_jv(&val);return;}
    if(jo->len<JO_M){ strncpy(jo->keys[jo->len],key,NM-1); jo->keys[jo->len][NM-1]=0; jo->vals[jo->len]=jvi_from_jv(&val); jo->len++; }
}

/* -------------------------------------------------------------------------
 * Forward declarations
 * ---------------------------------------------------------------------- */
static JV eval_assign(JSX*x);
static void exec_stmt(JSX*x);
static void exec_block(JSX*x);

/* -------------------------------------------------------------------------
 * Argument list
 * ---------------------------------------------------------------------- */
static unsigned eval_args(JSX*x, JV*args, unsigned cap) {
    unsigned n=0; jsx_ws(x);
    if (jsx_p(x)==')') { jsx_eat(x); return 0; }
    while (!jsx_end(x)) {
        if (n<cap) args[n++]=eval_assign(x); else eval_assign(x);
        jsx_ws(x); char c=jsx_p(x);
        if (c==')') { jsx_eat(x); break; }
        if (c==',') { jsx_eat(x); jsx_ws(x); continue; }
        break;
    }
    return n;
}

/* -------------------------------------------------------------------------
 * User function call
 * ---------------------------------------------------------------------- */
static JV call_fn(JSX*x, unsigned fi, JV*args, unsigned nargs) {
    if (x->call_depth>=NCD) return jvu();
    x->call_depth++;
    unsigned sp=x->pos, snv=x->nv, sret=x->ret_f;
    x->ret_f=0; x->brk_f=0; x->cont_f=0;
    JFN*f=&x->fn[fi];
    for (unsigned i=0;i<f->np;i++) var_set(x,f->pn[i],i<nargs?args[i]:jvu());
    x->pos=f->bs;
    exec_block(x);
    JV result=x->ret_f?x->ret_v:jvu();
    x->ret_f=sret; x->nv=snv; x->pos=sp;
    x->call_depth--;
    return result;
}

/* -------------------------------------------------------------------------
 * Find function by name
 * ---------------------------------------------------------------------- */
static int find_fn(JSX*x, const char*name) {
    for (unsigned i=0;i<x->nf;i++) if (!strcmp(x->fn[i].name,name)) return (int)i;
    return -1;
}

/* -------------------------------------------------------------------------
 * Primary expression
 * ---------------------------------------------------------------------- */
static JV eval_primary(JSX*x) {
    jsx_ws(x); char c=jsx_p(x);

    /* numeric literal */
    if (isdigit((unsigned char)c)||
        (c=='0'&&(jsx_p2(x)=='x'||jsx_p2(x)=='X'))) {
        char buf[64]; unsigned n=0;
        if (c=='0'&&(jsx_p2(x)=='x'||jsx_p2(x)=='X')) {
            buf[n++]=jsx_eat(x); buf[n++]=jsx_eat(x);
            while(!jsx_end(x)&&isxdigit((unsigned char)jsx_p(x))&&n+1<64) buf[n++]=jsx_eat(x);
            buf[n]=0; return jvd((double)strtol(buf,NULL,16));
        }
        while(!jsx_end(x)&&(isdigit((unsigned char)jsx_p(x))||jsx_p(x)=='.'
               ||jsx_p(x)=='e'||jsx_p(x)=='E')&&n+1<64) buf[n++]=jsx_eat(x);
        buf[n]=0; return jvd(atof(buf));
    }

    /* string literal */
    if (c=='"'||c=='\''||c=='`') { char buf[NS]; read_str(x,buf,sizeof(buf)); return jvs(buf); }

    /* array literal */
    if (c=='[') {
        jsx_eat(x);
        int ai=pool_arr_alloc(); JA*ja=&s_arr_pool[ai];
        jsx_ws(x);
        while (!jsx_end(x)&&jsx_p(x)!=']') {
            if (ja->len<JA_M) {
                JV item=eval_assign(x);
                ja->items[ja->len++]=jvi_from_jv(&item);
            } else {
                eval_assign(x); /* consume but discard */
            }
            jsx_ws(x);
            if (jsx_p(x)==',') { jsx_eat(x); jsx_ws(x); }
        }
        if (jsx_p(x)==']') jsx_eat(x);
        return jvarr(ai);
    }

    /* object literal */
    if (c=='{') {
        jsx_eat(x);
        int oi=pool_obj_alloc(); JO*jo=&s_obj_pool[oi];
        jsx_ws(x);
        while (!jsx_end(x)&&jsx_p(x)!='}') {
            char key[NM]="";
            char kc=jsx_p(x);
            if (kc=='"'||kc=='\'') { read_str(x,key,sizeof(key)); }
            else if (is_id_s(kc)) { read_id(x,key,sizeof(key)); }
            else { jsx_eat(x); break; }
            jsx_ws(x);
            if (jsx_p(x)==':') jsx_eat(x); jsx_ws(x);
            JV val=eval_assign(x);
            if (jo->len<JO_M) {
                strncpy(jo->keys[jo->len],key,NM-1); jo->keys[jo->len][NM-1]=0;
                jo->vals[jo->len]=jvi_from_jv(&val); jo->len++;
            }
            jsx_ws(x);
            if (jsx_p(x)==',') { jsx_eat(x); jsx_ws(x); }
        }
        if (jsx_p(x)=='}') jsx_eat(x);
        return jvobj(oi);
    }

    /* parenthesised expression */
    if (c=='(') {
        jsx_eat(x); JV v=eval_assign(x); jsx_ws(x);
        if (jsx_p(x)==')') jsx_eat(x); return v;
    }

    /* identifier */
    if (is_id_s(c)) {
        char name[NM]; read_id(x,name,sizeof(name)); jsx_ws(x);

        /* keywords */
        if (!strcmp(name,"undefined")) return jvu();
        if (!strcmp(name,"null"))      return jvn();
        if (!strcmp(name,"true"))      return jvb(1);
        if (!strcmp(name,"false"))     return jvb(0);
        if (!strcmp(name,"NaN"))       return jvd(0.0);
        if (!strcmp(name,"Infinity"))  return jvd(1e300);

        /* typeof prefix */
        if (!strcmp(name,"typeof")) {
            jsx_ws(x);
            unsigned sp=x->pos; JV v=eval_primary(x);
            x->pos=sp; eval_primary(x);
            const char*tn="undefined";
            if (v.t==VT_S) tn="string";
            if (v.t==VT_FN) tn="function";
            if (v.t==VT_D) tn="number";
            if (v.t==VT_B) tn="boolean";
            if (v.t==VT_N||v.t==VT_OBJ||v.t==VT_ARR) tn="object";
            return jvs(tn);
        }

        /* new constructor */
        if (!strcmp(name,"new")) {
            char cn[NM]; jsx_ws(x); read_id(x,cn,sizeof(cn)); jsx_ws(x);
            JV cargs[8]; unsigned cna=0;
            if (jsx_p(x)=='(') { jsx_eat(x); cna=eval_args(x,cargs,8); }
            if (!strcmp(cn,"Date")) {
                /* Build date object with UTC time (caller may set res->tz_offset_minutes) */
                int oi=pool_obj_alloc(); JO*jo=&s_obj_pool[oi];
                jo->len=0;
                int tzm=x->res?x->res->tz_offset_minutes:0;
                unsigned long ts=(unsigned long)tal_time_get_posix() + (unsigned long)(tzm*60);
                unsigned long sec=ts%60; unsigned long rem=ts/60;
                unsigned long mn=rem%60; rem/=60;
                unsigned long hr=rem%24; unsigned long days=rem/24;
                unsigned long yr=1970; unsigned long mo,dy;
                while(1){unsigned yd=((yr%4==0&&yr%100!=0)||(yr%400==0))?366:365;if(days<yd)break;days-=yd;yr++;}
                static const unsigned char mdays[12]={31,28,31,30,31,30,31,31,30,31,30,31};
                for(mo=0;mo<12;mo++){unsigned md=mdays[mo]+(mo==1&&((yr%4==0&&yr%100!=0)||(yr%400==0))?1:0);if(days<md)break;days-=md;}
                dy=days+1;
                JV fyr=jvd((double)yr); JV fmo=jvd((double)mo); JV fda=jvd((double)dy);
                JV fhr=jvd((double)hr); JV fmn=jvd((double)mn); JV fsc=jvd((double)sec);
                #define _dset(k,v) strncpy(jo->keys[jo->len],k,NM-1);jo->keys[jo->len][NM-1]=0;jo->vals[jo->len]=jvi_from_jv(&(v));jo->len++
                _dset("$fy",fyr); _dset("$fm",fmo); _dset("$fd",fda);
                _dset("$fh",fhr); _dset("$fn",fmn); _dset("$fs",fsc);
                #undef _dset
                return jvobj(oi);
            }
            if (!strcmp(cn,"Array")) {
                int ai=pool_arr_alloc();
                if(cna==1&&cargs[0].t==VT_D){
                    /* new Array(n) — pre-fill with undefined */
                    unsigned cnt=(unsigned)jvtod(&cargs[0]); if(cnt>JA_M) cnt=JA_M;
                    JV u=jvu();
                    for(unsigned i=0;i<cnt;i++) s_arr_pool[ai].items[i]=jvi_from_jv(&u);
                    s_arr_pool[ai].len=cnt;
                }
                return jvarr(ai);
            }
            if (!strcmp(cn,"Object")) { int oi=pool_obj_alloc(); return jvobj(oi); }
            return jvu();
        }

        /* function expression — register if named, skip body */
        if (!strcmp(name,"function")) {
            char fname[NM]=""; jsx_ws(x);
            if (is_id_s(jsx_p(x))) read_id(x,fname,sizeof(fname));
            jsx_ws(x);
            char pn[8][NM]; unsigned np=0;
            if (jsx_p(x)=='(') {
                jsx_eat(x); jsx_ws(x);
                while (jsx_p(x)!=')'&&!jsx_end(x)) {
                    if (np<8&&is_id_s(jsx_p(x))) read_id(x,pn[np++],NM);
                    jsx_ws(x); if (jsx_p(x)==','){jsx_eat(x);jsx_ws(x);}
                }
                if (jsx_p(x)==')') jsx_eat(x);
            }
            jsx_ws(x);
            unsigned bs=x->pos; /* points at { */
            /* auto-name anonymous function */
            char rname[NM];
            if (!fname[0]) { snprintf(rname,sizeof(rname),"__anon%u",x->anon_cnt++); strncpy(fname,rname,NM-1); }
            if (x->nf<NF) {
                strncpy(x->fn[x->nf].name,fname,NM-1);
                x->fn[x->nf].bs=bs; x->fn[x->nf].np=np;
                for(unsigned i=0;i<np;i++) strncpy(x->fn[x->nf].pn[i],pn[i],NM-1);
                x->nf++;
            }
            skip_block(x);
            return jvfn(fname);
        }

        /* built-in object sentinels (no call) */
        if (!strcmp(name,"document")) return jvs("[doc]");
        if (!strcmp(name,"window")  ) return jvs("[win]");
        if (!strcmp(name,"Math")    ) return jvs("[Math]");
        if (!strcmp(name,"console") ) return jvs("[con]");
        if (!strcmp(name,"JSON")    ) return jvs("[JSON]");
        if (!strcmp(name,"location")) {
            /* standalone location object */
            return jvs("[loc]");
        }
        /* Class namespace sentinels — only when NOT called as function (e.g. String.fromCharCode) */
        if (!strcmp(name,"String") && jsx_p(x)!='(') return jvs("[Str]");
        if (!strcmp(name,"Object") && jsx_p(x)!='(') return jvs("[Obj]");
        if (!strcmp(name,"Array")  && jsx_p(x)!='(') return jvs("[Arr]");

        /* direct function call */
        if (jsx_p(x)=='(') {
            jsx_eat(x); /* consume ( */
            JV args[8]; unsigned na=eval_args(x,args,8);

            if (!strcmp(name,"alert")||!strcmp(name,"confirm")||!strcmp(name,"prompt")) {
                if (na>0&&x->res&&!x->res->has_alert) {
                    jvtos(&args[0],x->res->alert_msg,JS_ALERT_MAX-1);
                    x->res->has_alert=TRUE;
                }
                return !strcmp(name,"confirm")?jvb(1):jvu();
            }
            if (!strcmp(name,"parseInt")||!strcmp(name,"parseFloat")) {
                if (!na) return jvd(0.0);
                char t[NS]; jvtos(&args[0],t,sizeof(t));
                return jvd(!strcmp(name,"parseInt")?(double)strtol(t,NULL,na>1?(int)jvtod(&args[1]):10):atof(t));
            }
            if (!strcmp(name,"String"))  { char t[NS]; jvtos(na?&args[0]:&(JV){VT_U,0,{0}},t,sizeof(t)); return jvs(t); }
            if (!strcmp(name,"Number"))  return jvd(na?jvtod(&args[0]):0.0);
            if (!strcmp(name,"Boolean")) return jvb(na?jvtruthy(&args[0]):0);
            if (!strcmp(name,"isNaN")) {
                if(!na||args[0].t==VT_U) return jvb(1);
                if(args[0].t==VT_D||args[0].t==VT_B||args[0].t==VT_N) return jvb(0);
                char _nt[NS]; jvtos(&args[0],_nt,sizeof(_nt));
                unsigned _ni=0; while(_nt[_ni]==' '||_nt[_ni]=='\t') _ni++;
                if(!_nt[_ni]) return jvb(0);
                char *_ne; strtod(_nt+_ni,&_ne);
                while(*_ne==' '||*_ne=='\t') _ne++;
                return jvb(*_ne!=0);
            }
            if (!strcmp(name,"isFinite"))return jvb(1);
            if (!strcmp(name,"encodeURIComponent")||!strcmp(name,"decodeURIComponent")) return na?args[0]:jvu();
            if (!strcmp(name,"encodeURI")||!strcmp(name,"decodeURI")) return na?args[0]:jvu();
            if (!strcmp(name,"unescape")||!strcmp(name,"escape")) return na?args[0]:jvu();
            if (!strcmp(name,"clearTimeout")||!strcmp(name,"clearInterval")) return jvu();
            if (!strcmp(name,"setTimeout")||!strcmp(name,"setInterval"))     return jvd(0.0);

            /* user function */
            int fi=find_fn(x,name);
            if (fi>=0) return call_fn(x,(unsigned)fi,args,na);
            return jvu();
        }

        /* plain identifier — variable lookup */
        return var_get(x,name);
    }

    return jvu();
}

/* -------------------------------------------------------------------------
 * Member access and suffix calls
 * ---------------------------------------------------------------------- */
static JV eval_call(JSX*x) {
    JV val=eval_primary(x);

    while (!jsx_end(x)) {
        jsx_ws(x); char c=jsx_p(x);

        if (c=='.') {
            jsx_eat(x); char member[NM]; read_id(x,member,sizeof(member)); jsx_ws(x);
            char sv[NS]; jvtos(&val,sv,sizeof(sv));

            if (jsx_p(x)=='(') {
                /* method call */
                jsx_eat(x); JV args[8]; unsigned na=eval_args(x,args,8);

                /* array methods */
                if (val.t==VT_ARR) { val=barr(x,&val,member,args,na); continue; }

                /* object method calls (Date methods + common) */
                if (val.t==VT_OBJ) {
                    if (!strcmp(member,"getFullYear"))  { val=bobj_get(&val,"$fy"); continue; }
                    if (!strcmp(member,"getMonth"))     { val=bobj_get(&val,"$fm"); continue; }
                    if (!strcmp(member,"getDate"))      { val=bobj_get(&val,"$fd"); continue; }
                    if (!strcmp(member,"getHours"))     { val=bobj_get(&val,"$fh"); continue; }
                    if (!strcmp(member,"getMinutes"))   { val=bobj_get(&val,"$fn"); continue; }
                    if (!strcmp(member,"getSeconds"))   { val=bobj_get(&val,"$fs"); continue; }
                    if (!strcmp(member,"getTime"))      { val=jvd((double)tal_time_get_posix()*1000.0); continue; }
                    if (!strcmp(member,"hasOwnProperty")) {
                        if(!na){val=jvb(0);continue;}
                        char hk[NM]; jvtos(&args[0],hk,sizeof(hk));
                        int hoi=(int)val.d; JO*hjo=(hoi>=0&&hoi<JO_N)?&s_obj_pool[hoi]:NULL;
                        int hf=0; if(hjo) for(unsigned hi=0;hi<hjo->len;hi++) if(!strcmp(hjo->keys[hi],hk)){hf=1;break;}
                        val=jvb(hf); continue;
                    }
                    if (!strcmp(member,"toString")||!strcmp(member,"valueOf")) { val=jvs("[object Object]"); continue; }
                    val=jvu(); continue;
                }

                /* JSON methods */
                if (!strcmp(sv,"[JSON]")) {
                    if (!strcmp(member,"stringify")) {
                        if (!na) { val=jvs("undefined"); continue; }
                        char out[NS]; jvtos(&args[0],out,sizeof(out));
                        if (args[0].t==VT_OBJ) {
                            /* build {"k":v,...} */
                            int oi=(int)args[0].d; JO*jo=(oi>=0&&oi<JO_N)?&s_obj_pool[oi]:NULL;
                            char jout[NS]; unsigned pos=0; jout[pos++]='{';
                            if(jo) for(unsigned i=0;i<jo->len&&pos+6<NS;i++){
                                JV tmp=jv_from_jvi(&jo->vals[i]); char vb[JVI_SMAX+4]; jvtos(&tmp,vb,sizeof(vb));
                                unsigned kl=(unsigned)strlen(jo->keys[i]),vl=(unsigned)strlen(vb);
                                if(pos+kl+vl+6>=NS) break;
                                jout[pos++]='"'; memcpy(jout+pos,jo->keys[i],kl); pos+=kl; jout[pos++]='"'; jout[pos++]=':';
                                if(tmp.t==VT_S){jout[pos++]='"'; memcpy(jout+pos,vb,vl); pos+=vl; jout[pos++]='"';}
                                else{memcpy(jout+pos,vb,vl);pos+=vl;}
                                if(i+1<jo->len&&pos+1<NS) jout[pos++]=',';
                            }
                            if(pos<NS) jout[pos++]='}'; jout[pos]=0;
                            val=jvs(jout); continue;
                        } else if (args[0].t==VT_ARR) {
                            char jout[NS]; unsigned pos=0; jout[pos++]='[';
                            int ai=(int)args[0].d; JA*ja=(ai>=0&&ai<JA_N)?&s_arr_pool[ai]:NULL;
                            if(ja) for(unsigned i=0;i<ja->len&&pos+4<NS;i++){
                                JV tmp=jv_from_jvi(&ja->items[i]); char vb[JVI_SMAX+4]; jvtos(&tmp,vb,sizeof(vb));
                                unsigned vl=(unsigned)strlen(vb);
                                if(pos+vl+4>=NS) break;
                                if(tmp.t==VT_S){jout[pos++]='"'; memcpy(jout+pos,vb,vl); pos+=vl; jout[pos++]='"';}
                                else{memcpy(jout+pos,vb,vl);pos+=vl;}
                                if(i+1<ja->len&&pos+1<NS) jout[pos++]=',';
                            }
                            if(pos<NS) jout[pos++]=']'; jout[pos]=0;
                            val=jvs(jout); continue;
                        }
                        if (args[0].t==VT_S) { char q[NS+4]; snprintf(q,sizeof(q),"\"%s\"",args[0].s); val=jvs(q); continue; }
                        val=jvs(out); continue;
                    }
                    if (!strcmp(member,"parse")) {
                        /* Minimal JSON parser: return object or array */
                        if (!na) { val=jvn(); continue; }
                        char src[NS]; jvtos(&args[0],src,sizeof(src));
                        char *p=src; while(*p==' '||*p=='\t'||*p=='\n') p++;
                        if (*p=='{') {
                            int oi=pool_obj_alloc(); JO*jo=&s_obj_pool[oi]; p++;
                            while(*p&&*p!='}') {
                                while(*p==' '||*p=='\t'||*p=='\n') p++;
                                if(*p!='"'&&*p!='\'') { p++; continue; }
                                char q=*p++; char key[NM]; unsigned ki=0;
                                while(*p&&*p!=q&&ki+1<NM) key[ki++]=*p++; key[ki]=0; if(*p) p++;
                                while(*p==' '||*p=='\t'||*p=='\n') p++; if(*p==':') p++;
                                while(*p==' '||*p=='\t'||*p=='\n') p++;
                                JV v; char vb[JVI_SMAX]; unsigned vi=0;
                                if(*p=='"'||*p=='\''){char vq=*p++;while(*p&&*p!=vq&&vi+1<JVI_SMAX)vb[vi++]=*p++;vb[vi]=0;if(*p)p++;v=jvs(vb);}
                                else if(*p=='t'&&p[1]=='r'&&p[2]=='u'&&p[3]=='e'){v=jvb(1);p+=4;}
                                else if(*p=='f'){v=jvb(0);p+=5;}
                                else if(*p=='n'){v=jvn();p+=4;}
                                else{while(*p&&*p!=','&&*p!='}'&&vi+1<JVI_SMAX)vb[vi++]=*p++;vb[vi]=0;v=jvd(atof(vb));}
                                if(jo->len<JO_M){strncpy(jo->keys[jo->len],key,NM-1);jo->keys[jo->len][NM-1]=0;jo->vals[jo->len]=jvi_from_jv(&v);jo->len++;}
                                while(*p==' '||*p=='\t') p++; if(*p==',') p++;
                            }
                            val=jvobj(oi); continue;
                        }
                        if (*p=='[') {
                            int ai=pool_arr_alloc(); JA*ja=&s_arr_pool[ai]; p++;
                            while(*p&&*p!=']') {
                                while(*p==' '||*p=='\t'||*p=='\n') p++;
                                if(*p==']') break;
                                JV v; char vb[JVI_SMAX]; unsigned vi=0;
                                if(*p=='"'||*p=='\''){char vq=*p++;while(*p&&*p!=vq&&vi+1<JVI_SMAX)vb[vi++]=*p++;vb[vi]=0;if(*p)p++;v=jvs(vb);}
                                else if(*p=='t'&&p[1]=='r'){v=jvb(1);p+=4;}
                                else if(*p=='f'){v=jvb(0);p+=5;}
                                else if(*p=='n'){v=jvn();p+=4;}
                                else{while(*p&&*p!=','&&*p!=']'&&vi+1<JVI_SMAX)vb[vi++]=*p++;vb[vi]=0;v=jvd(atof(vb));}
                                if(ja->len<JA_M)ja->items[ja->len++]=jvi_from_jv(&v);
                                while(*p==' '||*p=='\t') p++; if(*p==',') p++;
                            }
                            val=jvarr(ai); continue;
                        }
                        val=jvn(); continue;
                    }
                    val=jvu(); continue;
                }

                /* document methods */
                if (!strcmp(sv,"[doc]")) {
                    if (!strcmp(member,"write")||!strcmp(member,"writeln")) {
                        for(unsigned i=0;i<na;i++) js_write_val(x,&args[i]);
                        if (!strcmp(member,"writeln")) js_write(x,"\n");
                        val=jvu(); continue;
                    }
                    if (!strcmp(member,"getElementById") && na > 0) {
                        char id[64]; jvtos(&args[0], id, sizeof(id));
                        WIN95_HTML_DOM_INFO_T info;
                        val = win95_html_dom_get_info(id, &info) ? jvdom(id) : jvu();
                        continue;
                    }
                    if (!strcmp(member,"querySelector") && na > 0) {
                        char sel[64]; jvtos(&args[0], sel, sizeof(sel));
                        if (sel[0] == '#') {
                            WIN95_HTML_DOM_INFO_T info;
                            val = win95_html_dom_get_info(sel + 1, &info) ? jvdom(sel + 1) : jvu();
                        } else {
                            val = jvu();
                        }
                        continue;
                    }
                    if (!strcmp(member,"getElementsByTagName")||!strcmp(member,"getElementsByClassName"))
                        { val=jvu(); continue; }
                    val=jvu(); continue;
                }

                if (is_dom_ref(&val)) {
                    char id[64], buf[NS];
                    if (!dom_ref_id(&val, id, sizeof(id))) { val = jvu(); continue; }
                    if (!strcmp(member,"getAttribute")) {
                        if (!na) { val = jvs(""); continue; }
                        char attr[64]; jvtos(&args[0], attr, sizeof(attr));
                        if (win95_html_dom_get_attr(id, attr, buf, sizeof(buf))) val = jvs(buf);
                        else val = jvu();
                        continue;
                    }
                    if (!strcmp(member,"setAttribute")) {
                        if (na >= 2) {
                            char attr[64], value[NS];
                            jvtos(&args[0], attr, sizeof(attr));
                            jvtos(&args[1], value, sizeof(value));
                            win95_html_dom_set_attr(id, attr, value);
                        }
                        val = jvu(); continue;
                    }
                    if (!strcmp(member,"submit")) {
                        win95_html_dom_submit(id);
                        val = jvu(); continue;
                    }
                    if (!strcmp(member,"focus")) {
                        val = jvu(); continue;
                    }
                }

                /* window methods */
                if (!strcmp(sv,"[win]")) {
                    if (!strcmp(member,"alert")||!strcmp(member,"confirm")) {
                        if (na>0&&x->res&&!x->res->has_alert) { jvtos(&args[0],x->res->alert_msg,JS_ALERT_MAX-1); x->res->has_alert=TRUE; }
                        val=!strcmp(member,"confirm")?jvb(1):jvu(); continue;
                    }
                    val=jvu(); continue;
                }

                /* console methods */
                if (!strcmp(sv,"[con]")) { val=jvu(); continue; }

                /* Math */
                if (!strcmp(sv,"[Math]")) { val=bmath(member,args,na); continue; }

                /* String static methods */
                if (!strcmp(sv,"[Str]")) {
                    if (!strcmp(member,"fromCharCode")) {
                        char sfc[NS]; unsigned sfp=0;
                        for(unsigned i=0;i<na&&sfp+1<NS;i++){int cd=(int)jvtod(&args[i]);if(cd>0&&cd<128)sfc[sfp++]=(char)cd;}
                        sfc[sfp]=0; val=jvs(sfc); continue;
                    }
                    val=jvu(); continue;
                }

                /* Object static methods */
                if (!strcmp(sv,"[Obj]")) {
                    if (!strcmp(member,"keys")) {
                        int ri=pool_arr_alloc(); JA*ra=&s_arr_pool[ri]; ra->len=0;
                        if(na&&args[0].t==VT_OBJ){int oi=(int)args[0].d;JO*jo=(oi>=0&&oi<JO_N)?&s_obj_pool[oi]:NULL;
                            if(jo) for(unsigned i=0;i<jo->len&&ra->len<JA_M;i++){JVI it;it.t=VT_S;it.d=0;strncpy(it.s,jo->keys[i],JVI_SMAX-1);it.s[JVI_SMAX-1]=0;ra->items[ra->len++]=it;}}
                        val=jvarr(ri); continue;
                    }
                    if (!strcmp(member,"values")) {
                        int ri=pool_arr_alloc(); JA*ra=&s_arr_pool[ri]; ra->len=0;
                        if(na&&args[0].t==VT_OBJ){int oi=(int)args[0].d;JO*jo=(oi>=0&&oi<JO_N)?&s_obj_pool[oi]:NULL;
                            if(jo) for(unsigned i=0;i<jo->len&&ra->len<JA_M;i++) ra->items[ra->len++]=jo->vals[i];}
                        val=jvarr(ri); continue;
                    }
                    if (!strcmp(member,"assign")) {
                        if(na>=2&&args[0].t==VT_OBJ&&args[1].t==VT_OBJ){
                            int si2=(int)args[1].d; JO*src=(si2>=0&&si2<JO_N)?&s_obj_pool[si2]:NULL;
                            if(src) for(unsigned i=0;i<src->len;i++){JV tmp=jv_from_jvi(&src->vals[i]);bobj_set(&args[0],src->keys[i],tmp);}
                        }
                        val=na?args[0]:jvu(); continue;
                    }
                    if (!strcmp(member,"entries")) {
                        int ri=pool_arr_alloc(); JA*ra=&s_arr_pool[ri]; ra->len=0;
                        if(na&&args[0].t==VT_OBJ){int oi=(int)args[0].d;JO*jo=(oi>=0&&oi<JO_N)?&s_obj_pool[oi]:NULL;
                            if(jo) for(unsigned i=0;i<jo->len&&ra->len<JA_M;i++){
                                int ei2=pool_arr_alloc(); JA*ea=&s_arr_pool[ei2]; ea->len=0;
                                JVI kv; kv.t=VT_S; kv.d=0; strncpy(kv.s,jo->keys[i],JVI_SMAX-1); kv.s[JVI_SMAX-1]=0;
                                ea->items[ea->len++]=kv; ea->items[ea->len++]=jo->vals[i];
                                JV eav=jvarr(ei2); JVI eavi=jvi_from_jv(&eav); ra->items[ra->len++]=eavi;
                            }
                        }
                        val=jvarr(ri); continue;
                    }
                    if (!strcmp(member,"create")) {
                        /* Object.create(proto) — return empty object (proto chain not tracked) */
                        val=jvobj(pool_obj_alloc()); continue;
                    }
                    val=jvu(); continue;
                }

                /* Array static methods */
                if (!strcmp(sv,"[Arr]")) {
                    if (!strcmp(member,"isArray")) { val=jvb(na&&args[0].t==VT_ARR); continue; }
                    if (!strcmp(member,"from")) {
                        if(!na){val=jvarr(pool_arr_alloc());continue;}
                        if(args[0].t==VT_ARR){val=args[0];continue;}
                        char ft[NS]; jvtos(&args[0],ft,sizeof(ft)); unsigned fsl=(unsigned)strlen(ft);
                        int ri=pool_arr_alloc(); JA*ra=&s_arr_pool[ri]; ra->len=0;
                        for(unsigned i=0;i<fsl&&ra->len<JA_M;i++){JVI it;it.t=VT_S;it.d=0;it.s[0]=ft[i];it.s[1]=0;ra->items[ra->len++]=it;}
                        val=jvarr(ri); continue;
                    }
                    if (!strcmp(member,"of")) {
                        int ri=pool_arr_alloc(); JA*ra=&s_arr_pool[ri]; ra->len=0;
                        for(unsigned i=0;i<na&&ra->len<JA_M;i++) ra->items[ra->len++]=jvi_from_jv(&args[i]);
                        val=jvarr(ri); continue;
                    }
                    val=jvu(); continue;
                }

                /* String / number methods */
                val=bstr(&val,member,args,na); continue;
            }

            /* property get (no call) */
            if (val.t==VT_ARR) {
                if (!strcmp(member,"length")) { int ai=(int)val.d; val=jvd((double)(ai>=0&&ai<JA_N?s_arr_pool[ai].len:0)); continue; }
                val=jvu(); continue;
            }
            if (val.t==VT_OBJ) {
                if (!strcmp(member,"getFullYear"))  { val=bobj_get(&val,"$fy"); continue; }
                if (!strcmp(member,"getMonth"))     { val=bobj_get(&val,"$fm"); continue; }
                if (!strcmp(member,"getDate"))      { val=bobj_get(&val,"$fd"); continue; }
                if (!strcmp(member,"getHours"))     { val=bobj_get(&val,"$fh"); continue; }
                if (!strcmp(member,"getMinutes"))   { val=bobj_get(&val,"$fn"); continue; }
                if (!strcmp(member,"getSeconds"))   { val=bobj_get(&val,"$fs"); continue; }
                val=bobj_get(&val,member); continue;
            }
            if (!strcmp(sv,"[doc]")) {
                if (!strcmp(member,"title")) { val=x->res?jvs(x->res->title):jvs(""); continue; }
                val=jvu(); continue;
            }
            if (is_dom_ref(&val)) {
                char id[64], buf[NS];
                if (!dom_ref_id(&val, id, sizeof(id))) { val = jvu(); continue; }
                if (!strcmp(member,"innerHTML") || !strcmp(member,"innerText") ||
                    !strcmp(member,"textContent") || !strcmp(member,"value")) {
                    if (win95_html_dom_get_attr(id, member, buf, sizeof(buf))) val = jvs(buf);
                    else val = jvs("");
                    continue;
                }
                if (!strcmp(member,"id") || !strcmp(member,"name") || !strcmp(member,"type")) {
                    if (win95_html_dom_get_attr(id, member, buf, sizeof(buf))) val = jvs(buf);
                    else val = jvs("");
                    continue;
                }
                val = jvu(); continue;
            }
            if (!strcmp(sv,"[win]")) {
                if (!strcmp(member,"location")) { val=jvs("[loc]"); continue; }
                val=jvu(); continue;
            }
            if (!strcmp(sv,"[loc]")) {
                if (!strcmp(member,"href")||!strcmp(member,"pathname")||!strcmp(member,"host"))
                    { val=x->res?jvs(x->res->navigate_url):jvs(""); continue; }
                val=jvu(); continue;
            }
            if (!strcmp(sv,"[Math]")) {
                if (!strcmp(member,"PI"))  { val=jvd(3.14159265358979); continue; }
                if (!strcmp(member,"E"))   { val=jvd(2.71828182845905); continue; }
                val=jvu(); continue;
            }
            if (!strcmp(member,"length")) {
                char t[NS]; jvtos(&val,t,sizeof(t)); val=jvd((double)strlen(t)); continue;
            }
            val=jvu(); continue;
        }

        /* subscript */
        if (c=='[') {
            jsx_eat(x); JV idx=eval_assign(x); jsx_ws(x);
            if (jsx_p(x)==']') jsx_eat(x);
            if (val.t==VT_ARR) {
                int ai=(int)val.d; JA*ja=(ai>=0&&ai<JA_N)?&s_arr_pool[ai]:NULL;
                unsigned i=(unsigned)jvtod(&idx);
                val=(ja&&i<ja->len)?jv_from_jvi(&ja->items[i]):jvu(); continue;
            }
            if (val.t==VT_OBJ) {
                char key[NM]; jvtos(&idx,key,sizeof(key)); val=bobj_get(&val,key); continue;
            }
            char t[NS]; jvtos(&val,t,sizeof(t));
            unsigned i=(unsigned)jvtod(&idx); char ch[2]={0,0};
            if (i<strlen(t)) ch[0]=t[i]; val=jvs(ch); continue;
        }

        /* direct call on a function value */
        if (c=='('&&val.t==VT_FN) {
            jsx_eat(x); JV args[8]; unsigned na=eval_args(x,args,8);
            int fi=find_fn(x,val.s); val=fi>=0?call_fn(x,(unsigned)fi,args,na):jvu(); continue;
        }

        break;
    }
    return val;
}

/* -------------------------------------------------------------------------
 * Unary
 * ---------------------------------------------------------------------- */
static JV eval_unary(JSX*x) {
    jsx_ws(x); char c=jsx_p(x), c2=jsx_p2(x);
    if (c=='!'&&c2!='=') { jsx_eat(x); JV v=eval_unary(x); return jvb(!jvtruthy(&v)); }
    if (c=='-'&&c2!='-'&&c2!='=') { jsx_eat(x); JV v=eval_unary(x); return jvd(-jvtod(&v)); }
    if (c=='+'&&c2!='+'&&c2!='=') { jsx_eat(x); JV v=eval_unary(x); return jvd(jvtod(&v)); }
    if (c=='~') { jsx_eat(x); JV v=eval_unary(x); return jvd((double)(~(int)jvtod(&v))); }
    /* prefix ++ / -- */
    if ((c=='+'&&c2=='+')||(c=='-'&&c2=='-')) {
        jsx_eat(x); jsx_eat(x); jsx_ws(x);
        char nm[NM]; read_id(x,nm,sizeof(nm));
        JV v=var_get(x,nm); double d=jvtod(&v)+(c=='+'?1.0:-1.0);
        var_set(x,nm,jvd(d)); return jvd(d);
    }
    return eval_call(x);
}

static JV eval_mul(JSX*x) {
    JV l=eval_unary(x);
    while (!jsx_end(x)) {
        jsx_ws(x); char c=jsx_p(x),c2=jsx_p2(x);
        if (c=='*'&&c2!='*'&&c2!='=') { jsx_eat(x); JV r=eval_unary(x); l=jvd(jvtod(&l)*jvtod(&r)); continue; }
        if (c=='/'&&c2!='/'&&c2!='*'&&c2!='=') { jsx_eat(x); JV r=eval_unary(x); double d=jvtod(&r); l=jvd(d?jvtod(&l)/d:0.0); continue; }
        if (c=='%'&&c2!='=') { jsx_eat(x); JV r=eval_unary(x); long long a=(long long)jvtod(&l),b=(long long)jvtod(&r); l=jvd((double)(b?a%b:0)); continue; }
        break;
    }
    return l;
}

static JV eval_add(JSX*x) {
    JV l=eval_mul(x);
    while (!jsx_end(x)) {
        jsx_ws(x); char c=jsx_p(x),c2=jsx_p2(x);
        if (c=='+'&&c2!='+'&&c2!='=') {
            jsx_eat(x); JV r=eval_mul(x);
            if (l.t==VT_S||r.t==VT_S) { char a[NS],b[NS],out[NS]; jvtos(&l,a,sizeof(a)); jvtos(&r,b,sizeof(b)); snprintf(out,sizeof(out),"%s%s",a,b); l=jvs(out); }
            else l=jvd(jvtod(&l)+jvtod(&r)); continue;
        }
        if (c=='-'&&c2!='-'&&c2!='=') { jsx_eat(x); JV r=eval_mul(x); l=jvd(jvtod(&l)-jvtod(&r)); continue; }
        break;
    }
    return l;
}

static JV eval_rel(JSX*x) {
    JV l=eval_add(x);
    while (!jsx_end(x)) {
        jsx_ws(x); char c=jsx_p(x),c2=jsx_p2(x);
        if (c=='<'&&c2!='='&&c2!='<') { jsx_eat(x); JV r=eval_add(x); l=jvb(jvtod(&l)<jvtod(&r)); continue; }
        if (c=='>'&&c2!='='&&c2!='>') { jsx_eat(x); JV r=eval_add(x); l=jvb(jvtod(&l)>jvtod(&r)); continue; }
        if (c=='<'&&c2=='=') { jsx_eat(x);jsx_eat(x); JV r=eval_add(x); l=jvb(jvtod(&l)<=jvtod(&r)); continue; }
        if (c=='>'&&c2=='=') { jsx_eat(x);jsx_eat(x); JV r=eval_add(x); l=jvb(jvtod(&l)>=jvtod(&r)); continue; }
        break;
    }
    return l;
}

static JV eval_eq(JSX*x) {
    JV l=eval_rel(x);
    while (!jsx_end(x)) {
        jsx_ws(x); char c=jsx_p(x),c2=jsx_p2(x),c3=(x->pos+2<x->len)?x->src[x->pos+2]:0;
        int eq=0,neq=0;
        if (c=='='&&c2=='='&&c3=='='){jsx_eat(x);jsx_eat(x);jsx_eat(x);eq=1;}
        else if(c=='='&&c2=='='){jsx_eat(x);jsx_eat(x);eq=1;}
        else if(c=='!'&&c2=='='&&c3=='='){jsx_eat(x);jsx_eat(x);jsx_eat(x);neq=1;}
        else if(c=='!'&&c2=='='){jsx_eat(x);jsx_eat(x);neq=1;}
        else break;
        JV r=eval_rel(x);
        int same;
        if (l.t==VT_D&&r.t==VT_D) same=(l.d==r.d);
        else if (l.t==VT_S&&r.t==VT_S) same=!strcmp(l.s,r.s);
        else if ((l.t==VT_U||l.t==VT_N)&&(r.t==VT_U||r.t==VT_N)) same=1;
        else { same=(jvtod(&l)==jvtod(&r)); }
        l=jvb(eq?same:!same);
    }
    return l;
}

static JV eval_and(JSX*x) {
    JV l=eval_eq(x);
    while (!jsx_end(x)) { jsx_ws(x); if (jsx_p(x)!='&'||jsx_p2(x)!='&') break; jsx_eat(x);jsx_eat(x); if(!jvtruthy(&l)){eval_eq(x); return l;} l=eval_eq(x); }
    return l;
}
static JV eval_or(JSX*x) {
    JV l=eval_and(x);
    while (!jsx_end(x)) { jsx_ws(x); if (jsx_p(x)!='|'||jsx_p2(x)!='|') break; jsx_eat(x);jsx_eat(x); if(jvtruthy(&l)){eval_and(x); return l;} l=eval_and(x); }
    return l;
}
static JV eval_ternary(JSX*x) {
    JV c=eval_or(x); jsx_ws(x);
    if (jsx_p(x)!='?') return c;
    jsx_eat(x); JV t=eval_ternary(x); jsx_ws(x);
    if (jsx_p(x)==':') jsx_eat(x);
    JV f=eval_ternary(x); return jvtruthy(&c)?t:f;
}

/* member property set helper */
static void member_set(JSX*x, const char*obj, const char*prop, JV val) {
    if (!strcmp(obj,"[doc]")||!strcmp(obj,"document")) {
        if (!strcmp(prop,"title")&&x->res) { jvtos(&val,x->res->title,JS_TITLE_MAX-1); x->res->has_title=TRUE; }
    } else if (!strncmp(obj, "[dom:", 5)) {
        char id[64], buf[NS];
        JV tmp = jvs(obj);
        if (dom_ref_id(&tmp, id, sizeof(id))) {
            jvtos(&val, buf, sizeof(buf));
            win95_html_dom_set_attr(id, prop, buf);
        }
    } else if (!strcmp(obj,"[win]")||!strcmp(obj,"window")) {
        if (!strcmp(prop,"location")&&x->res) { jvtos(&val,x->res->navigate_url,JS_URL_MAX-1); x->res->has_navigate=TRUE; }
    } else if (!strcmp(obj,"[loc]")||!strcmp(obj,"location")) {
        if ((!strcmp(prop,"href")||!strcmp(prop,"assign"))&&x->res) { jvtos(&val,x->res->navigate_url,JS_URL_MAX-1); x->res->has_navigate=TRUE; }
    }
}

static JV eval_assign(JSX*x) {
    unsigned saved=x->pos; jsx_ws(x); char c=jsx_p(x);

    if (is_id_s(c)) {
        char n1[NM],n2[NM],n3[NM]; n1[0]=n2[0]=n3[0]=0;
        read_id(x,n1,sizeof(n1)); jsx_ws(x);
        if (jsx_p(x)=='.') {
            jsx_eat(x); read_id(x,n2,sizeof(n2)); jsx_ws(x);
            if (jsx_p(x)=='.') { jsx_eat(x); read_id(x,n3,sizeof(n3)); jsx_ws(x); }
        } else if (jsx_p(x)=='[') {
            /* subscript assignment: name[idx] = val  or  name[idx] op= val */
            jsx_eat(x); JV sidx=eval_assign(x); jsx_ws(x);
            if(jsx_p(x)==']') jsx_eat(x); jsx_ws(x);
            char sop=jsx_p(x),sop2=jsx_p2(x);
            if(sop=='='&&sop2!='=') {
                jsx_eat(x); JV sv2=eval_assign(x);
                JV sarr=var_get(x,n1);
                if(sarr.t==VT_ARR){
                    int ai=(int)sarr.d; JA*ja=(ai>=0&&ai<JA_N)?&s_arr_pool[ai]:NULL;
                    if(ja){ unsigned ii=(unsigned)jvtod(&sidx);
                        if(ii<JA_M){ JVI uv; uv.t=VT_U; uv.d=0; uv.s[0]=0;
                            while(ja->len<=ii&&ja->len<JA_M) ja->items[ja->len++]=uv;
                            if(ii<JA_M) ja->items[ii]=jvi_from_jv(&sv2); }
                    }
                } else if(sarr.t==VT_OBJ) {
                    char sk[NM]; jvtos(&sidx,sk,sizeof(sk));
                    bobj_set(&sarr,sk,sv2);
                }
                return sv2;
            }
            x->pos=saved; /* not assignment */
        }

        char op=jsx_p(x),op2=jsx_p2(x);

        /* assignment operators */
        if (op=='='&&op2!='=') {
            jsx_eat(x); JV v=eval_assign(x);
            if (!n2[0]) var_set(x,n1,v);
            else if (!n3[0]) {
                /* obj.prop = val */
                JV obj=var_get(x,n1);
                if (obj.t==VT_OBJ) { bobj_set(&obj,n2,v); var_set(x,n1,obj); }
                else if (obj.t==VT_ARR) {
                    /* arr.push/pop used as property is rare; handle length=noop */
                } else {
                    char sv[NS]; jvtos(&obj,sv,sizeof(sv));
                    member_set(x,sv,n2,v);
                    member_set(x,n1,n2,v);
                }
            } else {
                /* obj.prop.sub = val */
                if (!strcmp(n1,"window")&&!strcmp(n2,"location")&&!strcmp(n3,"href")&&x->res) {
                    jvtos(&v,x->res->navigate_url,JS_URL_MAX-1); x->res->has_navigate=TRUE;
                }
            }
            return v;
        }
        /* augmented */
        if ((op=='+'||op=='-'||op=='*'||op=='/')&&op2=='=') {
            jsx_eat(x);jsx_eat(x); JV old=var_get(x,n1),rhs=eval_assign(x),v;
            if (op=='+'&&(old.t==VT_S||rhs.t==VT_S)){char a[NS],b[NS],out[NS];jvtos(&old,a,sizeof(a));jvtos(&rhs,b,sizeof(b));snprintf(out,sizeof(out),"%s%s",a,b);v=jvs(out);}
            else { double lo=jvtod(&old),lr=jvtod(&rhs); v=jvd(op=='+'?lo+lr:op=='-'?lo-lr:op=='*'?lo*lr:(lr?lo/lr:0.0)); }
            var_set(x,n1,v); return v;
        }
        /* post-increment */
        if (op=='+'&&op2=='+') { jsx_eat(x);jsx_eat(x); JV old=var_get(x,n1); var_set(x,n1,jvd(jvtod(&old)+1)); return old; }
        if (op=='-'&&op2=='-') { jsx_eat(x);jsx_eat(x); JV old=var_get(x,n1); var_set(x,n1,jvd(jvtod(&old)-1)); return old; }

        x->pos=saved;
    } else { x->pos=saved; }

    return eval_ternary(x);
}

/* -------------------------------------------------------------------------
 * Statement executor
 * ---------------------------------------------------------------------- */
static void exec_block(JSX*x) {
    jsx_ws(x);
    if (jsx_p(x)=='{') {
        jsx_eat(x);
        while (!jsx_end(x)&&!x->ret_f&&!x->brk_f&&!x->cont_f) {
            jsx_ws(x); if (jsx_p(x)=='}') { jsx_eat(x); return; }
            exec_stmt(x);
        }
        /* skip to matching } */
        while (!jsx_end(x)&&jsx_p(x)!='}') {
            char c=jsx_p(x);
            if (c=='"'||c=='\''||c=='`'){jsx_eat(x);skip_str_body(x,c);}
            else jsx_eat(x);
        }
        if (!jsx_end(x)) jsx_eat(x); /* consume } */
    } else {
        exec_stmt(x);
    }
}

static void exec_stmt(JSX*x) {
    jsx_ws(x); if (jsx_end(x)) return;
    char c=jsx_p(x);

    /* block */
    if (c=='{') { exec_block(x); return; }

    /* semicolon / empty */
    if (c==';') { jsx_eat(x); return; }

    char kw[NM]="";
    unsigned kp=x->pos;
    if (is_id_s(c)) read_id(x,kw,sizeof(kw));

    /* var / let / const */
    if (!strcmp(kw,"var")||!strcmp(kw,"let")||!strcmp(kw,"const")) {
        do {
            jsx_ws(x); if (!is_id_s(jsx_p(x))) break;
            char nm[NM]; read_id(x,nm,sizeof(nm)); jsx_ws(x);
            if (jsx_p(x)=='=') { jsx_eat(x); JV v=eval_assign(x); var_set(x,nm,v); }
            else var_set(x,nm,jvu());
            jsx_ws(x);
        } while (jsx_p(x)==','&&jsx_eat(x));
        if (jsx_p(x)==';') jsx_eat(x);
        return;
    }

    /* function declaration */
    if (!strcmp(kw,"function")) {
        jsx_ws(x); char fname[NM]=""; read_id(x,fname,sizeof(fname)); jsx_ws(x);
        char pn[8][NM]; unsigned np=0;
        if (jsx_p(x)=='(') {
            jsx_eat(x); jsx_ws(x);
            while (jsx_p(x)!=')'&&!jsx_end(x)) {
                if (np<8&&is_id_s(jsx_p(x))) read_id(x,pn[np++],NM);
                jsx_ws(x); if (jsx_p(x)==','){jsx_eat(x);jsx_ws(x);}
            }
            if (jsx_p(x)==')') jsx_eat(x);
        }
        jsx_ws(x);
        unsigned bs=x->pos;
        if (fname[0]&&x->nf<NF) {
            strncpy(x->fn[x->nf].name,fname,NM-1);
            x->fn[x->nf].bs=bs; x->fn[x->nf].np=np;
            for(unsigned i=0;i<np;i++) strncpy(x->fn[x->nf].pn[i],pn[i],NM-1);
            x->nf++;
        }
        skip_block(x);
        return;
    }

    /* return */
    if (!strcmp(kw,"return")) {
        jsx_ws(x);
        if (jsx_p(x)!=';'&&jsx_p(x)!='}'&&jsx_p(x)!='\n'&&!jsx_end(x))
            x->ret_v=eval_assign(x);
        else x->ret_v=jvu();
        x->ret_f=1;
        if (jsx_p(x)==';') jsx_eat(x);
        return;
    }

    /* break / continue */
    if (!strcmp(kw,"break"))    { x->brk_f=1;  if (jsx_p(x)==';')jsx_eat(x); return; }
    if (!strcmp(kw,"continue")) { x->cont_f=1; if (jsx_p(x)==';')jsx_eat(x); return; }

    /* switch */
    if (!strcmp(kw,"switch")) {
        jsx_ws(x); if (jsx_p(x)=='(') jsx_eat(x);
        JV disc=eval_assign(x); jsx_ws(x); if (jsx_p(x)==')') jsx_eat(x);
        char ds[NS]; jvtos(&disc,ds,sizeof(ds));
        jsx_ws(x); if (jsx_p(x)=='{') jsx_eat(x);
        /* scan for matching case */
        int matched=0, has_default=0; unsigned default_pos=0;
        while (!jsx_end(x)&&jsx_p(x)!='}') {
            jsx_ws(x); if (jsx_p(x)=='}') break;
            char ck[NM]; unsigned cp=x->pos;
            if (!is_id_s(jsx_p(x))) { jsx_eat(x); continue; }
            read_id(x,ck,sizeof(ck)); jsx_ws(x);
            if (!strcmp(ck,"case")) {
                JV cv=eval_assign(x); jsx_ws(x); if(jsx_p(x)==':')jsx_eat(x);
                if (!matched) {
                    char cs[NS]; jvtos(&cv,cs,sizeof(cs));
                    if (!strcmp(cs,ds)) matched=1;
                }
                if (matched) {
                    /* execute stmts until break or next case/default/} */
                    while (!jsx_end(x)&&!x->brk_f&&!x->ret_f) {
                        jsx_ws(x); if(jsx_p(x)=='}') break;
                        unsigned pp3=x->pos;
                        if (is_id_s(jsx_p(x))) {
                            char nk[NM]; read_id(x,nk,sizeof(nk)); jsx_ws(x);
                            if (!strcmp(nk,"case")||!strcmp(nk,"default")) { x->pos=pp3; break; }
                            x->pos=pp3;
                        } else { x->pos=pp3; }
                        exec_stmt(x);
                    }
                    if (x->brk_f) { x->brk_f=0; }
                    break;
                }
            } else if (!strcmp(ck,"default")) {
                if(jsx_p(x)==':') jsx_eat(x);
                has_default=1; default_pos=x->pos;
                /* skip ahead to look for matching case first */
            } else { x->pos=cp; jsx_eat(x); }
        }
        if (!matched&&has_default) {
            x->pos=default_pos;
            while (!jsx_end(x)&&!x->brk_f&&!x->ret_f) {
                jsx_ws(x); if(jsx_p(x)=='}') break;
                unsigned pp4=x->pos;
                if (is_id_s(jsx_p(x))) {
                    char nk[NM]; read_id(x,nk,sizeof(nk)); jsx_ws(x);
                    if (!strcmp(nk,"case")||!strcmp(nk,"default")) { x->pos=pp4; break; }
                    x->pos=pp4;
                } else { x->pos=pp4; }
                exec_stmt(x);
            }
            if (x->brk_f) x->brk_f=0;
        }
        /* skip to closing } */
        while (!jsx_end(x)&&jsx_p(x)!='}') {
            char cc=jsx_p(x);
            if(cc=='"'||cc=='\''||cc=='`'){jsx_eat(x);skip_str_body(x,cc);}
            else jsx_eat(x);
        }
        if (!jsx_end(x)) jsx_eat(x);
        return;
    }

    /* if */
    if (!strcmp(kw,"if")) {
        jsx_ws(x); if (jsx_p(x)=='(') jsx_eat(x);
        JV cond=eval_assign(x); jsx_ws(x); if (jsx_p(x)==')') jsx_eat(x);
        if (jvtruthy(&cond)) {
            exec_block(x);
            jsx_ws(x);
            if (is_id_s(jsx_p(x))) {
                unsigned ep=x->pos; char ek[NM]; read_id(x,ek,sizeof(ek));
                if (!strcmp(ek,"else")) skip_block(x);
                else x->pos=ep;
            }
        } else {
            skip_block(x); jsx_ws(x);
            if (is_id_s(jsx_p(x))) {
                unsigned ep=x->pos; char ek[NM]; read_id(x,ek,sizeof(ek));
                if (!strcmp(ek,"else")) exec_block(x);
                else x->pos=ep;
            }
        }
        return;
    }

    /* while */
    if (!strcmp(kw,"while")) {
        unsigned loop_start=x->pos;
        for (unsigned iter=0;iter<NIT;iter++) {
            x->pos=loop_start; jsx_ws(x);
            if (jsx_p(x)=='(') jsx_eat(x);
            JV cond=eval_assign(x); jsx_ws(x); if (jsx_p(x)==')') jsx_eat(x);
            if (!jvtruthy(&cond)) { skip_block(x); return; }
            x->brk_f=0; x->cont_f=0;
            exec_block(x);
            if (x->ret_f) return;
            if (x->brk_f) { x->brk_f=0; return; }
        }
        return;
    }

    /* for */
    if (!strcmp(kw,"for")) {
        jsx_ws(x); if (jsx_p(x)=='(') jsx_eat(x);
        /* detect for-in: for (var k in obj) */
        jsx_ws(x);
        unsigned for_start=x->pos;
        {
            unsigned pp=x->pos; char kk[NM]=""; char varnm[NM]="";
            if (is_id_s(jsx_p(x))) read_id(x,kk,sizeof(kk));
            if (!strcmp(kk,"var")||!strcmp(kk,"let")||!strcmp(kk,"const")) {
                jsx_ws(x); if (is_id_s(jsx_p(x))) read_id(x,varnm,sizeof(varnm));
            } else { strncpy(varnm,kk,NM-1); }
            jsx_ws(x);
            char ink[NM]=""; if(is_id_s(jsx_p(x))) read_id(x,ink,sizeof(ink));
            if (!strcmp(ink,"in")) {
                /* for-in */
                jsx_ws(x); JV obj=eval_assign(x); jsx_ws(x); if(jsx_p(x)==')') jsx_eat(x);
                unsigned body_pos=x->pos;
                if (obj.t==VT_OBJ) {
                    int oi=(int)obj.d; JO*jo=(oi>=0&&oi<JO_N)?&s_obj_pool[oi]:NULL;
                    if (jo) for(unsigned ki=0;ki<jo->len&&!x->brk_f&&!x->ret_f;ki++) {
                        var_set(x,varnm,jvs(jo->keys[ki]));
                        x->pos=body_pos; x->cont_f=0;
                        exec_block(x);
                        if (x->brk_f) { x->brk_f=0; break; }
                    }
                } else if (obj.t==VT_ARR) {
                    int ai=(int)obj.d; JA*ja=(ai>=0&&ai<JA_N)?&s_arr_pool[ai]:NULL;
                    if (ja) for(unsigned ki=0;ki<ja->len&&!x->brk_f&&!x->ret_f;ki++) {
                        char idx[16]; snprintf(idx,sizeof(idx),"%u",ki);
                        var_set(x,varnm,jvs(idx));
                        x->pos=body_pos; x->cont_f=0;
                        exec_block(x);
                        if (x->brk_f) { x->brk_f=0; break; }
                    }
                } else {
                    x->pos=body_pos; skip_block(x);
                }
                return;
            }
            x->pos=pp; /* not for-in, restore and parse normally */
        }
        /* init */
        jsx_ws(x);
        if (jsx_p(x)!=';') {
            if (is_id_s(jsx_p(x))) {
                unsigned pp=x->pos; char kk[NM]; read_id(x,kk,sizeof(kk)); jsx_ws(x);
                if (!strcmp(kk,"var")||!strcmp(kk,"let")||!strcmp(kk,"const")) {
                    jsx_ws(x); char nm[NM]; read_id(x,nm,sizeof(nm)); jsx_ws(x);
                    if (jsx_p(x)=='='){jsx_eat(x);JV v=eval_assign(x);var_set(x,nm,v);}
                    else var_set(x,nm,jvu());
                } else { x->pos=pp; eval_assign(x); }
            } else eval_assign(x);
        }
        if (jsx_p(x)==';') jsx_eat(x);

        unsigned cond_start=x->pos;
        /* find update start by scanning to second ; */
        unsigned pp2=x->pos; int dep=0;
        while (!jsx_end(x)) {
            char cc=jsx_p(x);
            if (cc=='"'||cc=='\''||cc=='`'){jsx_eat(x);skip_str_body(x,cc);continue;}
            if (cc=='('||cc=='[') dep++;
            else if (cc==')'||cc==']') { if (!dep) break; dep--; }
            else if (cc==';'&&!dep) break;
            jsx_eat(x);
        }
        unsigned cond_end=x->pos; if (jsx_p(x)==';') jsx_eat(x);
        unsigned update_start=x->pos;
        /* skip update expr to find body */
        dep=0;
        while (!jsx_end(x)) {
            char cc=jsx_p(x);
            if (cc=='"'||cc=='\''||cc=='`'){jsx_eat(x);skip_str_body(x,cc);continue;}
            if (cc=='(') dep++;
            else if (cc==')') { if(!dep) break; dep--; }
            jsx_eat(x);
        }
        unsigned update_end=x->pos; if (jsx_p(x)==')') jsx_eat(x);
        unsigned body_start=x->pos;

        for (unsigned iter=0;iter<NIT;iter++) {
            /* evaluate condition */
            if (cond_end>cond_start) {
                x->pos=cond_start;
                JV cond=eval_assign(x);
                if (!jvtruthy(&cond)) { x->pos=body_start; skip_block(x); return; }
            }
            /* execute body */
            x->pos=body_start; x->brk_f=0; x->cont_f=0;
            exec_block(x);
            if (x->ret_f) return;
            if (x->brk_f) { x->brk_f=0; return; }
            /* execute update */
            if (update_end>update_start) { x->pos=update_start; eval_assign(x); }
        }
        x->pos=body_start; skip_block(x);
        return;
    }

    /* do-while (simplified) */
    if (!strcmp(kw,"do")) {
        unsigned body_start=x->pos;
        for (unsigned iter=0;iter<NIT;iter++) {
            x->pos=body_start; x->brk_f=0; x->cont_f=0;
            exec_block(x);
            if (x->ret_f) return;
            if (x->brk_f) { x->brk_f=0; return; }
            jsx_ws(x); char wkw[NM]=""; if (is_id_s(jsx_p(x))) read_id(x,wkw,sizeof(wkw));
            if (!strcmp(wkw,"while")) {
                jsx_ws(x); if (jsx_p(x)=='(') jsx_eat(x);
                JV cond=eval_assign(x); jsx_ws(x); if (jsx_p(x)==')') jsx_eat(x);
                if (jsx_p(x)==';') jsx_eat(x);
                if (!jvtruthy(&cond)) return;
            } else return;
        }
        return;
    }

    /* try-catch (skip body, handle error gracefully) */
    if (!strcmp(kw,"try")) {
        exec_block(x); jsx_ws(x);
        if (is_id_s(jsx_p(x))) {
            char ck[NM]; unsigned cp=x->pos; read_id(x,ck,sizeof(ck));
            if (!strcmp(ck,"catch")) { jsx_ws(x); if(jsx_p(x)=='('){jsx_eat(x);while(!jsx_end(x)&&jsx_p(x)!=')')jsx_eat(x);if(!jsx_end(x))jsx_eat(x);} skip_block(x); }
            else x->pos=cp;
        }
        jsx_ws(x);
        if (is_id_s(jsx_p(x))) {
            char fk[NM]; unsigned fp=x->pos; read_id(x,fk,sizeof(fk));
            if (!strcmp(fk,"finally")) exec_block(x); else x->pos=fp;
        }
        return;
    }

    /* throw (ignore value) */
    if (!strcmp(kw,"throw")) { eval_assign(x); return; }

    /* expression statement */
    x->pos=kp;
    eval_assign(x);
    jsx_ws(x);
    if (jsx_p(x)==';') jsx_eat(x);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */
VOID_T win95_js_run(CONST CHAR_T *script, WIN95_JS_RESULT_T *result)
{
    if (!script||!result) return;

    /* Reset array/object pools for this run */
    memset(s_arr_used, 0, sizeof(s_arr_used));
    memset(s_obj_used, 0, sizeof(s_obj_used));

    JSX x;
    memset(&x,0,sizeof(x));
    x.src=(const char*)script;
    x.len=(unsigned)strlen(script);
    x.res=result;

    while (!jsx_end(&x)&&!x.ret_f) {
        jsx_ws(&x);
        if (jsx_end(&x)) break;
        exec_stmt(&x);
    }
}

VOID_T win95_js_result_free(WIN95_JS_RESULT_T *result)
{
    if (!result) return;
    if (result->write_buf) { tal_free(result->write_buf); result->write_buf=NULL; result->write_len=0; }
}
