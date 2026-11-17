#include "list_service.h"
#include "apps/reminders/reminder_core.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <memory>
#include <new>
#include <algorithm>

namespace lists {
namespace {
constexpr uint32_t kMagic=0x3154534c;
StaticSemaphore_t mutex_storage;SemaphoreHandle_t mutex;
std::atomic<uint32_t> rev{0};
using Buffer=std::unique_ptr<List>;
struct Guard {Guard(){xSemaphoreTake(mutex,portMAX_DELAY);}~Guard(){xSemaphoreGive(mutex);}};
struct Storage {nvs_handle_t h{};bool ok;Storage():ok(nvs_open("text_lists_v1",NVS_READWRITE,&h)==ESP_OK){}~Storage(){if(ok)nvs_close(h);}};
bool fail(const char **error,const char *text){*error=text;return false;}
bool text_ok(const char *s,size_t limit){return s&&std::strlen(s)<limit&&reminders::valid_title(s);}
bool valid(const List &r){
    if(r.magic!=kMagic||!r.id||!r.revision||!r.next_item||r.count>kItems||
       !std::memchr(r.title,0,kTitle)||!text_ok(r.title,kTitle))return false;
    for(unsigned i=0;i<r.count;++i){const auto &v=r.items[i];
        if(!v.id||v.id>=r.next_item||v.done>1||!std::memchr(v.text,0,kText)||!text_ok(v.text,kText))return false;
        for(unsigned j=0;j<i;++j)if(r.items[j].id==v.id)return false;
    }return true;
}
void key(unsigned slot,char (&name)[8]){std::snprintf(name,sizeof(name),"list%u",slot);}
bool load(Storage &s,unsigned slot,List *out,const char **error){
    *out={};char name[8];key(slot,name);size_t len=sizeof(*out);
    const auto err=nvs_get_blob(s.h,name,out,&len);
    if(err==ESP_ERR_NVS_NOT_FOUND)return true;
    if(err!=ESP_OK||len!=sizeof(*out)||!valid(*out))return fail(error,"List storage unreadable; nothing overwritten");
    return true;
}
bool save(Storage &s,unsigned slot,List &value,const char **error){
    char name[8];key(slot,name);
    auto err=value.id?nvs_set_blob(s.h,name,&value,sizeof(value)):nvs_erase_key(s.h,name);
    if(err==ESP_ERR_NVS_NOT_FOUND&&!value.id)err=ESP_OK;
    if(err==ESP_OK)err=nvs_commit(s.h);
    if(err!=ESP_OK)return fail(error,"Save failed. Read the list and retry");
    ++rev;return true;
}
bool append(List &r,const char *const *items,size_t count,const char **error){
    if(!count||count>kBatch||!items)return fail(error,"Add 1 to 3 items at a time");
    for(size_t i=0;i<count;++i){
        if(!text_ok(items[i],kText))return fail(error,"Item invalid or too long; keep it under 95 characters");
        bool exists=false;for(unsigned j=0;j<r.count;++j)if(!std::strcmp(r.items[j].text,items[i]))exists=true;
        if(exists)continue;
        if(r.count==kItems)return fail(error,"A list holds 12 items. Remove some first");
        if(r.next_item==UINT32_MAX)return fail(error,"Item IDs used up");
        auto &v=r.items[r.count++];v={};v.id=r.next_item++;std::strcpy(v.text,items[i]);
    }return true;
}
}
void initialize(){if(!mutex)mutex=xSemaphoreCreateMutexStatic(&mutex_storage);}
uint32_t revision(){return rev.load();}
bool catalog(Summary *out,size_t *count,const char **error){
    *error=nullptr;*count=0;if(!mutex)return fail(error,"Lists not ready");
    Buffer r(new(std::nothrow) List{});if(!r)return fail(error,"Low memory. Try again later");
    Guard g;Storage s;if(!s.ok)return fail(error,"List storage unavailable");
    for(unsigned i=0;i<kLists;++i){if(!load(s,i,r.get(),error))return false;if(!r->id)continue;
        auto &v=out[(*count)++];v={};v.id=r->id;v.count=r->count;std::strcpy(v.title,r->title);
        for(unsigned j=0;j<r->count;++j)v.done+=r->items[j].done;
    }return true;
}
bool read(uint32_t id,List *out,const char **error){
    *error=nullptr;if(!mutex)return fail(error,"Lists not ready");
    Guard g;Storage s;if(!s.ok)return fail(error,"List storage unavailable");
    for(unsigned i=0;i<kLists;++i){if(!load(s,i,out,error))return false;if(id&&out->id==id)return true;}
    return fail(error,"This list no longer exists");
}
bool create(const char *title,const char *const *items,size_t count,uint32_t *id,const char **error){
    *error=nullptr;if(!mutex)return fail(error,"Lists not ready");
    if(count>kBatch||(count&&!items))return fail(error,"At most 3 items per call; save in batches");
    if(!text_ok(title,kTitle))return fail(error,"Title too long; keep it under 47 characters");
    Buffer r(new(std::nothrow) List{});if(!r)return fail(error,"Low memory. Try again later");
    Guard g;Storage s;if(!s.ok)return fail(error,"List storage unavailable");
    unsigned slot=kLists;uint32_t next=1;
    const auto next_err=nvs_get_u32(s.h,"next",&next);
    if(next_err!=ESP_OK&&next_err!=ESP_ERR_NVS_NOT_FOUND)return fail(error,"Could not read list ID");
    for(unsigned i=0;i<kLists;++i){
        if(!load(s,i,r.get(),error))return false;
        if(!r->id){if(slot==kLists)slot=i;continue;}
        if(!std::strcmp(title,r->title)){
            bool same=count==r->count;
            for(size_t j=0;same&&j<count;++j)same=items&&items[j]&&!std::strcmp(items[j],r->items[j].text);
            if(same){*id=r->id;return true;}
            return fail(error,"A list with this name exists. Read it and add to it; it cannot be overwritten");
        }
        if(r->id>=next)next=r->id==UINT32_MAX?0:r->id+1;
    }
    if(slot==kLists)return fail(error,"Up to 5 lists. Delete one first");
    if(!next||next==UINT32_MAX)return fail(error,"List IDs used up");
    *r={};r->magic=kMagic;r->id=next;r->revision=1;r->next_item=1;std::strcpy(r->title,title);
    if(count&&!append(*r,items,count,error))return false;
    if(nvs_set_u32(s.h,"next",next+1)!=ESP_OK||nvs_commit(s.h)!=ESP_OK)return fail(error,"Could not save list ID");
    if(!save(s,slot,*r,error))return false;
    *id=next;return true;
}
bool modify(uint32_t id,uint32_t expected,Action action,uint32_t item_id,const char *text,int done,
            const char *const *items,size_t count,const char **error){
    *error=nullptr;if(!mutex)return fail(error,"Lists not ready");
    Buffer r(new(std::nothrow) List{});if(!r)return fail(error,"Low memory. Try again later");
    Guard g;Storage s;if(!s.ok)return fail(error,"List storage unavailable");
    unsigned slot=0;for(;slot<kLists;++slot){if(!load(s,slot,r.get(),error))return false;if(id&&r->id==id)break;}
    if(slot==kLists)return fail(error,"This list no longer exists");
    if(!expected||r->revision!=expected)return fail(error,"List changed. Read it again first");
    if(action==Action::Delete){*r={};return save(s,slot,*r,error);}
    if(action==Action::Add){if(!append(*r,items,count,error))return false;}
    else if(action==Action::Rename){
        if(!text_ok(text,kTitle))return fail(error,"Title invalid or too long");
        // Keep names unique without a second retained list buffer.
        const auto new_title=std::string(text);
        Buffer other(new(std::nothrow) List{});if(!other)return fail(error,"Low memory");
        for(unsigned i=0;i<kLists;++i)if(i!=slot){if(!load(s,i,other.get(),error))return false;
            if(other->id&&!std::strcmp(other->title,new_title.c_str()))return fail(error,"A list with this name exists");}
        std::strcpy(r->title,new_title.c_str());
    }else{
        unsigned index=0;while(index<r->count&&r->items[index].id!=item_id)++index;
        if(index==r->count)return fail(error,"Item no longer exists. Read the list again");
        if(action==Action::Remove){
            for(unsigned i=index;i+1<r->count;++i)r->items[i]=r->items[i+1];
            r->items[--r->count]={};
        }else if(action==Action::Set){
            if(!text&&done<0)return fail(error,"Give text or done state");
            if(text){if(!text_ok(text,kText))return fail(error,"Item invalid or too long");std::strcpy(r->items[index].text,text);}
            if(done>=0){if(done>1)return fail(error,"Invalid done state");r->items[index].done=done;}
        }else return fail(error,"Unsupported list action");
    }
    if(r->revision==UINT32_MAX)return fail(error,"List revisions used up");
    ++r->revision;return save(s,slot,*r,error);
}
}
