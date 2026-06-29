// png_decode.h — tiny self-contained PNG decoder. Supports 8-bit non-interlaced
// PNGs of colour type 0 (grey), 2 (RGB), 3 (palette), 6 (RGBA), with optional
// tRNS. Includes a from-scratch DEFLATE/zlib inflater. Output is always RGBA8.
// No external dependencies.
#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <cstdlib>

namespace png {

struct Image { int w=0, h=0; std::vector<uint8_t> rgba; bool ok=false; std::string error; };

// ----------------------------- DEFLATE inflate -----------------------------
struct BitReader {
    const uint8_t* d; size_t n; size_t pos=0; uint32_t buf=0; int cnt=0; bool bad=false;
    BitReader(const uint8_t* p, size_t len): d(p), n(len) {}
    int bit(){ if(cnt==0){ if(pos>=n){ bad=true; return 0; } buf=d[pos++]; cnt=8; } int b=buf&1; buf>>=1; --cnt; return b; }
    uint32_t bits(int c){ uint32_t v=0; for(int i=0;i<c;++i) v|=((uint32_t)bit())<<i; return v; }
    void align(){ cnt=0; }
};

struct Huff {
    int counts[16]={0};
    std::vector<int> symbols;
    void build(const int* lengths, int num){
        for(int i=0;i<16;++i) counts[i]=0;
        for(int i=0;i<num;++i) counts[lengths[i]]++;
        counts[0]=0;
        int offs[16]; offs[0]=0; int sum=0;
        for(int i=1;i<16;++i){ offs[i]=sum; sum+=counts[i]; }
        symbols.assign(num,0);
        for(int i=0;i<num;++i) if(lengths[i]) symbols[offs[lengths[i]]++]=i;
    }
    int decode(BitReader& br) const {
        int code=0, first=0, index=0;
        for(int len=1; len<=15; ++len){
            code |= br.bit();
            int count=counts[len];
            if(code - first < count) return symbols[index + (code - first)];
            index += count; first += count; first <<= 1; code <<= 1;
        }
        return -1;
    }
};

inline bool inflate(const uint8_t* src, size_t len, std::vector<uint8_t>& out){
    static const int lenBase[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const int lenExtra[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const int distBase[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const int distExtra[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    static const int clOrder[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

    BitReader br(src, len);
    for(;;){
        int final=br.bit();
        int type=(int)br.bits(2);
        if(type==0){                                  // stored
            br.align();
            if(br.pos+4>br.n) return false;
            int l = src[br.pos] | (src[br.pos+1]<<8); br.pos+=2; br.pos+=2; // LEN, skip NLEN
            if(br.pos+l>br.n) return false;
            out.insert(out.end(), src+br.pos, src+br.pos+l); br.pos+=l;
        } else if(type==1 || type==2){
            Huff lit, dist;
            if(type==1){
                int ll[288]; for(int i=0;i<144;++i) ll[i]=8; for(int i=144;i<256;++i) ll[i]=9;
                for(int i=256;i<280;++i) ll[i]=7; for(int i=280;i<288;++i) ll[i]=8;
                int dl[30]; for(int i=0;i<30;++i) dl[i]=5;
                lit.build(ll,288); dist.build(dl,30);
            } else {
                int hlit=(int)br.bits(5)+257, hdist=(int)br.bits(5)+1, hclen=(int)br.bits(4)+4;
                int cl[19]={0}; for(int i=0;i<hclen;++i) cl[clOrder[i]]=(int)br.bits(3);
                Huff clh; clh.build(cl,19);
                int lengths[288+32]={0}; int n=0, total=hlit+hdist;
                while(n<total){
                    int sym=clh.decode(br); if(sym<0) return false;
                    if(sym<16) lengths[n++]=sym;
                    else if(sym==16){ if(n==0) return false; int r=(int)br.bits(2)+3; while(r-- && n<total){ lengths[n]=lengths[n-1]; ++n; } }
                    else if(sym==17){ int r=(int)br.bits(3)+3;  while(r-- && n<total) lengths[n++]=0; }
                    else            { int r=(int)br.bits(7)+11; while(r-- && n<total) lengths[n++]=0; }
                }
                lit.build(lengths,hlit); dist.build(lengths+hlit,hdist);
            }
            for(;;){
                int sym=lit.decode(br); if(sym<0) return false;
                if(sym==256) break;
                if(sym<256) out.push_back((uint8_t)sym);
                else {
                    sym-=257; if(sym>=29) return false;
                    int length=lenBase[sym]+(int)br.bits(lenExtra[sym]);
                    int dsym=dist.decode(br); if(dsym<0||dsym>=30) return false;
                    int d=distBase[dsym]+(int)br.bits(distExtra[dsym]);
                    if((size_t)d>out.size()) return false;
                    size_t from=out.size()-d;
                    for(int i=0;i<length;++i) out.push_back(out[from+i]);
                }
            }
        } else return false;                          // reserved
        if(br.bad) return false;
        if(final) break;
    }
    return true;
}

// strip the 2-byte zlib header (+ optional preset dict) then inflate
inline bool zlibInflate(const std::vector<uint8_t>& z, std::vector<uint8_t>& out){
    if(z.size()<2) return false;
    size_t off=2;
    if(z[1] & 0x20) off+=4;                           // FDICT preset dictionary
    if(off>z.size()) return false;
    return inflate(z.data()+off, z.size()-off, out);
}

// ----------------------------- PNG -----------------------------------------
inline int paeth(int a,int b,int c){ int p=a+b-c, pa=abs(p-a), pb=abs(p-b), pc=abs(p-c); return (pa<=pb&&pa<=pc)?a:(pb<=pc?b:c); }

inline uint32_t rd32(const uint8_t* p){ return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }

inline Image decode(const uint8_t* data, size_t len){
    Image im;
    static const uint8_t sig[8]={0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    if(len<8 || std::memcmp(data,sig,8)!=0){ im.error="not a PNG"; return im; }
    size_t i=8;
    int W=0,H=0,bitDepth=0,colorType=0,interlace=0;
    std::vector<uint8_t> palette, trns, idat;
    while(i+8<=len){
        uint32_t clen=rd32(data+i); const char* type=(const char*)(data+i+4); i+=8;
        if(i+clen+4>len) break;
        if(!std::memcmp(type,"IHDR",4)){
            W=(int)rd32(data+i); H=(int)rd32(data+i+4);
            bitDepth=data[i+8]; colorType=data[i+9]; interlace=data[i+12];
        } else if(!std::memcmp(type,"PLTE",4)){ palette.assign(data+i,data+i+clen); }
        else if(!std::memcmp(type,"tRNS",4)){ trns.assign(data+i,data+i+clen); }
        else if(!std::memcmp(type,"IDAT",4)){ idat.insert(idat.end(),data+i,data+i+clen); }
        else if(!std::memcmp(type,"IEND",4)){ break; }
        i+=clen+4;                                    // skip data + CRC
    }
    if(W<=0||H<=0){ im.error="bad IHDR"; return im; }
    if(bitDepth!=8){ im.error="only 8-bit depth supported"; return im; }
    if(interlace!=0){ im.error="interlaced PNG not supported"; return im; }

    int channels;
    switch(colorType){ case 0: channels=1; break; case 2: channels=3; break;
                       case 3: channels=1; break; case 4: channels=2; break; case 6: channels=4; break;
                       default: im.error="unsupported colour type"; return im; }

    std::vector<uint8_t> raw;
    if(!zlibInflate(idat, raw)){ im.error="inflate failed"; return im; }
    size_t stride=(size_t)W*channels;
    if(raw.size() < (stride+1)*(size_t)H){ im.error="short pixel data"; return im; }

    // ---- unfilter scanlines in place into a tight buffer ----
    std::vector<uint8_t> img((size_t)H*stride);
    int bpp = channels;                               // bytes per pixel (8-bit)
    for(int y=0;y<H;++y){
        const uint8_t* src=&raw[(size_t)y*(stride+1)];
        int filter=src[0]; const uint8_t* in=src+1;
        uint8_t* row=&img[(size_t)y*stride];
        uint8_t* prev=(y>0)?&img[(size_t)(y-1)*stride]:nullptr;
        for(size_t x=0;x<stride;++x){
            int a=(x>= (size_t)bpp)?row[x-bpp]:0;
            int b=prev?prev[x]:0;
            int c=(prev && x>=(size_t)bpp)?prev[x-bpp]:0;
            int v=in[x];
            switch(filter){
                case 0: break;
                case 1: v+=a; break;
                case 2: v+=b; break;
                case 3: v+=(a+b)/2; break;
                case 4: v+=paeth(a,b,c); break;
                default: im.error="bad filter"; return im;
            }
            row[x]=(uint8_t)v;
        }
    }

    // ---- expand to RGBA8 ----
    im.w=W; im.h=H; im.rgba.assign((size_t)W*H*4,255);
    for(int p=0;p<W*H;++p){
        const uint8_t* s=&img[(size_t)p*channels];
        uint8_t* o=&im.rgba[(size_t)p*4];
        if(colorType==0){ o[0]=o[1]=o[2]=s[0]; o[3]=255; }
        else if(colorType==4){ o[0]=o[1]=o[2]=s[0]; o[3]=s[1]; }
        else if(colorType==2){ o[0]=s[0]; o[1]=s[1]; o[2]=s[2]; o[3]=255; }
        else if(colorType==6){ o[0]=s[0]; o[1]=s[1]; o[2]=s[2]; o[3]=s[3]; }
        else { // palette
            int idx=s[0]; size_t pi=(size_t)idx*3;
            if(pi+2<palette.size()){ o[0]=palette[pi]; o[1]=palette[pi+1]; o[2]=palette[pi+2]; }
            o[3]=((size_t)idx<trns.size())?trns[idx]:255;
        }
    }
    im.ok=true;
    return im;
}

} // namespace png
