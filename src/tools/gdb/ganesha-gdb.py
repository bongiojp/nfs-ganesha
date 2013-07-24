#!/usr/bin/python
#from __future__ import with_statement
import gdb
import sys
import re
#example: http://tromey.com/blog/?p=501
#API: http://sourceware.org/gdb/onlinedocs/gdb/Python-API.html#Python-API

#CONFIG for 2.0
class Ganesha20:
    CIREAD_FUNC = "cache_inode_rdwr"
    CIREAD_CONTENTLOCK_REF = "&entry->content_lock" # from ciread_func
    CIREAD_CONTENTLOCK = "entry->content_lock" # from ciread_func

    FSALREAD_FUNC = "FSAL_read"
    FSALWRITE_FUNC = "FSAL_write"

    NFSPROTO_READ_FUNC = "nfs_Read"
    NFSPROTO_WRITE_FUNC = "nfs_Write"

    WRLOCK_FUNC = "pthread_rwlock_wrlock"
    RDLOCK_FUNC = "pthread_rwlock_rdlock"

    WORKERTHREAD_FUNC = "worker_run"
#CONFIG for 1.5
class Ganesha15:
    CIREAD_FUNC = "cache_inode_rdwr"
    CIREAD_CONTENTLOCK_REF = "&entry->content_lock" # from ciread_func
    CIREAD_CONTENTLOCK = "entry->content_lock" # from ciread_func

    FSALREAD_FUNC = "FSAL_read"
    FSALWRITE_FUNC = "FSAL_write"

    NFSPROTO_READ_FUNC = "nfs_Read"
    NFSPROTO_WRITE_FUNC = "nfs_Write"

    WRLOCK_FUNC = "pthread_rwlock_wrlock"
    RDLOCK_FUNC = "pthread_rwlock_rdlock"

    WORKERTHREAD_FUNC = "worker_thread"
class PrintLockOwners(gdb.Command):
    """print owner of locks

Iterate through each thread that is blocked on a lock. Print
inode entry info and lock and the lock owner."""
    def __init__(self):
        self.ganeshafunc = self.getGaneshaConfig()
        # This is where we register the new function with GDB
        super (PrintLockOwners, self).__init__ ("print_lock_owners",
                                                gdb.COMMAND_SUPPORT,
                                                gdb.COMPLETE_NONE, True)
#        gdb.Command.__init__(self, "print_lock_owners", gdb.COMMAND_DATA, gdb.COMPLETE_SYMBOL, True)
    def invoke(self, arg, from_tty):
        if sys.version_info[0] == 2 and sys.version_info[2] > 4:
            arg_list = gdb.string_to_argv(arg)
        else: # python version <= 2.4 
            arg_list = [arg]

        if len(arg_list) < 1:
            # CI = Cache Inode
            print("usage: print_lock_owners [CI_RW|BACK]")
            return
        {'CI_RW': self.find_ci_rw_locks,
         'BACK': self.printbacktrace,
         }.get(arg_list[0], self.functionnotfound)()
            
        return

    # Functon to figure out if we're looking at ganesha 2.0 or ganesha 1.5
    # return the ganesha config class
    def getGaneshaConfig(self):
        unique_20lib = "libfsal"
        FILES_QUERY = "info files"
        
        # Let's see if we loaded a libfsal object file, which only exists in ganesha >2.0
        for obj in gdb.objfiles():
            result = re.search(r".*libfsal.*", obj.filename, re.MULTILINE|re.IGNORECASE)
            if result:
                print("Looks like Ganesha 2.0, loading the appropriate config ...")
                return Ganesha20()                
        # We did not find an libfsal object file, must be Ganesha 1.5
        print("Looks like Ganesha 1.5, loading the appropriate config ...")
        return Ganesha15()        
    def parseAndEvaluate(self,exp):
        if gdb.VERSION.startswith("6.8.50.2009"):
            return gdb.parse_and_eval(exp)
        # Work around non-existing gdb.parse_and_eval as in released 7.0
        gdb.execute("set logging redirect on")
        gdb.execute("set logging on")
        gdb.execute("print %s" % exp)
        gdb.execute("set logging off")
        return gdb.history(0)
    
    def printbacktrace(self):
        print("Printing backtrace")
        ganesha_process = gdb.inferiors()[0] # there should only be one inferior process
                                             # "inferior" refers to any process object
                                             # inferior to the GDB process.
        for thread in ganesha_process.threads():
            print("Thread ", thread.num)
            if sys.version_info[0] == 2 and sys.version_info[2] > 4:
                print(" name:", thread.name)
    
    
            thread.switch() # switch to this thread so we can explore further
            currframe = gdb.selected_frame()
            while currframe != None:
                #print "function:", currframe.name(), " resumeaddr:", currframe.pc(), " block:", currframe.block()
                currsymtab = currframe.find_sal()
                out = ""
                if currsymtab.pc != 0:
                    out += "0x%x " % currsymtab.pc
                if sys.version_info[0] == 2 and sys.version_info[2] > 4:
                    out += "to %lx" % currsymtab.lastfg
                out += "%s () " % currframe.name()
                if currsymtab.symtab != None:
                    out += "at %s" % currsymtab.symtab.filename
                    out += ":%lu " % currsymtab.line
    
                print(out)
                currframe = currframe.older()
            print("\n")
        return    
    
    def find_ci_rw_locks(self):
        print("Looking at threads waiting on Cache inode RW locks")
        ganesha_process = gdb.inferiors()[0] # there should only be one inferior process
                                             # "inferior" refers to any process object
                                             # inferior to the GDB process.
        totalthreads = 0
        totalworkerthreads = 0
        totalreading = 0
        totalwriting = 0
        totalwaitstoreadonread = 0
        totalwaitstoreadonwrite = 0
        totalwaitstowrite = 0
        totallocks = 0
        dictoflocks = {}
        dictoflockdata = {}
    
        dictofowners = {}
    
        parse_lockdata = re.compile(r".*readers = (\d+).*readers_wakeup = (\d+)"
                                    ".*writer_wakeup = (\d+).*readers_queued = (\d+)"
                                    ".*writers_queued = (\d+).*writer = (\d+).*shared"
                                    " = (\d+).*", re.MULTILINE|re.IGNORECASE)
    
        for thread in ganesha_process.threads():
#            if sys.version_info[0] == 2 and sys.version_info[2] > 4:
#                print("Thread name: ", thread.name)
            #print "Thread  ", thread.num
    
            thread.switch() # switch to this thread so we can explore further
            totalthreads += 1
            currframe = gdb.selected_frame()
            lockdata = None
            lockptr = None
            entryhandle = None
    
            while currframe != None:
                if currframe.name() == self.ganeshafunc.FSALREAD_FUNC:
                    currframe = currframe.older()
                    while currframe != None:
                        if currframe.name() == self.ganeshafunc.CIREAD_FUNC:
                            # Make this frame the current frame in GDB
                            currframe.select()
                            lockptr = gdb.parse_and_eval(self.ganeshafunc.CIREAD_CONTENTLOCK_REF)
                            lockptr_str = "%s"%lockptr
                            if not dictofowners.has_key(lockptr_str):
                                dictofowners[lockptr_str] = [thread.num]
                            else:
                                dictofowners[lockptr_str].append(thread.num)
                        currframe = currframe.older()
                if currframe != None and currframe.name() == self.ganeshafunc.FSALWRITE_FUNC:
                    print("")
    
                if currframe != None and (currframe.name() == self.ganeshafunc.WRLOCK_FUNC
                                          or currframe.name() == self.ganeshafunc.RDLOCK_FUNC):
                    if currframe.name() == self.ganeshafunc.WRLOCK_FUNC:
                        locktype = "W"
                    else: # currframe.name() == self.ganeshafunc.RDLOCK_FUNC
                        locktype = "R"
                    #print "Found a thread waiting on ci rw_lock."
                    currframe = currframe.older()
                    while currframe != None:
                        if currframe.name() == self.ganeshafunc.CIREAD_FUNC:
                            # Make this frame the current frame in GDB
                            currframe.select()
    
                            # This has data on queued readers/writers, total readers, etc.
                            lockptr = gdb.parse_and_eval(self.ganeshafunc.CIREAD_CONTENTLOCK_REF)
    
                            lockptr_str = "%s"%lockptr
                            if not dictoflocks.has_key(lockptr_str):
                                dictoflocks[lockptr_str] = 1
                                
                                lockdata = gdb.parse_and_eval(self.ganeshafunc.CIREAD_CONTENTLOCK)
                                lockdata_str = "%s"%lockdata
                                result = parse_lockdata.match(lockdata_str)
                                if result != None:
                                    nr_readers = result.group(1)
                                    readers_wakeup = result.group(2)
                                    writers_wakeup = result.group(3)
                                    nr_readers_queued = result.group(4)
                                    nr_writers_queued = result.group(5)
                                    writer = result.group(6)
                                    shared = result.group(7)
                                    dictoflockdata[lockptr_str] = (nr_readers, readers_wakeup, writers_wakeup,
                                                                   nr_readers_queued, nr_writers_queued,
                                                                   writer, shared)
                            else:
                                dictoflocks[lockptr_str] += 1
                        if currframe.name() == self.ganeshafunc.NFSPROTO_READ_FUNC:
                            #print "Found the line where we're waiting to READ"
                            if locktype == "W":
                                totalwaitstoreadonwrite += 1
                            else: # locktype == "R":
                                totalwaitstoreadonread += 1
                            totalreading += 1
                        if currframe.name() == self.ganeshafunc.NFSPROTO_WRITE_FUNC:
                            #print "Found the line where we're waiting to WRITE"
                            totalwaitstowrite += 1
                            totalwriting += 1
                        if currframe != None and currframe.name() == self.ganeshafunc.WORKERTHREAD_FUNC:
                            totalworkerthreads += 1
                        currframe = currframe.older()
                        
                if currframe != None and currframe.name() == self.ganeshafunc.NFSPROTO_READ_FUNC:
                    #print "Found the line where we're reading but not waiting"
                    totalreading += 1
    
                    # Find which lock this owns
                if currframe != None and currframe.name() == self.ganeshafunc.NFSPROTO_WRITE_FUNC:
                    #print "Found the line where we're writing but not waiting"
                    totalwriting += 1

                if currframe != None and currframe.name() == self.ganeshafunc.WORKERTHREAD_FUNC:
                    totalworkerthreads += 1
    
                    # Find which lock this owns
                if currframe != None:
                    currframe = currframe.older()
        
        print("total threads: ", totalthreads)
        print("total worker threads: ", totalworkerthreads)
        print("total waiting to read with a readlock: ", totalwaitstoreadonread)
        print("total waiting to read with a writelock: ", totalwaitstoreadonwrite)
        print("total waiting to write: ", totalwaitstowrite)
        print("total threads reading: ", totalreading)
        print("total threads writing: ", totalwriting)
        print("total num of locks: ", len(dictoflocks.keys())) # how many locks is the contention over?
        for lock_str in dictoflocks.keys():
            print("threads waiting on lock ", lock_str, ": ", dictoflocks[lock_str])
            (nr_readers, readers_wakeup, writers_wakeup,
             nr_readers_queued, nr_writers_queued,
             writer, shared) = dictoflockdata[lock_str]
            print("    nr_readers: ",nr_readers)
            print("    nr_readers_wakeup: ",readers_wakeup)
            print("    writers_wakeup: ",writers_wakeup)
            print("    nr_readers_queued: ",nr_readers_queued)
            print("    nr_writers_queued: ",nr_writers_queued)
            print("    writer: ",writer)
            print("    shared: ",shared)
        for owned_lock in dictofowners.keys():
            print("Lock ", owned_lock, " owned by:")
            for threadnum in dictofowners[owned_lock]:
                print("    thread ", threadnum)
        return
    
    def functionnotfound(self):
        print("Invalid argument. Supported args are CI_RW or BACK.")
        return

# Now register the commands:
PrintLockOwners()


        
