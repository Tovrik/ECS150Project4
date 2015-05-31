#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "VirtualMachine.h"
#include "Machine.h"
#include <vector>
#include <deque>
#include <list>
#include <fcntl.h>
using namespace std;

const TVMMemoryPoolID VM_MEMORY_POOL_ID_SYSTEM = 0;

extern "C" {
///////////////////////// TCB Class Definition ///////////////////////////
class TCB
{
public:
    TCB(TVMThreadIDRef id, TVMThreadState thread_state, TVMThreadPriority thread_priority, TVMMemorySize stack_size, TVMThreadEntry entry_point, void *entry_params, TVMTick ticks_remaining) :
        id(id),
        thread_state(thread_state),
        thread_priority(thread_priority),
        stack_size(stack_size),
        entry_point(entry_point),
        entry_params(entry_params),
        ticks_remaining(ticks_remaining) {
            // need to use mem alloc for base, not new
            // TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void **pointer) {
            VMMemoryPoolAllocate(VM_MEMORY_POOL_ID_SYSTEM, stack_size, &stack_base);
            // stack_base = new uint8_t[stack_size];
            // call_back_result = -1;
        }

    TVMThreadIDRef id;
    TVMThreadState thread_state;
    TVMThreadPriority thread_priority;
    TVMMemorySize stack_size;
    void *stack_base;
    TVMThreadEntry entry_point;
    void *entry_params;
    TVMTick ticks_remaining;
    SMachineContext machine_context; // for the context to switch to/from the thread
    int call_back_result;
};



///////////////////////// Mutex Class definition ///////////////////////////
class Mutex{
    public:
        Mutex(TVMMutexIDRef mutex_id_ref, TVMThreadIDRef owner_id_ref):
            mutex_id_ref(mutex_id_ref),
            owner_id_ref(owner_id_ref) {}

        TVMMutexIDRef mutex_id_ref;
        TVMThreadIDRef owner_id_ref;
        deque<TCB*> mutex_low_priority_queue;
        deque<TCB*> mutex_normal_priority_queue;
        deque<TCB*> mutex_high_priority_queue;
};


///////////////////////// MemoryPool Class definition ///////////////////////////

typedef struct mem_chunk {
    uint8_t *base;
    int length;
} mem_chunk;


class MemoryPool {
    public:
        MemoryPool(TVMMemorySize memory_pool_size, TVMMemoryPoolID memory_pool_id, uint8_t *base):
            memory_pool_size(memory_pool_size),
            memory_pool_id(memory_pool_id),
            // memory_size_ref(memory_size_ref),
            base(base),
            free_space(memory_pool_size) {
                // `("%d\n", memory_pool_id);
                mem_chunk *free_chunk = new mem_chunk();
                free_chunk->base = base;
                free_chunk->length = memory_pool_size;
                free_list.push_back(free_chunk);
                // printf("%p\n", base);
            }

    TVMMemorySize memory_pool_size;
    TVMMemoryPoolID memory_pool_id;
    // TVMMemorySizeRef memory_size_ref;
    list<mem_chunk*> free_list;
    list<mem_chunk*> alloc_list;
    uint8_t *base;
    unsigned int free_space;
};


///////////////////////// BPB Struct definition ///////////////////////////
#pragma pack(1)
typedef struct bpb {
    unsigned char   oem_name[8];
    uint16_t        bytes_per_sector;
    uint8_t         sectors_per_cluster;
    uint16_t        reserved_sector_count;
    uint8_t         fat_count;                 //NumFATs
    uint16_t        root_entry_count;
    uint16_t        sector_count_16;           //TotSec16
    uint8_t         media;
    uint16_t        fat_size;
    uint16_t        sectors_per_track;
    uint16_t        head_count;                 //NumHeads
    uint32_t        hidden_sector_count;
    uint32_t        sector_count_32;            //TotSec32
    uint8_t         drive_number;
    uint8_t         boot_signature;
    unsigned char   volume_id[4];
    unsigned char   volume_label[11];
    unsigned char   file_system_type[8];
    // these are supposed to be calculated values
    // FirstRootSector = BPB_RsvdSecCnt + BPB_NumFATs * BPB_FATSz16;
    // RootDirectorySectors = (BPB_RootEntCnt * 32) / 512;
    // FirstDataSector = FirstRootSector + RootDirectorySectors;
    // ClusterCount = (BPB_TotSec32 - FirstDataSector) / BPB_SecPerClus;
    uint8_t         root_dir_sectors;
    uint8_t         first_root_sector;
    uint8_t         first_data_sector;
    uint16_t        cluster_count;
} bpb;

///////////////////////// Root Entry Struct definition ///////////////////////////
typedef struct{
    char DLongFileName[VM_FILE_SYSTEM_MAX_PATH];
    char DShortFileName[VM_FILE_SYSTEM_SFN_SIZE];
    unsigned int DSize;
    unsigned char DAttributes;
    SVMDateTime DCreate;
    SVMDateTime DAccess;
    SVMDateTime DModify;
    uint16_t cluster_location;
} entry;

///////////////////////// Globals ///////////////////////////
#define VM_THREAD_PRIORITY_IDLE                  ((TVMThreadPriority)0x00)

vector<TCB*> thread_vector;
vector<Mutex*> mutex_vector;
vector<MemoryPool*> mem_pool_vector;
vector<uint16_t> fat_vector;
void*       FAT_buffer;
deque<TCB*> low_priority_queue;
deque<TCB*> normal_priority_queue;
deque<TCB*> high_priority_queue;
vector<TCB*> sleep_vector;
TCB*        idle_thread;
TCB*        current_thread;
Mutex*      current_mutex;
MemoryPool* current_mem_pool;
int         file_descriptor;
bpb         *the_bpb;
TVMMutexID  sector_mutex;
vector<entry*> entry_vector;

volatile int timer;
TMachineSignalStateRef sigstate;

///////////////////////// Function Prototypes ///////////////////////////
TVMMainEntry VMLoadModule(const char *module);

///////////////////////// Utilities ///////////////////////////
void actual_removal(TCB* thread, deque<TCB*> &Q) {
    for (deque<TCB*>::iterator it=Q.begin(); it != Q.end(); ++it) {
        if (*it == thread) {
            Q.erase(it);
            break;
        }
    }
}


void determine_queue_and_push(TCB* thread) {
    if (thread->thread_priority == VM_THREAD_PRIORITY_LOW) {
        low_priority_queue.push_back(thread);
    }
    else if (thread->thread_priority == VM_THREAD_PRIORITY_NORMAL) {
        normal_priority_queue.push_back(thread);
    }
    else if (thread->thread_priority == VM_THREAD_PRIORITY_HIGH) {
        high_priority_queue.push_back(thread);
    }
}


void mutex_determine_queue_and_push(TCB* thread, Mutex* mutex) {
    if (thread->thread_priority == VM_THREAD_PRIORITY_LOW) {
        low_priority_queue.push_back(thread);
    }
    else if (thread->thread_priority == VM_THREAD_PRIORITY_NORMAL) {
        normal_priority_queue.push_back(thread);
    }
    else if (thread->thread_priority == VM_THREAD_PRIORITY_HIGH) {
        high_priority_queue.push_back(thread);
    }
}

void determine_queue_and_remove(TCB *thread) {
    if (thread->thread_priority == VM_THREAD_PRIORITY_LOW) {
        actual_removal(thread, low_priority_queue);
    }
    else if (thread->thread_priority == VM_THREAD_PRIORITY_NORMAL) {
        actual_removal(thread, normal_priority_queue);
    }
    else if (thread->thread_priority == VM_THREAD_PRIORITY_HIGH) {
        actual_removal(thread, high_priority_queue);
    }
}

void scheduler_action(deque<TCB*> &Q) {
    // set current thread to ready state if it was running
    if (current_thread->thread_state == VM_THREAD_STATE_RUNNING) {
        current_thread->thread_state = VM_THREAD_STATE_READY;
    }
    if (current_thread->thread_state == VM_THREAD_STATE_READY) {
        determine_queue_and_push(current_thread);
    }
    // need the ticks_remaining condition because threads that are waiting on file I/O are in wait state but shouldn't go to sleep_vector
    else if (current_thread->thread_state == VM_THREAD_STATE_WAITING && current_thread->ticks_remaining != 0) {
        sleep_vector.push_back(current_thread);
    }
    TCB* temp = current_thread;
    // set current to next and remove from Q
    current_thread = Q.front();
    Q.pop_front();
    // set current to running
    current_thread->thread_state = VM_THREAD_STATE_RUNNING;

    MachineContextSwitch(&(temp->machine_context), &(current_thread->machine_context));
}

void scheduler() {
    if (!high_priority_queue.empty()) {
        scheduler_action(high_priority_queue);
    }
    else if (!normal_priority_queue.empty()) {
        scheduler_action(normal_priority_queue);
    }
    else if (!low_priority_queue.empty()) {
        scheduler_action(low_priority_queue);
    }
    // schedule idle thread
    else {
        if (current_thread->thread_state == VM_THREAD_STATE_READY) {
            determine_queue_and_push(current_thread);
        }
        // need the ticks_remaining condition because threads that are waiting on file I/O are in wait state but shouldn't go to sleep_vector
        else if (current_thread->thread_state == VM_THREAD_STATE_WAITING && current_thread->ticks_remaining != 0) {
            sleep_vector.push_back(current_thread);
        }
        TCB* temp = current_thread;
        current_thread = idle_thread;
        current_thread->thread_state = VM_THREAD_STATE_RUNNING;
        thread_vector.push_back(current_thread);
        MachineContextSwitch(&(temp->machine_context), &(idle_thread->machine_context));
    }
}

void release(TVMMutexID mutex, deque<TCB*> &Q) {
    TCB* new_thread;
    if(!(Q.empty())) {
        new_thread = Q.front();;
        Q.pop_front();
        new_thread->thread_state = VM_THREAD_STATE_READY;
        high_priority_queue.push_back(new_thread);
        if(new_thread->thread_priority > current_thread->thread_priority) {
            scheduler();
        }
    }
}

void add_to_free_list(MemoryPool *actual_mem_pool, mem_chunk* free_mem_chunk) {
    // find position to insert into free_list
    list<mem_chunk*>::iterator it = actual_mem_pool->free_list.begin();
    for (; it !=actual_mem_pool->free_list.end(); ++it) {
        if((*it)->base >= free_mem_chunk->base) {
            break;
        }
    }
    actual_mem_pool->free_list.insert(it, free_mem_chunk);          // it = current. after insert it = next     _ v _   --> _ _ v _
    // merge with chunk after in memory
    if (it != actual_mem_pool->free_list.end()) {
        --it;                                                       // it = current                             _ v _ _
        if ((*it)->base + (*it)->length == (*(++it))->base) {       // it = current. after ++it, it = next      _ v _ _ --> _ _ v _
            --it;                                                   // it = current                             _ v _ _
            (*it)->length = (*it)->length + (*(++it))->length;      // it = current. after ++it, it = next      _ v _ _ --> _ _ v _
            actual_mem_pool->free_list.erase(it);                   // it = next                                _ _ v
            --it;                                                   // it = current                             _ v _
        }
    }
    // need to decrement because end points to the "past-the-end" element
    else {
        --it;
    }
    // merge with chunk before in memory
    if (it != actual_mem_pool->free_list.begin()) {
        --it;                                                       // it = prev                                v _ _
        if ((*it)->base + (*it)->length == (*(++it))->base) {       // it = prev. after ++it, it = current      v _ _   --> _ v _
            --it;                                                   // it = prev.                               v _ _
            // printf("%x %x\n", (*it)->base, (*it)->length);
            (*it)->length = (*it)->length + (*(++it))->length;      // it = prev. after ++it, it = current      v _ _   --> _ v _
            actual_mem_pool->free_list.erase(it);                   // it = next                                _ v
            // it--;
            // printf("%x %x\n", (*it)->base, (*it)->length);
        }
    }
    // printf("%d\n", actual_mem_pool->free_list.size());
}

///////////////////////// Callbacks ///////////////////////////
// for ref: typedef void (*TMachineAlarmCallback)(void *calldata);
void timerDecrement(void *calldata) {
    // decrements ticks for each sleeping thread
    for (int i = 0; i < sleep_vector.size(); i++) {
        sleep_vector[i]->ticks_remaining--;
        if (sleep_vector[i]->ticks_remaining ==  0) {
            sleep_vector[i]->thread_state = VM_THREAD_STATE_READY;
            determine_queue_and_push(sleep_vector[i]);
            sleep_vector.erase(sleep_vector.begin() + i);
            scheduler();
        }
    }
}

void SkeletonEntry(void *param) {
    MachineEnableSignals();
    TCB* temp = (TCB*)param;
    temp->entry_point(temp->entry_params);
    VMThreadTerminate(*(temp->id)); // This will allow you to gain control back if the ActualThreadEntry returns
}

void idleEntry(void *param) {
    MachineEnableSignals();
    while(1);
}

void MachineFileCallback(void* param, int result) {
    TCB* temp = (TCB*)param;
    temp->thread_state = VM_THREAD_STATE_READY;
    determine_queue_and_push(temp);
    temp->call_back_result = result;
    // printf("%d\n", result);
    if ((current_thread->thread_state == VM_THREAD_STATE_RUNNING && current_thread->thread_priority < temp->thread_priority) || current_thread->thread_state != VM_THREAD_STATE_RUNNING) {
        scheduler();
    }
}

///////////////////////// FAT Utility Functions ///////////////////////////
void read_bpb(int fd, int offset) {
    MachineSuspendSignals(sigstate);
    VMMutexAcquire(sector_mutex, VM_TIMEOUT_INFINITE);
    void *addr;
    VMMemoryPoolAllocate(1, 512, &addr);
    MachineFileSeek(fd, offset, 0, MachineFileCallback, current_thread);
    current_thread->thread_state = VM_THREAD_STATE_WAITING;
    scheduler();
    MachineFileRead(fd, addr, 512, MachineFileCallback, current_thread);
    current_thread->thread_state = VM_THREAD_STATE_WAITING;
    scheduler();
    bpb *temp = new bpb;
    memcpy(temp, addr, 512);
    the_bpb = temp;
    VMMemoryPoolDeallocate(1, addr);
    VMMutexRelease(sector_mutex);
    MachineResumeSignals(sigstate);
}

void read_FAT(int fd, int offset) {
    MachineSuspendSignals(sigstate);
    VMMutexAcquire(sector_mutex, VM_TIMEOUT_INFINITE);
    VMMemoryPoolAllocate(1, 512, &FAT_buffer);
    MachineFileSeek(fd, offset, 0, MachineFileCallback, current_thread);
    current_thread->thread_state = VM_THREAD_STATE_WAITING;
    scheduler();
    MachineFileRead(fd, FAT_buffer, 512, MachineFileCallback, current_thread);
    current_thread->thread_state = VM_THREAD_STATE_WAITING;
    scheduler();
    uint16_t *temp_buffer = (uint16_t*)FAT_buffer;
    for (int i = 0; i < 256; ++i)
    {
        if(temp_buffer[i] != 0) {
            fat_vector.push_back(temp_buffer[i]);
        }
        else {
            break;
        }
    }
    VMMemoryPoolDeallocate(1, FAT_buffer);
    VMMutexRelease(sector_mutex);
    MachineResumeSignals(sigstate);
}

void read_root(int fd, int offset) {
    printf("1\n");
    MachineSuspendSignals(sigstate);
    printf("2\n");
    VMMutexAcquire(sector_mutex, VM_TIMEOUT_INFINITE);
    printf("3\n");
    void *addr;
    printf("4\n");
    VMMemoryPoolAllocate(1, 512, &addr);
    printf("5\n");
    MachineFileSeek(fd, offset, 0, MachineFileCallback, current_thread);
    printf("6\n");
    current_thread->thread_state = VM_THREAD_STATE_WAITING;
    printf("7\n");
    scheduler();
    printf("8\n");
    MachineFileRead(fd, addr, 512, MachineFileCallback, current_thread);
    printf("9\n");
    current_thread->thread_state = VM_THREAD_STATE_WAITING;
    printf("10\n");
    scheduler();
    for (int i = 0; i < 512/32; ++i)
    {
        printf("%p\n", addr);
        printf("11-%d\n", i);
        if (((unsigned char *)addr)[11] & 0xF) 
        {
            printf("12-%d\n", i);
        }
        else {
            printf("13-%d\n", i);
            entry* new_entry = new entry;
            for (int i = 0; i < 8; ++i)
            {
                if (((char *)addr)[i] != 0x20)
                {
                    new_entry->DShortFileName[i] = ((char *)addr)[i];
                }
            }
            new_entry->DShortFileName[i] = '.';
            for (int i = 0; i < 3; ++i)
            {
                if (((char *)addr)[i] != 0x20)
                {
                    new_entry->DShortFileName[8+i] = ((char *)addr)[i];
                }
            }
            new_entry->DSize = *((unsigned int*)addr + 28);
            new_entry->DAttributes = *((unsigned char*)addr+11);
            new_entry->cluster_location = *((uint16_t*)addr+26);
            // entry->DCreate = 
            // entry->DAccess = 
            // entry->DModify = 
            entry_vector.push_back(new_entry);
        }
        addr += 32;
    }
    printf("%d\n", entry_vector.size());
    VMMemoryPoolDeallocate(1, addr);
    VMMutexRelease(sector_mutex);
    MachineResumeSignals(sigstate);
}

void write_sector() {

}

///////////////////////// VMThread Functions ///////////////////////////
TVMStatus VMStart(int tickms, TVMMemorySize heapsize, int machinetickms, TVMMemorySize sharedsize, const char *mount, int argc, char *argv[]) { //The time in milliseconds of the virtual machine tick is specified by the tickms parameter, the machine responsiveness is specified by the machinetickms.
    typedef void (*TVMMainEntry)(int argc, char* argv[]);
    TVMMainEntry VMMain;
    VMMain = VMLoadModule(argv[0]);
    if (VMMain != NULL) {
        sharedsize = (sharedsize + 0xFFF) & (~0xFFF);
        void *shared_mem_base = MachineInitialize(machinetickms, sharedsize); //The timeout parameter specifies the number of milliseconds the machine will sleep between checking for requests.
        MachineRequestAlarm(tickms*1000, timerDecrement, NULL); // NULL b/c passing data through global vars
        MachineEnableSignals();
        // VM_MEMORY_POOL_SYSTEM
        uint8_t* base = new uint8_t[heapsize];
        MemoryPool* main_pool = new MemoryPool(heapsize, VM_MEMORY_POOL_ID_SYSTEM, base);
        mem_pool_vector.push_back(main_pool);
        // SHARED_MEMORY_POOL
        MemoryPool* shared_mem_pool = new MemoryPool(sharedsize, 1, (uint8_t*)shared_mem_base);
        mem_pool_vector.push_back(shared_mem_pool);
        // create main_thread
        TCB* main_thread = new TCB((unsigned int *)0, VM_THREAD_STATE_RUNNING, VM_THREAD_PRIORITY_NORMAL, 0, NULL, NULL, 0);
        thread_vector.push_back(main_thread);
        current_thread = main_thread;
        // create idle_thread
        idle_thread = new TCB((unsigned int *)1, VM_THREAD_STATE_DEAD, VM_THREAD_PRIORITY_IDLE, 0x100000, NULL, NULL, 0);
        idle_thread->thread_state = VM_THREAD_STATE_READY;
        MachineContextCreate(&(idle_thread->machine_context), idleEntry, NULL, idle_thread->stack_base, idle_thread->stack_size);
        // mount filesystem and read BPB
        MachineFileOpen(mount, O_RDWR, 0644, MachineFileCallback, current_thread);
        current_thread->thread_state = VM_THREAD_STATE_WAITING;
        scheduler();
        file_descriptor = current_thread->call_back_result;
        VMMutexCreate(&sector_mutex);
        the_bpb = new bpb;
        read_bpb(file_descriptor, 3);
        // bpb calculations
        the_bpb->root_dir_sectors  = (the_bpb->root_entry_count * 32) / 512;
        the_bpb->first_root_sector = the_bpb->reserved_sector_count + the_bpb->fat_count * the_bpb->fat_size;
        the_bpb->first_data_sector = the_bpb->first_root_sector + the_bpb->root_dir_sectors;
        the_bpb->cluster_count     = (the_bpb->sector_count_32 - the_bpb->first_data_sector) / the_bpb->sectors_per_cluster;
        // printf("bps %d\nspc %d\nrsc %d\nfc %d\nrec %d\nsc16 %d\nm %d\nfs %d\nspt %d\nhc %d\nsc32 %d\nfrs %d\nrds %d\nfds %d\ncc %d\n", the_bpb->bytes_per_sector, the_bpb->sectors_per_cluster, the_bpb->reserved_sector_count,
        //                                                                             the_bpb->fat_count, the_bpb->root_entry_count, the_bpb->sector_count_16, the_bpb->media, the_bpb->fat_size,
        //                                                                             the_bpb->sectors_per_track, the_bpb->head_count, the_bpb->sector_count_32,
        //                                                                             the_bpb->first_root_sector, the_bpb->root_dir_sectors, the_bpb->first_data_sector,
        //                                                                             the_bpb->cluster_count);
        // read FAT
        for (int i = 0; i < the_bpb->fat_size; ++i) {
            read_FAT(file_descriptor, (i+1)*512);
        }
        // for (int i = 0; i < fat_vector.size(); ++i)
        // {
        //     printf("%x\n", fat_vector[i]);
        // }
        // read root
        for (int i = the_bpb->first_root_sector; i < the_bpb->first_data_sector; ++i) {
            printf("#########%d\n", i);
            read_root(file_descriptor, i*512); 
        }
        // call VMMain
        VMMain(argc, argv);
        // unmount
        MachineFileClose(file_descriptor, MachineFileCallback, current_thread);
        current_thread->thread_state = VM_THREAD_STATE_WAITING;
        scheduler();
        return VM_STATUS_SUCCESS;
    }
    else {
        return VM_STATUS_FAILURE;
    }
}

TVMStatus VMThreadActivate(TVMThreadID thread) {
    MachineSuspendSignals(sigstate);
    TCB* actual_thread = thread_vector[thread];
    if (actual_thread->thread_state != VM_THREAD_STATE_DEAD) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    else {
        MachineContextCreate(&(actual_thread->machine_context), SkeletonEntry, actual_thread, actual_thread->stack_base, actual_thread->stack_size);
        actual_thread->thread_state = VM_THREAD_STATE_READY;
        determine_queue_and_push(actual_thread);
        if ((current_thread->thread_state == VM_THREAD_STATE_RUNNING && current_thread->thread_priority < actual_thread->thread_priority) || current_thread->thread_state != VM_THREAD_STATE_RUNNING) {
            scheduler();
        }
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid) {
    MachineSuspendSignals(sigstate);
    if (entry == NULL || tid == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }

    else {
        // NEED TO ALLOCATE SPACE FOR BASE OF THREADS FROM THE MAIN MEMPOOL, NOT USING NEW (look at constructor)
        TCB *new_thread = new TCB(tid, VM_THREAD_STATE_DEAD, prio, memsize, entry, param, 0);
        *(new_thread->id) = (TVMThreadID)thread_vector.size();
        thread_vector.push_back(new_thread);
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadDelete(TVMThreadID thread) {
    MachineSuspendSignals(sigstate);
    if (thread_vector[thread]->thread_state == VM_THREAD_STATE_DEAD) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    else {
        delete thread_vector[thread];
        thread_vector[thread] = NULL;
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadID(TVMThreadIDRef threadref) {
    MachineSuspendSignals(sigstate);
    if (threadref == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else {
        threadref = current_thread->id;
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadSleep(TVMTick tick){
    MachineSuspendSignals(sigstate);
    if (tick == VM_TIMEOUT_INFINITE) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else {
        current_thread->ticks_remaining = tick;
        current_thread->thread_state = VM_THREAD_STATE_WAITING;
        // determine_queue_and_push();
        scheduler();
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef stateref) {
    MachineSuspendSignals(sigstate);
    if(thread == VM_THREAD_ID_INVALID) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    if (stateref == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else {
        *stateref = thread_vector[thread]->thread_state;
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadTerminate(TVMThreadID thread) {
    MachineSuspendSignals(sigstate);
    if (thread_vector[thread]->thread_state == VM_THREAD_STATE_DEAD) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    else {
        thread_vector[thread]->thread_state = VM_THREAD_STATE_DEAD;
        determine_queue_and_remove(thread_vector[thread]);
        scheduler();
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

///////////////////////// VMFile Functions ///////////////////////////
TVMStatus VMFileClose(int filedescriptor) {
    MachineSuspendSignals(sigstate);
    current_thread->thread_state = VM_THREAD_STATE_WAITING;
    MachineFileClose(filedescriptor, MachineFileCallback, current_thread);
    scheduler();
    if (current_thread->call_back_result != -1) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
    else {
        MachineResumeSignals(sigstate);
        return VM_STATUS_FAILURE;
    }
}

TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor) {
    MachineSuspendSignals(sigstate);
    if (filename == NULL || filedescriptor == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else {
        current_thread->thread_state = VM_THREAD_STATE_WAITING;
        MachineFileOpen(filename, flags, mode, MachineFileCallback, current_thread);
        scheduler();
        *filedescriptor = current_thread->call_back_result;
        if (*filedescriptor != -1) {
            MachineResumeSignals(sigstate);
            return VM_STATUS_SUCCESS;
        }
        else {
            MachineResumeSignals(sigstate);
            return VM_STATUS_FAILURE;
        }
    }
}


// 1. Allocate location in shared memory.
// 2. Copy the data into the shared memory location.
// 3. Call MachineFileWrite with the new address so that it can be written (The new address is from the allocation from shared memory.)
// 4. Put thread to wait state
// 5. Schedule
// 6. When callback is called wake the thread
// 7. In the woken thread.  Deallocate the shared memory location.
TVMStatus VMFileWrite(int filedescriptor, void *data, int *length) {
    MachineSuspendSignals(sigstate);
    // printf("system_base: %p, shared_base: %p\n",mem_pool_vector[0]->base, mem_pool_vector[1]->base);
    if (data == NULL || length == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    // TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void **pointer) {
    void* addr;
    if (*length > 512) {
        VMMemoryPoolAllocate(1, 512, &addr);
    }
    else {
        VMMemoryPoolAllocate(1, (TVMMemorySize)(*length), &addr);
    }
    // printf("writing_chunk_base: %p\n",addr);
    // void * memcpy ( void * destination, const void * source, size_t num );
    current_thread->thread_state = VM_THREAD_STATE_WAITING;
    int temp_length = *length;
    while (temp_length > 0) {
        if (temp_length > 512) {
            memcpy(addr, data, 512);
            MachineFileWrite(filedescriptor, addr, 512, MachineFileCallback, current_thread);
            scheduler();
        }
        else {
            memcpy(addr, data, temp_length);
            MachineFileWrite(filedescriptor, addr, temp_length, MachineFileCallback, current_thread);
            scheduler();

        }
        temp_length -= 512;
        data += 512;
    }

    // TVMStatus VMMemoryPoolDeallocate(TVMMemoryPoolID memory, void *pointer) {
    VMMemoryPoolDeallocate(1, addr);
    if (current_thread->call_back_result != -1) {
        MachineResumeSignals(sigstate);
        // printf("done writing\n");
        return VM_STATUS_SUCCESS;
    }
    else {
        MachineResumeSignals(sigstate);
        return VM_STATUS_FAILURE;
    }
}

// 1. Allocate location in shared memory.
// 2. Call MachineFileRead with the new address so that it can be filled
// 3. Put thread to wait state
// 4. Schedule
// 5. When callback is called wake the thread
// 6. In the woken thread. Copy the read data from the shared memory location to the data location passed in with VMFileRead. Remember the data parameter is the destination and the shared memory location is the source.
// 7. Deallocate the shared memory location.
TVMStatus VMFileRead(int filedescriptor, void *data, int *length) {
    MachineSuspendSignals(sigstate);
    if (data == NULL || length == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    void* addr;
    if (*length > 512) {
        VMMemoryPoolAllocate(1, 512, &addr);
    }
    else {
        VMMemoryPoolAllocate(1, (TVMMemorySize)(*length), &addr);
    }
    current_thread->thread_state = VM_THREAD_STATE_WAITING;
    int temp_length = *length;
    *length = 0;
    while (temp_length > 0) {
        if (temp_length > 512) {
            MachineFileRead(filedescriptor, addr, 512, MachineFileCallback, current_thread);
            scheduler();
            memcpy(data, addr, 512);
            *length += current_thread->call_back_result;
        }
        else {
            MachineFileRead(filedescriptor, addr, temp_length, MachineFileCallback, current_thread);
            scheduler();
            memcpy(data, addr, temp_length);
            *length += current_thread->call_back_result;
        }
        temp_length -= 512;
        data += 512;
        // printf("%d\n", *length);
    }
    // VMMemoryPoolAllocate(1, (TVMMemorySize)(*length), &addr);
    // current_thread->thread_state = VM_THREAD_STATE_WAITING;
    // MachineFileRead(filedescriptor, addr, *length, MachineFileCallback, current_thread);
    // memcpy(data, addr, *length);
    VMMemoryPoolDeallocate(1, addr);
    if(*length > 0) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
    else {
        MachineResumeSignals(sigstate);
        return VM_STATUS_FAILURE;
    }
}

TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset) {
    MachineSuspendSignals(sigstate);
    current_thread->thread_state = VM_THREAD_STATE_WAITING;
    MachineFileSeek(filedescriptor, offset, whence, MachineFileCallback, current_thread);
    scheduler();
    if(newoffset != NULL) {
        *newoffset = current_thread->call_back_result;
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
    else {
        MachineResumeSignals(sigstate);
        return VM_STATUS_FAILURE;
    }
}


/////////////////////// VMMutex Functions ///////////////////////////
TVMStatus VMMutexCreate(TVMMutexIDRef mutexref) {
    MachineSuspendSignals(sigstate);
    if(mutexref == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    *mutexref = (unsigned int)mutex_vector.size();
    current_mutex = new Mutex(mutexref, (TVMThreadIDRef)-1);
    mutex_vector.push_back(current_mutex);
    MachineResumeSignals(sigstate);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexDelete(TVMMutexID mutex) {
    MachineSuspendSignals(sigstate);
    if(mutex_vector[mutex] == NULL || mutex >= mutex_vector.size()) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    else if(*(mutex_vector[mutex]->owner_id_ref) != -1) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    delete mutex_vector[mutex];
    mutex_vector[mutex] = NULL;
    MachineResumeSignals(sigstate);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexQuery(TVMMutexID mutex, TVMThreadIDRef ownerref) {
    MachineSuspendSignals(sigstate);
    if(ownerref == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else if(mutex >= mutex_vector.size() || mutex_vector[mutex] == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    else if(*(mutex_vector[mutex]->owner_id_ref) == -1) {
        MachineResumeSignals(sigstate);
        return VM_THREAD_ID_INVALID;
    }
    else {
        *ownerref = *(mutex_vector[mutex]->owner_id_ref);
    }
    MachineResumeSignals(sigstate);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout) {
    MachineSuspendSignals(sigstate);
    if(mutex > mutex_vector.size()) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    if(mutex_vector[mutex]->owner_id_ref == (TVMThreadIDRef)-1) {
        mutex_vector[mutex]->owner_id_ref = current_thread->id;
    }
    else if(timeout == VM_TIMEOUT_IMMEDIATE) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_FAILURE;
    }
    else if(timeout == VM_TIMEOUT_INFINITE) {
        current_thread->thread_state = VM_THREAD_STATE_WAITING;
        if(current_thread->thread_priority == VM_THREAD_PRIORITY_LOW) {
            mutex_vector[mutex]->mutex_low_priority_queue.push_back(current_thread);
        }
        else if(current_thread->thread_priority == VM_THREAD_PRIORITY_NORMAL) {
            mutex_vector[mutex]->mutex_normal_priority_queue.push_back(current_thread);
        }
        else if(current_thread->thread_priority == VM_THREAD_PRIORITY_HIGH) {
            mutex_vector[mutex]->mutex_high_priority_queue.push_back(current_thread);
        }
        scheduler();
    }
    else {
        determine_queue_and_push(current_thread);
        VMThreadSleep(timeout);
        if(mutex_vector[mutex]->owner_id_ref != current_thread->id) {
            MachineResumeSignals(sigstate);
            return VM_STATUS_FAILURE;
        }
    }
    MachineResumeSignals(sigstate);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexRelease(TVMMutexID mutex) {
    MachineSuspendSignals(sigstate);
    if(mutex_vector[mutex] == NULL || mutex >= mutex_vector.size()) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    else if(mutex_vector[mutex]->owner_id_ref == (TVMThreadIDRef)-1) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    else {
        if(!(mutex_vector[mutex]->mutex_high_priority_queue.empty())) {
            release(mutex, mutex_vector[mutex]->mutex_high_priority_queue);
        }
        else if(!(mutex_vector[mutex]->mutex_normal_priority_queue.empty())) {
            release(mutex, mutex_vector[mutex]->mutex_normal_priority_queue);
        }
        else if(!(mutex_vector[mutex]->mutex_low_priority_queue.empty())) {
            release(mutex, mutex_vector[mutex]->mutex_low_priority_queue);
        }
        else {
            mutex_vector[mutex]->owner_id_ref = (TVMThreadIDRef)-1;
        }
    }
    MachineResumeSignals(sigstate);
    return VM_STATUS_SUCCESS;
}

/////////////////////// VMMemoryPool Functions ///////////////////////////
TVMStatus VMMemoryPoolCreate(void *base, TVMMemorySize size, TVMMemoryPoolIDRef memory) {
    MachineSuspendSignals(sigstate);
    if(base == NULL || memory == NULL || size == 0) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else {
        *memory = (TVMMemoryPoolID)mem_pool_vector.size();
        MemoryPool* new_mem_pool = new MemoryPool(size, *memory, (uint8_t*)base);

        // printf("mempool id: %d\n", new_mem_pool->memory_pool_id);
        mem_pool_vector.push_back(new_mem_pool);
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMMemoryPoolDelete(TVMMemoryPoolID memory) {
    MachineSuspendSignals(sigstate);
    if(mem_pool_vector[memory] == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else if(!mem_pool_vector[memory]->alloc_list.empty()) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    else {
        delete mem_pool_vector[memory];
        mem_pool_vector[memory] = NULL;
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMMemoryPoolQuery(TVMMemoryPoolID memory, TVMMemorySizeRef bytesleft) {
    MachineSuspendSignals(sigstate);
    if(mem_pool_vector[memory] == NULL || bytesleft == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    else {
        *bytesleft = mem_pool_vector[memory]->free_space;
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMMemoryPoolAllocate(TVMMemoryPoolID memory, TVMMemorySize size, void **pointer) {
    MachineSuspendSignals(sigstate);
    if(mem_pool_vector[memory] == NULL || size == 0 || pointer == NULL) {
        MachineResumeSignals(sigstate);
        // printf("\n1: %d %d\n", memory, size);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else {
        mem_chunk *new_chunk = new mem_chunk();
        // round size up to next 64 byte chunk (eqn given in discussion)
        new_chunk->length = (size + 0x3F) & (~0x3F);
        list<mem_chunk*>::iterator it_free;
        // traverse free_list to find if space available
        for (it_free= mem_pool_vector[memory]->free_list.begin(); it_free !=mem_pool_vector[memory]->free_list.end(); ++it_free) {
            if((*it_free)->length >= new_chunk->length) {
                break;
            }
        }
        // if it_free reached end of free list then we know there isn't enough space for the new_chunk
        if ((*it_free)->length < new_chunk->length) {
            MachineResumeSignals(sigstate);
            // printf("\n2: %d %d\n", memory, size);
            return VM_STATUS_ERROR_INSUFFICIENT_RESOURCES;
        }
        else {
            list<mem_chunk*>::iterator it_alloc;
            // traverse alloc_list to find where to insert
            for (it_alloc= mem_pool_vector[memory]->alloc_list.begin(); it_alloc !=mem_pool_vector[memory]->alloc_list.end(); ++it_alloc) {
                if((*it_alloc)->base > (*it_free)->base) {
                    break;
                }
            }
            new_chunk->base = (*it_free)->base;
            // printf("new_chunk base: %p\n", new_chunk->base);
            *pointer = new_chunk->base;
            // insert new_chunk into correct spot
            mem_pool_vector[memory]->alloc_list.insert(it_alloc, new_chunk);
            // update amount of free_space
            mem_pool_vector[memory]->free_space -= new_chunk->length;
            // update free_list
            // printf("%x\n", (*it_free)->base);
            (*it_free)->base += new_chunk->length;
            // printf("%x\n", (*it_free)->base);
            (*it_free)->length -= new_chunk->length;
            // printf("free_list node size now = %d\n", (*it_free)->length);
            // if after updating the free list, the length is 0 then we know we used exactly the amount of free space in that chunk (no fragmentation) so we delete that node in the free_list
            if ((*it_free)->length == 0) {
                mem_pool_vector[memory]->free_list.erase(it_free);
                // printf("deleted node in free_list, list size now = %d\n", mem_pool_vector[memory]->free_list.size());
            }

            MachineResumeSignals(sigstate);
            return VM_STATUS_SUCCESS;
        }
    }
}

TVMStatus VMMemoryPoolDeallocate(TVMMemoryPoolID memory, void *pointer) {
    MachineSuspendSignals(sigstate);
    // printf("\ndealloc\n");
    if(mem_pool_vector[memory] == NULL || pointer == NULL) {
        MachineResumeSignals(sigstate);
        // printf("\n3: %d\n", memory);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else {
        MemoryPool* actual_mem_pool = mem_pool_vector[memory];
        list<mem_chunk*>::iterator it;
        for (it = actual_mem_pool->alloc_list.begin(); it !=actual_mem_pool->alloc_list.end(); ++it) {
            // printf("%x, %x\n", (*it)->base, (uint8_t*)pointer);
            if((*it)->base == (uint8_t*)pointer) {
                // printf("found chunk\n");
                break;
            }
        }
        // if pointer doesn't point to something in the alloc list
        if (it == actual_mem_pool->alloc_list.end()) {
            MachineResumeSignals(sigstate);
            return VM_STATUS_ERROR_INVALID_PARAMETER;
        }
        else {
            // store chunk in temp to be added to free list and remove from alloc list
            mem_chunk* dealloced_mem_chunk = *it;
            // printf("%x\n", (*it)->base);
            actual_mem_pool->alloc_list.erase(it);
            // printf("%x\n", (*it)->base);
            add_to_free_list(actual_mem_pool, dealloced_mem_chunk);
            // update amount of free space
            actual_mem_pool->free_space += dealloced_mem_chunk->length;
            MachineResumeSignals(sigstate);
            return VM_STATUS_SUCCESS;
        }
    }
}
} // end extern C
