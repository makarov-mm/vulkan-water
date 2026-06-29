// gltf_min.h — tiny self-contained glTF 2.0 / GLB loader. Reads POSITION, optional
// NORMAL and indices from the first mesh primitive. Supports .glb (binary), .gltf
// with an external .bin, and base64 data URIs. No external dependencies.
#pragma once
#include <string>
#include <vector>
#include <utility>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <cstdlib>

namespace gltfmin {

struct Result {
    std::vector<float>    positions;   // xyz * N
    std::vector<float>    normals;     // xyz * N (empty if absent)
    std::vector<uint32_t> indices;     // empty if non-indexed
    bool ok=false;
    std::string error;
};

// ---------- minimal JSON ----------
struct JVal {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t=NUL;
    bool b=false; double num=0; std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string,JVal>> obj;
    const JVal* find(const std::string& k) const {
        for(auto& p:obj) if(p.first==k) return &p.second; return nullptr;
    }
    int    asInt()    const { return (int)num; }
    double asNum()    const { return num; }
};

struct JParser {
    const std::string& s; size_t i=0; bool err=false;
    JParser(const std::string& src): s(src) {}
    void ws(){ while(i<s.size()){ char c=s[i]; if(c==' '||c=='\t'||c=='\n'||c=='\r') ++i; else break; } }
    JVal value(){
        ws(); if(i>=s.size()){ err=true; return {}; }
        char c=s[i];
        if(c=='{') return object();
        if(c=='[') return array();
        if(c=='"'){ JVal v; v.t=JVal::STR; v.str=str(); return v; }
        if(c=='t'){ i+=4; JVal v; v.t=JVal::BOOL; v.b=true;  return v; }
        if(c=='f'){ i+=5; JVal v; v.t=JVal::BOOL; v.b=false; return v; }
        if(c=='n'){ i+=4; JVal v; v.t=JVal::NUL; return v; }
        return number();
    }
    std::string str(){
        std::string out; ++i; // opening quote
        while(i<s.size()){
            char c=s[i++];
            if(c=='"') break;
            if(c=='\\' && i<s.size()){
                char e=s[i++];
                switch(e){
                    case 'n': out+='\n'; break; case 't': out+='\t'; break;
                    case 'r': out+='\r'; break; case 'b': out+='\b'; break;
                    case 'f': out+='\f'; break; case '/': out+='/';  break;
                    case '\\':out+='\\'; break; case '"': out+='"';  break;
                    case 'u': { if(i+4<=s.size()){ int v=(int)strtol(s.substr(i,4).c_str(),nullptr,16); i+=4; out+=(v<128)?(char)v:'?'; } } break;
                    default:  out+=e; break;
                }
            } else out+=c;
        }
        return out;
    }
    JVal number(){
        size_t start=i;
        while(i<s.size()){ char c=s[i]; if((c>='0'&&c<='9')||c=='-'||c=='+'||c=='.'||c=='e'||c=='E') ++i; else break; }
        JVal v; v.t=JVal::NUM; v.num=strtod(s.substr(start,i-start).c_str(),nullptr); return v;
    }
    JVal object(){
        JVal v; v.t=JVal::OBJ; ++i; ws();
        if(i<s.size()&&s[i]=='}'){ ++i; return v; }
        while(i<s.size()){
            ws(); std::string key=str(); ws();
            if(i<s.size()&&s[i]==':') ++i;
            v.obj.emplace_back(key, value()); ws();
            if(i<s.size()&&s[i]==','){ ++i; continue; }
            if(i<s.size()&&s[i]=='}'){ ++i; break; }
            err=true; break;
        }
        return v;
    }
    JVal array(){
        JVal v; v.t=JVal::ARR; ++i; ws();
        if(i<s.size()&&s[i]==']'){ ++i; return v; }
        while(i<s.size()){
            v.arr.push_back(value()); ws();
            if(i<s.size()&&s[i]==','){ ++i; continue; }
            if(i<s.size()&&s[i]==']'){ ++i; break; }
            err=true; break;
        }
        return v;
    }
};

inline int getInt(const JVal& o, const char* key, int def){
    const JVal* v=o.find(key); return (v&&v->t==JVal::NUM)? v->asInt() : def;
}

inline std::vector<uint8_t> readFile(const std::string& path){
    std::ifstream f(path, std::ios::binary|std::ios::ate);
    if(!f) return {};
    std::streamsize n=f.tellg(); f.seekg(0);
    std::vector<uint8_t> d((size_t)n);
    if(n>0) f.read((char*)d.data(), n);
    return d;
}

inline std::vector<uint8_t> base64Decode(const std::string& in){
    auto dec=[](char c)->int{
        if(c>='A'&&c<='Z') return c-'A';
        if(c>='a'&&c<='z') return c-'a'+26;
        if(c>='0'&&c<='9') return c-'0'+52;
        if(c=='+') return 62; if(c=='/') return 63; return -1;
    };
    std::vector<uint8_t> out; int val=0, bits=0;
    for(char c:in){ int d=dec(c); if(d<0) continue; val=(val<<6)|d; bits+=6;
        if(bits>=8){ bits-=8; out.push_back((uint8_t)((val>>bits)&0xFF)); } }
    return out;
}

// ---------- loader ----------
inline Result load(const std::string& path){
    Result R;
    std::vector<uint8_t> file = readFile(path);
    if(file.empty()){ R.error="cannot read file"; return R; }

    std::string json;
    std::vector<uint8_t> glbBin;
    if(file.size()>=12 && file[0]=='g'&&file[1]=='l'&&file[2]=='T'&&file[3]=='F'){
        size_t off=12;
        while(off+8<=file.size()){
            uint32_t len; uint32_t type;
            std::memcpy(&len,&file[off],4); std::memcpy(&type,&file[off+4],4); off+=8;
            if(off+len>file.size()) break;
            if(type==0x4E4F534A) json.assign((const char*)&file[off], len);          // "JSON"
            else if(type==0x004E4942) glbBin.assign(file.begin()+off, file.begin()+off+len); // "BIN\0"
            off+=len;
        }
    } else {
        json.assign((const char*)file.data(), file.size());
    }
    if(json.empty()){ R.error="no JSON chunk"; return R; }

    JParser jp(json); JVal root=jp.value();
    if(root.t!=JVal::OBJ){ R.error="bad JSON root"; return R; }

    const JVal* buffers     = root.find("buffers");
    const JVal* bufferViews = root.find("bufferViews");
    const JVal* accessors   = root.find("accessors");
    const JVal* meshes      = root.find("meshes");
    if(!bufferViews||!accessors||!meshes||meshes->arr.empty()){ R.error="missing required arrays"; return R; }

    // resolve buffers
    std::string dir; { size_t p=path.find_last_of("/\\"); dir = (p==std::string::npos)?"":path.substr(0,p+1); }
    std::vector<std::vector<uint8_t>> bufData;
    if(buffers) for(const JVal& b : buffers->arr){
        const JVal* uri=b.find("uri");
        if(!uri){ bufData.push_back(glbBin); continue; }                     // GLB BIN chunk
        const std::string& u=uri->str;
        if(u.rfind("data:",0)==0){ size_t c=u.find("base64,"); bufData.push_back(c!=std::string::npos? base64Decode(u.substr(c+7)) : std::vector<uint8_t>{}); }
        else bufData.push_back(readFile(dir+u));
    }
    if(bufData.empty()) bufData.push_back(glbBin);

    // first primitive
    const JVal* prims = meshes->arr[0].find("primitives");
    if(!prims||prims->arr.empty()){ R.error="no primitives"; return R; }
    const JVal& prim = prims->arr[0];
    const JVal* attribs = prim.find("attributes");
    if(!attribs){ R.error="no attributes"; return R; }
    const JVal* posA = attribs->find("POSITION");
    if(!posA){ R.error="no POSITION"; return R; }
    const JVal* nrmA = attribs->find("NORMAL");
    const JVal* idxA = prim.find("indices");

    auto compSize=[](int ct)->int{ switch(ct){ case 5120:case 5121:return 1; case 5122:case 5123:return 2; case 5125:case 5126:return 4; default:return 4; } };

    auto readVec3=[&](int accIdx, std::vector<float>& out)->bool{
        const JVal& acc=accessors->arr[accIdx];
        int bvIdx=getInt(acc,"bufferView",-1); if(bvIdx<0) return false;
        int count=getInt(acc,"count",0);
        int accOff=getInt(acc,"byteOffset",0);
        const JVal& bv=bufferViews->arr[bvIdx];
        int bufIdx=getInt(bv,"buffer",0);
        int bvOff=getInt(bv,"byteOffset",0);
        int stride=getInt(bv,"byteStride",0); if(stride==0) stride=3*sizeof(float);
        if(bufIdx>=(int)bufData.size()) return false;
        const std::vector<uint8_t>& data=bufData[bufIdx];
        out.resize((size_t)count*3);
        for(int i=0;i<count;++i){
            size_t base=(size_t)bvOff+accOff+(size_t)i*stride;
            if(base+3*sizeof(float)>data.size()) return false;
            std::memcpy(&out[(size_t)i*3], &data[base], 3*sizeof(float));
        }
        return true;
    };

    if(!readVec3(posA->asInt(), R.positions)){ R.error="failed reading POSITION"; return R; }
    if(nrmA) readVec3(nrmA->asInt(), R.normals);

    if(idxA){
        const JVal& acc=accessors->arr[idxA->asInt()];
        int bvIdx=getInt(acc,"bufferView",-1);
        int count=getInt(acc,"count",0);
        int ct=getInt(acc,"componentType",5125);
        int accOff=getInt(acc,"byteOffset",0);
        if(bvIdx>=0){
            const JVal& bv=bufferViews->arr[bvIdx];
            int bufIdx=getInt(bv,"buffer",0);
            int bvOff=getInt(bv,"byteOffset",0);
            int cs=compSize(ct);
            int stride=getInt(bv,"byteStride",0); if(stride==0) stride=cs;
            const std::vector<uint8_t>& data=bufData[bufIdx];
            R.indices.resize(count);
            for(int i=0;i<count;++i){
                size_t base=(size_t)bvOff+accOff+(size_t)i*stride;
                if(base+cs>data.size()){ R.indices.clear(); break; }
                uint32_t v=0;
                if(ct==5121)      v=data[base];
                else if(ct==5123){ uint16_t s; std::memcpy(&s,&data[base],2); v=s; }
                else             { std::memcpy(&v,&data[base],4); }
                R.indices[i]=v;
            }
        }
    }
    R.ok=true;
    return R;
}

} // namespace gltfmin
