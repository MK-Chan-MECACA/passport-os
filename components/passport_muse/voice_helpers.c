/* Passport OS UTF-8 captions; preserves Chinese rather than converting to ASCII. */
#include "muse_chat_priv.h"
#include <string.h>
static void le(uint8_t *p,uint32_t v,int n){for(int i=0;i<n;i++)p[i]=(uint8_t)(v>>(8*i));}
void muse_hatch_wav_header(uint8_t h[MUSE_HATCH_WAV_HEADER],uint32_t rate){
    memcpy(h,"RIFF",4);le(h+4,UINT32_MAX,4);memcpy(h+8,"WAVEfmt ",8);le(h+16,16,4);
    le(h+20,1,2);le(h+22,1,2);le(h+24,rate,4);le(h+28,rate*2,4);le(h+32,2,2);le(h+34,16,2);
    memcpy(h+36,"data",4);le(h+40,UINT32_MAX,4);
}
size_t muse_hatch_base64(const uint8_t *p,size_t n,char *o){
    static const char a[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t k=0;for(size_t i=0;i<n;i+=3){uint32_t v=(uint32_t)p[i]<<16;
        if(i+1<n)v|=(uint32_t)p[i+1]<<8;
        if(i+2<n)v|=p[i+2];
        o[k++]=a[v>>18];o[k++]=a[v>>12&63];o[k++]=i+1<n?a[v>>6&63]:'=';o[k++]=i+2<n?a[v&63]:'=';}
    return k;
}
void muse_hatch_tail_words(const char *src,char *out,size_t cap){
    if(!cap)return;
    size_t len=strlen(src);const char *p=src;
    if(len>=cap){p=src+len-cap+1;while(((unsigned char)*p&0xc0)==0x80)p++;}
    size_t n = strlen(p);
    if(n >= cap)n=cap-1;
    memcpy(out,p,n);out[n]=0;
}
/* Five lines at the 216 px / 16 px reply label. Widths are buddy_font_16
 * advances (rounded up); anything outside ASCII counts as 16 px. Lines wrap at
 * the last space so English words stay whole. Return the next byte offset for
 * manual paging; never split a UTF-8 glyph. */
static size_t utf8_bytes(const char *p) {
    unsigned char c = (unsigned char)*p;
    size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    for (size_t i = 1; i < n; i++)
        if (!p[i] || ((unsigned char)p[i] & 0xc0) != 0x80) return 1;
    return n;
}
static const unsigned char ascii_px[95] = {
    4,5,8,9,9,15,11,5,6,6,8,9,5,6,5,7,9,9,9,9,9,9,9,9,9,9,5,5,9,9,9,8,
    15,10,11,11,11,10,9,11,12,5,9,11,9,13,12,12,10,12,11,10,10,12,10,14,9,9,10,6,7,6,9,9,
    10,9,10,9,10,9,6,9,10,5,5,9,5,15,10,10,10,10,7,8,6,10,9,13,8,9,8,6,5,6,9};
size_t muse_hatch_reply_page(const char *text, size_t at, char *out, size_t cap) {
    if (!cap) return at;
    out[0] = 0;
    size_t len = strlen(text);
    if (at >= len) return len;
    while (at && ((unsigned char)text[at] & 0xc0) == 0x80) at--;
    size_t used = 0, space_used = 0, space_at = 0;
    unsigned px = 0, lines = 1;
    bool space = false;  /* a space on this line we can wrap at */
    while (at < len) {
        size_t n = utf8_bytes(text + at);
        unsigned char c = (unsigned char)text[at];
        unsigned width = c >= 0x20 && c < 0x7f ? ascii_px[c - 0x20] : n == 1 ? 0 : 16;
        bool newline = c == '\n';
        if (newline || px + width > 216) {
            size_t resume = newline || c == ' ' ? at + 1 : at;
            if (!newline && c != ' ' && space) { used = space_used; resume = space_at + 1; }
            if (lines == 5) { at = resume; break; }
            if (used + 1 >= cap) break;
            out[used++] = '\n'; lines++; px = 0; space = false; at = resume;
            continue;
        }
        if (used + n >= cap) break;
        if (c == ' ' && px) { space = true; space_used = used; space_at = at; }
        memcpy(out + used, text + at, n); used += n; at += n; px += width;
    }
    out[used] = 0;
    return at;
}
bool muse_hatch_caption_at(const char *src,size_t at,char *out,size_t cap){
    if (!*src || !cap) return false;
    size_t start = 0, next, len = strlen(src);
    if (at >= len) at = len - 1;
    do {
        next = muse_hatch_reply_page(src, start, out, cap);
        if (next > at || next <= start || next == len) break;
        start = next;
    } while (start < len);
    return out[0] != 0;
}
