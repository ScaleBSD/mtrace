#include <ext/hash_map>
#include <assert.h>
#include <cinttypes>

#include "json.hh"
#include "hash.h"

using namespace::std;
using namespace::__gnu_cxx;

//
// Handle OS X:
//  * uint64_t is a long long unsigned int
//  * there is no hash<long long unsigned int>
//
namespace __gnu_cxx {
template <>
struct hash<long long unsigned int> {
    size_t operator()(long long unsigned int k) const {
        static const uint64_t length = sizeof(k)/sizeof(uintptr_t);
        static_assert(length, "Bad length");
        return bb_hash((uintptr_t*)&k, length);
    }
};
}

struct SerialSection {
    SerialSection(void)
        : start(0),
          end(0),
          acquire_cpu(0),
          release_cpu(0),
          call_pc(0),
          acquire_pc(0),
          tid(0),
          per_pc_coherence_miss(),
          locked_inst(0) {}

    timestamp_t start;
    timestamp_t end;

    int acquire_cpu;
    int release_cpu;

    pc_t call_pc;
    pc_t acquire_pc;

    tid_t tid;

    map<pc_t, uint64_t> per_pc_coherence_miss;
    uint64_t locked_inst;
};

class LockManager {
    struct LockState {
        LockState(void)
            : ss_(), acquired_ts_(0), depth_(0) {}

        void release(const struct mtrace_lock_entry* lock) {
            depth_--;
            if (depth_ == 0) {
                ss_.end = lock->h.ts;
                ss_.release_cpu = lock->h.cpu;
            }
        }

        void acquire(const struct mtrace_lock_entry* lock) {
            if (depth_ == 0) {
                ss_.start = lock->h.ts;
                ss_.call_pc = mtrace_call_pc[lock->h.cpu];
                ss_.acquire_cpu = lock->h.cpu;
                ss_.acquire_pc = lock->pc;
                ss_.tid = mtrace_tid[lock->h.cpu];
            }
            depth_++;
        }

        void acquired(const struct mtrace_lock_entry* lock) {
            if (acquired_ts_ == 0) {
                acquired_ts_ = lock->h.ts;
                ss_.start = lock->h.ts;
                ss_.acquire_cpu = lock->h.cpu;
                ss_.acquire_pc = lock->pc;
                ss_.tid = mtrace_tid[lock->h.cpu];
            }
        }

        void access(const struct mtrace_access_entry* a) {
            if (a->traffic)
                ss_.per_pc_coherence_miss[a->pc]++;
            else if (a->lock)
                ss_.locked_inst++;
        }

        struct SerialSection ss_;
        uint64_t acquired_ts_;
        int depth_;
    };

    typedef hash_map<uint64_t, LockState*> LockStateTable;

public:
    bool release(const struct mtrace_lock_entry* lock, SerialSection& ss) {
        static int misses;
        LockState* ls;

        auto it = state_.find(lock->lock);
        if (it == state_.end()) {
            misses++;
            if (misses >= 20)
                die("LockManager: released too many unheld locks");
            return false;
        }

        ls = it->second;
        ls->release(lock);

        if (ls->depth_ == 0) {
            ss = ls->ss_;
            stack_.remove(ls);
            state_.erase(it);
            delete ls;
            return true;
        }
        return false;
    }

    void acquire(const struct mtrace_lock_entry* lock) {
        auto it = state_.find(lock->lock);

        if (it == state_.end()) {
            pair<LockStateTable::iterator, bool> r;
            LockState* ls = new LockState();
            r = state_.insert(pair<uint64_t, LockState*>(lock->lock, ls));
            if (!r.second)
                die("acquire: insert failed");
            stack_.push_front(ls);
            it = r.first;
        }
        it->second->acquire(lock);
    }

    void acquired(const struct mtrace_lock_entry* lock) {
        static int misses;

        auto it = state_.find(lock->lock);

        if (it == state_.end()) {
            misses++;
            if (misses >= 10)
                die("acquired: acquired too many missing locks");
            return;
        }
        it->second->acquired(lock);
    }

    bool access(const struct mtrace_access_entry* a, SerialSection& ss) {
        if (stack_.empty()) {
            ss.start = a->h.ts;
            ss.end = ss.start + 1;
            ss.acquire_cpu = a->h.cpu;
            ss.release_cpu = a->h.cpu;
            ss.call_pc = mtrace_call_pc[a->h.cpu];
            ss.acquire_pc = a->pc;
            if (a->traffic)
                ss.per_pc_coherence_miss[a->pc] = 1;
            else if (a->lock)
                ss.locked_inst = 1;
            return true;
        }

        stack_.front()->access(a);
        return false;
    }

private:
    LockStateTable state_;
    list<LockState*> stack_;
};

//
// A summary of every serial section and a per-acquire PC breakdown
//
class SerialSections : public EntryHandler {
    struct SerialSectionSummary {
        SerialSectionSummary(void):
            per_pc_coherence_miss(),
            ts_cycles( {0}),
                 acquires(0),
                 mismatches(0),
        locked_inst(0) {}


        void add(const SerialSection* ss) {
            if (ss->acquire_cpu != ss->release_cpu) {
                mismatches++;
                return;
            }

            if (ss->end < ss->start)
                die("SerialSectionSummary::add %"PRIu64" < %"PRIu64,
                    ss->end, ss->start);

            ts_cycles[ss->acquire_cpu] += ss->end - ss->start;
            auto it = ss->per_pc_coherence_miss.begin();
            for (; it != ss->per_pc_coherence_miss.end(); ++it)
                per_pc_coherence_miss[it->first] += it->second;

            locked_inst += ss->locked_inst;
            acquires++;
        }

        timestamp_t total_cycles(void) const {
            timestamp_t sum = 0;
            int i;

            for (i = 0; i < MAX_CPUS; i++)
                sum += ts_cycles[i];
            return sum;
        }

        uint64_t coherence_misses(void) const {
            uint64_t sum = 0;

            auto it = per_pc_coherence_miss.begin();
            for (; it != per_pc_coherence_miss.end(); ++it)
                sum += it->second;

            return sum;
        }

        map<pc_t, uint64_t> per_pc_coherence_miss;
        timestamp_t ts_cycles[MAX_CPUS];
        uint64_t acquires;
        uint64_t mismatches;
        uint64_t locked_inst;
    };

    struct SerialSectionStat {
        SerialSectionStat(void) :
            lock_id(0),
            obj_id(0),
            name("") {}

        void add(const SerialSection* ss) {
            summary.add(ss);
            per_pc[ss->acquire_pc].add(ss);
            per_tid[ss->tid].add(ss);
        }

        void init(const MtraceObject* object, const struct mtrace_lock_entry* l) {
            lock_id = l->lock;
            obj_id = object->id_;
            name = l->str;
            lock_section = true;
        }

        void init(const MtraceObject* object, const struct mtrace_access_entry* a, string str) {
            lock_id = a->guest_addr;
            obj_id = object->id_;
            name = str;
            lock_section = false;
        }

        // Set by init
        uint64_t lock_id;
        uint64_t obj_id;
        string name;
        bool lock_section;

        // Updated by add
        SerialSectionSummary summary;
        map<pc_t, SerialSectionSummary> per_pc;
        map<tid_t, SerialSectionSummary> per_tid;
    };

    struct SerialSectionKey {
        uint64_t lock_id;
        uint64_t obj_id;
    };

public:
    SerialSections(void);
    virtual void handle(const union mtrace_entry* entry);
    virtual void exit(void);
    virtual void exit(JsonDict* json_file);
    timestamp_t total_cycles(void) const;
    uint64_t coherence_misses(void) const;

private:
    LockManager lock_manager_;

    struct SerialEq {
        bool operator()(const SerialSectionKey s1,
                        const SerialSectionKey s2) const
        {
            return (s1.lock_id == s2.lock_id) && (s1.obj_id == s2.obj_id);
        }
    };

    struct SerialHash {
        size_t operator()(const SerialSectionKey s) const {
            static const uint64_t length = sizeof(s)/sizeof(uintptr_t);
            register uintptr_t* k = (uintptr_t*)&s;
            static_assert(length == 2, "Bad length");

            return bb_hash(k, length);
        }
    };

    void populateSummaryDict(JsonDict* dict, SerialSectionSummary* sum);
    void handle_lock(const struct mtrace_lock_entry* l);
    void handle_access(const struct mtrace_access_entry* a);

    hash_map<SerialSectionKey, SerialSectionStat, SerialHash, SerialEq> stat_;
};
