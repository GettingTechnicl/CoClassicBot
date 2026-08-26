// =====================================================================
// explorer.cpp — standalone READ-ONLY game-state explorer for v1074.
//
// Like selftest.cpp but comprehensive: dumps the hero, enumerates nearby
// entities from the role-manager region, and dumps equipped items + the stat
// table raw so we can finish decoding CItem / CStatTable. Passive reads only,
// every read SEH-guarded (cannot crash the game). Writes
// C:\Users\Public\coclassic_explorer.json every few seconds.
// =====================================================================
#include <windows.h>
#include <cstdint>
#include <cstdio>

static const char* kPath = "C:\\Users\\Public\\coclassic_explorer.json";

template <class T> static bool TryRead(uintptr_t a, T* o)
{ __try { *o = *reinterpret_cast<volatile T*>(a); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static uint64_t U64(uintptr_t a){ uint64_t v=0; TryRead(a,&v); return v; }
static uint32_t U32(uintptr_t a){ uint32_t v=0; TryRead(a,&v); return v; }
static int32_t  I32(uintptr_t a){ int32_t  v=0; TryRead(a,&v); return v; }
static void Name(uintptr_t a, char* o, size_t n){
    o[0]=0;
    for(size_t i=0;i+1<n;i++){ uint8_t c=0; if(!TryRead(a+i,&c)||c<0x20||c>0x7e){o[i]=0;return;} o[i]=(char)c; o[i+1]=0; }
}
static bool IsHeap(uint64_t p){ return p>0x10000 && p<0x7FFFFFFFFFFFull; }

// Dump a memory region to a file (qword reads, SEH-guarded). For offline diffing:
// take two snapshots (before/after a change) and diff to locate ANY changed field.
static void DumpRegion(uintptr_t addr, size_t len, const char* path){
    FILE* f=nullptr; if(fopen_s(&f,path,"wb")!=0 || !f) return;
    const size_t CH=0x2000; static unsigned char buf[0x2000];
    for(size_t off=0; off<len; off+=CH){
        size_t n=(len-off<CH)?(len-off):CH;
        for(size_t i=0;i+8<=n;i+=8){ uint64_t v=0; TryRead(addr+off+i,&v); *(uint64_t*)(buf+i)=v; }
        fwrite(buf,1,n,f);
    }
    fclose(f);
}

static const uint64_t IMG_LO=0x140000000ull, IMG_HI=0x140000000ull+0x28C5000ull;

// Signature check: does object at 'p' look like a CRole?
//   vtable(+0x00) points into the image, id(+0x68) is a valid entity id,
//   and map position(+0xD8) is in-range.
static bool LooksLikeRole(uintptr_t p, uint32_t* id, int32_t* x, int32_t* y){
    if(!IsHeap(p)) return false;
    uint64_t vt=U64(p); if(vt<IMG_LO || vt>=IMG_HI) return false;
    uint32_t i = U32(p+0x68); int32_t px=I32(p+0xD8), py=I32(p+0xDC);
    if(i==0 || i>=5000000) return false;
    if(px<=0||px>1000||py<=0||py>1000) return false;
    *id=i; *x=px; *y=py; return true;
}

static DWORD WINAPI Run(LPVOID){
    const uint64_t base=(uint64_t)GetModuleHandleA(nullptr);
    uintptr_t hero=0, roleMgr=0;
    for(int i=0;i<1200;i++){
        roleMgr=(uintptr_t)U64(base+0x69C730);
        if(roleMgr){ hero=(uintptr_t)U64(roleMgr); if(hero && U32(hero+0x68)>=1000000) break; }
        Sleep(500);
    }
    if(!hero) return 0;

    for(;;){
        roleMgr=(uintptr_t)U64(base+0x69C730);
        hero = roleMgr ? (uintptr_t)U64(roleMgr) : 0;
        if(!hero){ Sleep(3000); continue; }

        FILE* f=nullptr;
        if(fopen_s(&f,kPath,"w")!=0 || !f){ Sleep(3000); continue; }

        char hname[16]; Name(hero+0x94,hname,sizeof(hname));
        fprintf(f,"{\n");
        fprintf(f,"  \"base\":\"0x%llX\", \"roleMgr\":\"0x%llX\", \"hero\":\"0x%llX\",\n",
                (unsigned long long)base,(unsigned long long)roleMgr,(unsigned long long)hero);
        fprintf(f,"  \"hero_stats\": { \"name\":\"%s\", \"id\":%u, \"maxHP\":%d, \"level\":%u, "
                  "\"silver\":%u, \"stamina\":%d, \"pos\":[%d,%d] },\n",
                hname, U32(hero+0x68), I32(hero+0x3D0), U32(hero+0x6E8),
                U32(hero+0xA80), I32(hero+0x6E0), I32(hero+0xD8), I32(hero+0xDC));

        // ---- entities: heap-scan for CRole-signature objects ----
        fprintf(f,"  \"entities\": [\n");
        uint32_t seen[256]; int nseen=0; int nmon=0,nplr=0,nnpc=0,noth=0; bool first=true;
        {
            MEMORY_BASIC_INFORMATION mbi;
            uintptr_t addr=0x10000; uint64_t scanned=0; const uint64_t SCAN_CAP=40000000ull;
            while(addr < 0x7FFFFFFF0000ull && nseen<256 && scanned<SCAN_CAP){
                if(!VirtualQuery((void*)addr,&mbi,sizeof(mbi))) break;
                uintptr_t rbase=(uintptr_t)mbi.BaseAddress; size_t rsize=mbi.RegionSize;
                bool ok = mbi.State==MEM_COMMIT && mbi.Type==MEM_PRIVATE
                          && (mbi.Protect==PAGE_READWRITE || mbi.Protect==PAGE_EXECUTE_READWRITE);
                if(ok && rsize>0x100 && rsize<0x4000000){          // skip tiny/huge regions
                    for(uintptr_t a=rbase; a+0xE0<rbase+rsize && nseen<256; a+=0x10){
                        scanned++;
                        uint32_t id; int32_t x,y;
                        if(!LooksLikeRole(a,&id,&x,&y)) continue;
                        char en[16]; Name(a+0x94,en,sizeof(en));
                        int nl=0; while(en[nl]) nl++;
                        if(nl<3) continue;              // real entities have real names; drops junk
                        bool dup=false; for(int k=0;k<nseen;k++) if(seen[k]==id){dup=true;break;}
                        if(dup) continue; seen[nseen++]=id;
                        const char* kind = (id>=1000000)?"player":(id>=400000&&id<500000)?"monster":
                                           (id<400000)?"npc":"other";
                        if(id>=1000000)nplr++; else if(id>=400000&&id<500000)nmon++;
                        else if(id<400000)nnpc++; else noth++;
                        fprintf(f,"%s    { \"id\":%u, \"kind\":\"%s\", \"name\":\"%s\", \"pos\":[%d,%d], "
                                  "\"maxHP\":%d, \"ptr\":\"0x%llX\" }",
                                first?"":",\n", id, kind, en, x, y, I32(a+0x3D0), (unsigned long long)a);
                        first=false;
                    }
                }
                addr = rbase + rsize; if(rsize==0) break;
            }
        }
        fprintf(f,"\n  ],\n");
        fprintf(f,"  \"entity_counts\": { \"monsters\":%d, \"players\":%d, \"npc\":%d, \"other\":%d, \"total\":%d },\n",
                nmon,nplr,nnpc,noth,nseen);

        // ---- current map: CGameMap objects carry a stable vtable (RVA 0x5CCB60) and
        //      DocumentId(u32) at +0x10. Authoritative = the CGameMap the hero points to. ----
        {
            const uint64_t GAMEMAP_VT = 0x140000000ull + 0x5CCB60;
            // DIAGNOSTIC: list EVERY pointer from hero/roleMgr to a CGameMap, with its docId.
            // The one whose docId changes as you change maps is the real current-map field.
            fprintf(f,"  \"map_ptrs_hero\": [");
            { bool fr=true; for(uint32_t o=0;o<0x4000;o+=8){
                uintptr_t p=(uintptr_t)U64(hero+o);
                if(IsHeap(p) && U64(p)==GAMEMAP_VT){
                    fprintf(f,"%s{\"off\":\"0x%X\",\"docId\":%u}",fr?"":",",o,U32(p+0x10)); fr=false; } } }
            fprintf(f,"],\n  \"map_ptrs_roleMgr\": [");
            { bool fr=true; for(uint32_t o=0;o<0x2000;o+=8){
                uintptr_t p=(uintptr_t)U64(roleMgr+o);
                if(IsHeap(p) && U64(p)==GAMEMAP_VT){
                    fprintf(f,"%s{\"off\":\"0x%X\",\"docId\":%u}",fr?"":",",o,U32(p+0x10)); fr=false; } } }
            fprintf(f,"],\n");
            // 2) context: all distinct DocumentIds among CGameMap instances (vtable scan)
            fprintf(f,"  \"gamemap_docids\": [");
            MEMORY_BASIC_INFORMATION mbi; uintptr_t addr=0x10000; bool fr=true;
            uint64_t scanned=0; const uint64_t CAP=60000000ull; uint32_t dseen[32]; int nd=0;
            while(addr<0x7FFFFFFF0000ull && nd<32 && scanned<CAP){
                if(!VirtualQuery((void*)addr,&mbi,sizeof(mbi))) break;
                uintptr_t rb=(uintptr_t)mbi.BaseAddress; size_t rs=mbi.RegionSize;
                bool ok=mbi.State==MEM_COMMIT && mbi.Type==MEM_PRIVATE
                        && (mbi.Protect==PAGE_READWRITE||mbi.Protect==PAGE_EXECUTE_READWRITE);
                if(ok && rs>0x40 && rs<0x4000000){
                    for(uintptr_t a=rb; a+0x20<rb+rs && nd<32; a+=0x10){
                        scanned++;
                        if(U64(a)!=GAMEMAP_VT) continue;
                        uint32_t did=U32(a+0x10);
                        bool dup=false; for(int k=0;k<nd;k++) if(dseen[k]==did){dup=true;break;}
                        if(!dup){ dseen[nd++]=did; fprintf(f,"%s%u",fr?"":",",did); fr=false; }
                    }
                }
                addr=rb+rs; if(rs==0) break;
            }
            fprintf(f,"],\n");
        }

        // ---- map-pointer deref: *(base+0x699370) is the current-map/scene pointer
        //      (found via differential). Dump the pointed-to object head to locate the
        //      doc-ID offset. 2nd candidate 0x6993B8. ----
        {
            fprintf(f,"  \"map_deref\": [");
            unsigned cands[]={0x699370,0x6993B8};
            for(int c=0;c<2;c++){
                uintptr_t obj=(uintptr_t)U64(base+cands[c]);
                uint64_t vt = obj? U64(obj):0;
                fprintf(f,"%s{\"ptr_rva\":\"0x%X\",\"obj\":\"0x%llX\",\"vtable_rva\":\"0x%llX\",\"u32\":[",
                        c?",":"", cands[c], (unsigned long long)obj,
                        (unsigned long long)((vt>=0x140000000ull && vt<0x143000000ull)? vt-0x140000000ull : vt));
                for(int j=0;j<40;j++) fprintf(f,"%u%s", obj?U32(obj+(uint64_t)j*4):0, j<39?",":"");
                fprintf(f,"]}");
            }
            fprintf(f,"],\n");
        }

        // ---- reusable value-search: read a u32 from C:\Users\Public\explorer_cfg.txt
        //      (decimal) and report every memory address holding it. Diff across two
        //      game states (e.g. two maps) to find the field that changed. ----
        {
            unsigned target=0;
            FILE* cf=nullptr;
            if(fopen_s(&cf,"C:\\Users\\Public\\explorer_cfg.txt","r")==0 && cf){ if(fscanf_s(cf,"%u",&target)!=1) target=0; fclose(cf); }
            fprintf(f,"  \"value_search\": { \"value\":%u, \"addrs\":[", target);
            if(target){
                MEMORY_BASIC_INFORMATION mbi; uintptr_t addr=0x10000; int nh=0; bool fr=true;
                uint64_t scanned=0; const uint64_t CAP=200000000ull;
                while(addr<0x7FFFFFFF0000ull && nh<3000 && scanned<CAP){
                    if(!VirtualQuery((void*)addr,&mbi,sizeof(mbi))) break;
                    uintptr_t rb=(uintptr_t)mbi.BaseAddress; size_t rs=mbi.RegionSize;
                    bool ok=mbi.State==MEM_COMMIT && (mbi.Protect==PAGE_READWRITE||mbi.Protect==PAGE_READONLY
                            ||mbi.Protect==PAGE_EXECUTE_READWRITE) && !(mbi.Protect&PAGE_GUARD);
                    if(ok && rs>0x40 && rs<0x8000000){
                        for(uintptr_t a=rb; a+4<rb+rs && nh<3000; a+=4){
                            scanned++;
                            if(U32(a)==target){ fprintf(f,"%s\"0x%llX\"",fr?"":",",(unsigned long long)a); fr=false; nh++; }
                        }
                    }
                    addr=rb+rs; if(rs==0) break;
                }
            }
            fprintf(f,"] },\n");
        }

        // ---- equipment: 4 CItems raw (first 0x60 as u32) ----
        fprintf(f,"  \"equipment\": [\n");
        for(int s=0;s<4;s++){
            uintptr_t it=(uintptr_t)U64(hero+0xBD8+(uint64_t)s*0x10);
            fprintf(f,"    { \"slot\":%d, \"ptr\":\"0x%llX\", \"typeId\":%u, \"u32\":[",
                    s,(unsigned long long)it, it?U32(it+0x10):0);
            for(int j=0;j<64;j++) fprintf(f,"%u%s",it?U32(it+(uint64_t)j*4):0, j<63?",":"");
            fprintf(f,"] }%s\n", s<3?",":"");
        }
        fprintf(f,"  ],\n");

        // ---- stat table: raw records ----
        uintptr_t st=(uintptr_t)U64(hero+0x968);
        fprintf(f,"  \"statTable\":\"0x%llX\", \"statTable_u32\":[",(unsigned long long)st);
        for(int j=0;j<48;j++) fprintf(f,"%u%s", st?U32(st+(uint64_t)j*4):0, j<47?",":"");
        fprintf(f,"],\n");

        // ---- map probe: deref candidate singletons (guard+8 RVAs) + dump heads.
        // One of these is CGameMap; correlate its fields with the current map to
        // pin the map-id offset. Also re-check the stale GAME_MAP=0x4E02E0.
        {
            const uint32_t cand[] = {0x6997F8,0x6A4748,0x69A528,0x6989E8,0x698700,0x698A50};
            fprintf(f,"  \"map_probe\": [\n");
            for(int c=0;c<6;c++){
                uintptr_t obj=(uintptr_t)U64(base+cand[c]);
                fprintf(f,"    { \"ptr_rva\":\"0x%X\", \"obj\":\"0x%llX\", \"u32\":[",
                        cand[c],(unsigned long long)obj);
                for(int j=0;j<12;j++) fprintf(f,"%u%s", obj?U32(obj+(uint64_t)j*4):0, j<11?",":"");
                fprintf(f,"] }%s\n", c<5?",":"");
            }
            uintptr_t oldmap=base+0x4E02E0;
            fprintf(f,"  ],\n");
            fprintf(f,"  \"stale_GAME_MAP_head_u32\":[");
            for(int j=0;j<8;j++) fprintf(f,"%u%s", U32(oldmap+(uint64_t)j*4), j<7?",":"");
            fprintf(f,"]\n}\n");
        }
        fclose(f);

        // ---- raw region dumps for offline differential analysis ----
        // (hero object, role manager, stat table, and the .data globals block).
        // Snapshot two states and diff to locate map-pointer / EXP / anything.
        DumpRegion(hero,       0x8000,  "C:\\Users\\Public\\co_hero.bin");
        DumpRegion(roleMgr,    0x2000,  "C:\\Users\\Public\\co_rolemgr.bin");
        DumpRegion((uintptr_t)U64(hero+0x968), 0x800, "C:\\Users\\Public\\co_stat.bin");
        DumpRegion(base+0x640000, 0x80000, "C:\\Users\\Public\\co_globals.bin");

        Sleep(2000);
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID){
    if(reason==DLL_PROCESS_ATTACH){ DisableThreadLibraryCalls(h); CreateThread(nullptr,0,Run,nullptr,0,nullptr); }
    return TRUE;
}
