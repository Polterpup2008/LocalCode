#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdarg>
#include "llama-mmap.h"
#include "llama-impl.h"
static unsigned warning_count=0;
void llama_log_internal(ggml_log_level level,const char* message,...){va_list a;va_start(a,message);vfprintf(stderr,message,a);va_end(a);if(level==GGML_LOG_LEVEL_WARN||level==GGML_LOG_LEVEL_ERROR)++warning_count;}
std::string format(const char* message,...){va_list a;va_start(a,message);va_list b;va_copy(b,a);int n=vsnprintf(nullptr,0,message,b);va_end(b);std::vector<char> text(size_t(n)+1);vsnprintf(text.data(),text.size(),message,a);va_end(a);return std::string(text.data(),size_t(n));}

static void require(bool ok,const char* what) { if(!ok)throw std::runtime_error(std::string(what)+" error="+std::to_string(GetLastError())); }
static uint64_t hash(const unsigned char* p,size_t n) {
    uint64_t h=1469598103934665603ULL;
    for(size_t i=0;i<n;++i)h=(h^p[i])*1099511628211ULL;
    return h;
}
struct View {
    llama_file file;llama_mmap mapped;unsigned char* data;size_t size;
    View(const char* name,size_t bytes,bool preserve=false):file(name,"rb"),
#if defined(LOCALCODE_OLD_CONSTRUCTOR)
        mapped(&file,0,false,{}),
#else
        mapped(&file,0,false,{},preserve),
#endif
        data(static_cast<unsigned char*>(mapped.addr())),size(mapped.size()) {require(size==bytes,"actual class file size");(void)preserve;}
    View(const View&)=delete;View& operator=(const View&)=delete;
};
struct Pages { size_t valid=0,locked=0,total=0; };
static Pages pages(unsigned char* data,size_t beg,size_t end,size_t page) {
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> info;
    for(size_t at=beg;at<end;at+=page){PSAPI_WORKING_SET_EX_INFORMATION row{};row.VirtualAddress=data+at;info.push_back(row);}
    require(!info.empty() && info.size()*sizeof(info[0])<=MAXDWORD,"query bounds");
    require(QueryWorkingSetEx(GetCurrentProcess(),info.data(),DWORD(info.size()*sizeof(info[0])))!=FALSE,"query working set");
    Pages result;result.total=info.size();
    for(auto& row:info) {if(row.VirtualAttributes.Valid){++result.valid;if(row.VirtualAttributes.Locked)++result.locked;}}
    return result;
}
static PROCESS_MEMORY_COUNTERS_EX memory() {
    PROCESS_MEMORY_COUNTERS_EX m{};m.cb=sizeof(m);
    require(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof(m))!=FALSE,"memory counters");return m;
}
static void print(std::ostream& out,const Pages& p){out<<"{\"valid\":"<<p.valid<<",\"locked\":"<<p.locked<<",\"total\":"<<p.total<<'}';}
struct Trim {size_t first=0,last=0;bool called=false;BOOL ok=FALSE;DWORD error=0;};
// Executes the patched mmap class; only logging and formatting are test adapters.
static Trim trim(View& view,size_t first,size_t last,size_t page) {
    view.mapped.reclaim_unused(first,last); // Actual class gets original bounds.
    Trim r;first=std::min(first,view.size);last=std::min(last,view.size);
    if(first>=last)return r;
    const size_t rem=first%page,skip=rem? page-rem:0;if(skip>=last-first)return r;
    first+=skip;last-=last%page;if(first>=last)return r;
    r.first=first;r.last=last;r.called=true;return r; // Range metadata only, not raw API return.
}

#include "llama-mmap-intervals.h"
#include <random>
#include <map>

using Ranges = std::vector<std::pair<size_t,size_t>>;
static Ranges complement(size_t size, const Ranges & keep) {
    Ranges result;
    llama_mmap_intervals::for_unused(size,keep,[&](size_t first,size_t last) {
        require(first<last && last<=size,"emitted bounds");
        require(result.empty() || result.back().second<=first,"emitted order");
        result.emplace_back(first,last);
    });
    return result;
}
static bool retained_byte(size_t at,size_t size,const Ranges & keep) {
    for(const auto & range:keep)
        if(range.first<range.second && range.first<size && range.first<=at && at<std::min(size,range.second))return true;
    return false;
}
static void test_collect() {
    std::vector<llama_mmap_intervals::host_span> empty;
    require(empty.empty() && empty.capacity()==0,"CPU host-span container starts without element storage");
    std::mt19937 generator(491773);
    for(size_t trial=0;trial<5000;++trial) {
        const size_t files=1+generator()%4,size=128;
        std::map<size_t,Ranges> lazy;
        std::vector<llama_mmap_intervals::host_span> hosts;
        for(size_t file=0;file<files;++file) {
            for(size_t j=0,n=generator()%5;j<n;++j)
                lazy[file].emplace_back(generator()%160,generator()%160);
        }
        for(size_t j=0,n=generator()%14;j<n;++j) {
            size_t first=generator()%160,last=generator()%160;
            if(j%5==0)last=SIZE_MAX;
            hosts.push_back({generator()%files,first,last});
        }
        const auto grouped=llama_mmap_intervals::collect(files,lazy,hosts);
        require(grouped.size()==files,"grouped mapping count");
        for(size_t file=0;file<files;++file) {
            const auto unused=complement(size,grouped[file]);
            for(size_t at=0;at<size;++at) {
                bool live=false;
                const auto row=lazy.find(file);
                if(row!=lazy.end())live=retained_byte(at,size,row->second);
                for(const auto & host:hosts)
                    if(host.file==file && host.first<host.last && host.first<=at && at<std::min(size,host.last))live=true;
                require(live!=retained_byte(at,size,unused),"lazy/host file union byte oracle");
            }
        }
    }
    bool invalid_lazy=false,invalid_host=false;
    try {std::map<size_t,Ranges> bad{{2,{{0,10}}}};(void)llama_mmap_intervals::collect(2,bad,empty);}
    catch(const std::out_of_range &){invalid_lazy=true;}
    try {std::map<size_t,Ranges> none;(void)llama_mmap_intervals::collect(2,none,{{SIZE_MAX,0,10}});}
    catch(const std::out_of_range &){invalid_host=true;}
    require(invalid_lazy && invalid_host,"invalid file indices rejected before hints");
}
int main(int argc,char ** argv) {
    try {
        SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
        require(argc==3,"usage: intervals data.bin result.json");
        SYSTEM_INFO info{};GetSystemInfo(&info);const size_t page=info.dwPageSize;
        require(page==4096,"fixture page geometry");
        test_collect();
        std::mt19937 random(94721);
        for(size_t trial=0;trial<20000;++trial) {
            const size_t size=random()%128;Ranges keep;
            for(size_t j=0,n=random()%12;j<n;++j) {
                size_t first=random()%160,last=random()%160;
                if(j%7==0)last=SIZE_MAX;
                if(j%11==0)first=SIZE_MAX;
                keep.emplace_back(first,last);
            }
            const auto unused=complement(size,keep);
            for(size_t at=0;at<size;++at)
                require(retained_byte(at,size,keep)!=retained_byte(at,size,unused),"exhaustive byte complement");
        }
        require(complement(SIZE_MAX,{{1,SIZE_MAX}})==Ranges{{0,1}},"max endpoint does not overflow");
        require(complement(SIZE_MAX,{{0,SIZE_MAX}}).empty(),"whole max range");
        require(complement(0,{{0,SIZE_MAX}}).empty(),"zero size");
        const size_t size=16*1024*1024+37;
        HANDLE created=CreateFileA(argv[1],GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        require(created!=INVALID_HANDLE_VALUE,"create private fixture");
        std::vector<unsigned char> block(65536);
        for(size_t i=0;i<block.size();++i)block[i]=static_cast<unsigned char>((i*37+113)^(i>>7));
        for(size_t at=0;at<size;) {
            DWORD count=static_cast<DWORD>(std::min(size-at,block.size())),written=0;
            require(WriteFile(created,block.data(),count,&written,nullptr) && count==written,"write fixture");at+=count;
        }
        require(CloseHandle(created)!=FALSE,"close fixture");
        std::vector<Ranges> cases={
            {},{{0,size}},
            {{8*1024*1024+19,8*1024*1024+2*page+20},{12*1024*1024+37,12*1024*1024+4*page-50},{4*1024*1024,4*1024*1024+65536},{6*1024*1024,8*1024*1024}},
            {{3*page,5*page},{page,3*page},{2*page,4*page},{page,page},{9*page,8*page},{2*page,3*page}},
            {{SIZE_MAX,SIZE_MAX},{page+1,page+2},{SIZE_MAX-1,SIZE_MAX}},
            {{size-21,SIZE_MAX}},
            {{page+1,2*page-1},{2*page-1,3*page+1}},
            {{0,0},{size+page,size+2*page},{2*page,page}},
            {{19,SIZE_MAX}}
        };
        std::ofstream out(argv[2]);out<<"{\"page_bytes\":"<<page<<",\"file_bytes\":"<<size<<",\"metadata_fuzz_cases\":20000,\"grouped_metadata_cases\":5000,\"trials\":[";
        uint64_t golden=0;
        for(size_t trial=0;trial<cases.size();++trial) {
            View v(argv[1],size);const uint64_t before=hash(v.data,size);
            if(trial==0)golden=before;require(before==golden,"fixture bytes before");
            auto gaps=complement(size,cases[trial]);
            for(const auto & gap:gaps)v.mapped.reclaim_unused(gap.first,gap.second);
            const size_t count=(size+page-1)/page;
            std::vector<PSAPI_WORKING_SET_EX_INFORMATION> ws(count);
            for(size_t j=0;j<count;++j)ws[j].VirtualAddress=v.data+j*page;
            require(QueryWorkingSetEx(GetCurrentProcess(),ws.data(),static_cast<DWORD>(ws.size()*sizeof(ws[0])))!=FALSE,"query actual pages");
            size_t live=0,live_valid=0,unused=0,unused_valid=0;
            for(size_t j=0;j<count;++j) {
                const size_t first=j*page,last=std::min(size,first+page);bool protected_page=last-first<page;
                for(const auto & range:cases[trial])
                    if(range.first<range.second && range.first<size && range.first<last && first<std::min(size,range.second))protected_page=true;
                if(protected_page){++live;live_valid+=ws[j].VirtualAttributes.Valid;}
                else {++unused;unused_valid+=ws[j].VirtualAttributes.Valid;}
            }
            require(!live || live_valid*100>=live*95,"live tensor/lazy/backend/boundary pages retained");
            require(!unused || unused_valid*100<=unused*5,"complete unused pages removed");
            MEMORY_BASIC_INFORMATION region{};require(VirtualQuery(v.data+page,&region,sizeof(region))==sizeof(region),"mapping metadata");
            require(region.AllocationBase==v.data && region.Type==MEM_MAPPED && region.State==MEM_COMMIT && region.Protect==PAGE_READONLY,"mapping still valid");
            require(hash(v.data,size)==golden,"all mapped bytes unchanged and refaultable");
            if(trial)out<<',';out<<"{\"case\":"<<trial<<",\"protected_pages\":"<<live<<",\"protected_valid\":"<<live_valid<<",\"unused_pages\":"<<unused<<",\"unused_valid\":"<<unused_valid<<",\"all_bytes_exact\":true}";
        }
        {
            View v(argv[1],size,true);require(hash(v.data,size)==golden,"lock fixture");
            llama_mlock lock;lock.init(v.data);lock.grow_to(5*page);
            require(pages(v.data,0,5*page,page).locked==5,"actual pages locked");
            llama_mmap_intervals::for_unused(size,{},[&](size_t first,size_t last){v.mapped.reclaim_unused(first,last);});
            const auto after=pages(v.data,0,5*page,page);
            require(after.valid==5 && after.locked==5,"explicit lock retained");
            require(hash(v.data,size)==golden,"locked bytes exact");
        }
        require(warning_count==0,"no unexpected mapped-page warning");
        out<<"],\"actual_locked_pages_preserved\":5,\"normal_warnings\":0,\"status\":\"passed\"}\n";
        require(bool(out),"write result");std::cout<<"20,000 interval cases,9 actual mapped views and lock preservation passed.\n";return 0;
    }catch(const std::exception & error){std::cerr<<error.what()<<'\n';return 1;}
}
