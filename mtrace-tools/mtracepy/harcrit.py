import sqlite3
import summary
import lock
from util import uhex
import model

class MtraceHarcrit:

    def __init__(self, labelType, labelId, conn, dataName, missDelay):
        self.labelType = labelType
        self.labelId = labelId
        self.conn = conn
        self.dataName = dataName
        self.missDelay = missDelay

        self.lock = 0

        self.exclusiveHoldTime = None
        self.name = None
        self.tids = None
        self.cpus = None
        self.pcs = None
        self.inited = False

    def __init_state(self):
        if self.inited:
            return

        c = self.conn.cursor()
        self.exclusiveHoldTime = 0
        self.cpus = {}
        self.tids = {}
        self.pcs = {}

        q = 'SELECT access_id, tid, cpu, pc from %s_accesses WHERE label_id = %lu AND locked_id = 0'
        q = q % (self.dataName, self.labelId)
        c.execute(q)
        for row in c:
            section = lock.MtraceSerialSection(row[0], 0, self.missDelay, 
                                               row[2], 0, row[1], row[3])
            self.exclusiveHoldTime += self.missDelay

            time = self.missDelay
            if section.tid in self.tids:
                time += self.tids[section.tid]
            self.tids[section.tid] = time

            time = self.missDelay
            if section.startCpu in self.cpus:
                time += self.cpus[section.startCpu]
            self.cpus[section.startCpu] = time

            entry = [ self.missDelay, 1 ]
            if section.pc in self.pcs:
                old = self.pcs[section.pc]
                entry = [ old[0] + self.missDelay, 
                          old[1] + 1 ]
            self.pcs[section.pc] = entry
        
        # Name
        q = 'SELECT str FROM %s_labels%u WHERE label_id = %lu'
        q = q % (self.dataName,
                 self.labelType,
                 self.labelId)
        c.execute(q)
        rs = c.fetchall()
        if len(rs) != 1:
            raise Exception('%s returned %u rows' % (query, len(rs)))
        self.name = rs[0][0]

        self.inited = True

    def get_exclusive_hold_time(self):
        self.__init_state()
        return self.exclusiveHoldTime
    def get_name(self):
        self.__init_state()
        return self.name
    def get_label_id(self):
        return self.labelId
    def get_tids(self):
        self.__init_state()
        return self.tids
    def get_cpus(self):
        self.__init_state()
        return self.cpus
    def get_lock(self):
        self.__init_state()
        return self.lock
    def get_pcs(self):
        self.__init_state()
        return self.pcs

def get_harcrits(dbFile, dataName):
    conn = sqlite3.connect(dbFile)
    c = conn.cursor()

    q = 'SELECT DISTINCT label_type, label_id FROM %s_accesses WHERE locked_id = 0;'
    q = q % (dataName)
    c.execute(q)

    lst = []
    for row in c:
        if row[0] == 0:
            continue
        lst.append(MtraceHarcrit(row[0], row[1], conn, dataName, model.MISS_LATENCY))
    return lst