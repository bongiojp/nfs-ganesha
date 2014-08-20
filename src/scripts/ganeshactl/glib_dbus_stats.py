#!/usr/bin/python

# You must initialize the gobject/dbus support for threading
# before doing anything.
import gobject
import sys
import time

gobject.threads_init()

from dbus import glib
glib.init_threads()

# Create a session bus.
import dbus

class RetrieveStats():
    def __init__(self):
        self.dbus_service_name = "org.ganesha.nfsd"
        self.dbus_exportstats_name = "org.ganesha.nfsd.exportstats"
        self.dbus_exportmgr_name = "org.ganesha.nfsd.exportmgr"
        self.dbus_clientstats_name = "org.ganesha.nfsd.clientstats"
        self.export_interface = "/org/ganesha/nfsd/ExportMgr"
        self.client_interface = "/org/ganesha/nfsd/ClientMgr"

        self.bus = dbus.SystemBus()
        try:
            self.exportmgrobj = self.bus.get_object(self.dbus_service_name,
                                self.export_interface)
            self.clientmgrobj = self.bus.get_object(self.dbus_service_name,
                                    self.client_interface)
        except:
            print "Error: Can't talk to ganesha service on d-bus. Looks like Ganesha is down"
            sys.exit()

    # NFSv3/NFSv4/NLM/MNT/QUOTA stats over all exports
    def fast_stats(self):
        stats_op = self.exportmgrobj.get_dbus_method("GetFastOPS",
                                 self.dbus_exportstats_name)
        return stats_op(0)
    # NFSv3/NFSv40/NFSv41/NFSv42/NLM4/MNTv1/MNTv3/RQUOTA totalled over all exports
    def global_stats(self):
        stats_op = self.exportmgrobj.get_dbus_method("GetGlobalOPS",
                                 self.dbus_exportstats_name)
        return stats_op(0)
    # cache inode stats
    def inode_stats(self):
        stats_op = self.exportmgrobj.get_dbus_method("ShowCacheInode",
                                 self.dbus_exportstats_name)
        return stats_op(0)
    # list of all exports
    def export_stats(self):
        stats_op = self.exportmgrobj.get_dbus_method("ShowExports",
                                 self.dbus_exportmgr_name)
        return stats_op()

    # delegation stats related to a single client ip
    def deleg_stats(self, ip):
        stats_op = self.clientmgrobj.get_dbus_method("GetDelegations",
                          self.dbus_clientstats_name)
        return stats_op(ip)    # return stats_op(str(ip))
    # NFSv3/NFSv4/NLM/MNT/QUOTA stats totalled for a single export
    def total_stats(self, export_id):
        stats_op = self.exportmgrobj.get_dbus_method("GetTotalOPS",
                                 self.dbus_exportstats_name)
        stats_dict = {}
        if export_id < 0:
            export_list = self.export_stats()
            for exports in export_list[1]:
                export_id=exports[0]
                stats_dict[export_id] = stats_op(export_id)
        else:
            stats_dict[export_id] = stats_op(export_id)
        return stats_dict

    def io_stats(self, stats_op, export_id):
        stats_dict = {}
        if export_id < 0:
            export_list = self.export_stats()
            for exports in export_list[1]:
                export_id=exports[0]
                stats_dict[export_id] = stats_op(export_id)
            return stats_dict
        else:
            stats_dict[export_id] = stats_op(export_id)
            return stats_dict
    def v3io_stats(self, export_id):
        stats_op =  self.exportmgrobj.get_dbus_method("GetNFSv3IO",
                                  self.dbus_exportstats_name)
        return self.io_stats(stats_op, export_id)
    def v4io_stats(self, export_id):
        stats_op = self.exportmgrobj.get_dbus_method("GetNFSv40IO",
                                 self.dbus_exportstats_name)
        return self.io_stats(stats_op, export_id)
    def pnfs_stats(self, export_id):
        stats_op = self.exportmgrobj.get_dbus_method("GetNFSv41Layouts",
                                 self.dbus_exportstats_name)
        stats_dict = {}
        if export_id < 0:
            export_list = self.export_stats()
            for exports in export_list[1]:
                export_id=exports[0]
                stats_dict[export_id] = stats_op(export_id)
            return stats_dict
        else:
            stats_dict[export_id] = stats_op(export_id)
            return stats_dict

# prints the stats returned by RetrieveStats() retrieved from the ganesha dbus interface.
class PrintStats():
    def print_deleg(self, stats):
        if stats[1] != "OK":
            print "GANESHA RESPONSE STATUS: ", stats[1]
        else:
            print "GANESHA RESPONSE STATUS: ", stats[1]
            print stats
            print "Timestamp: ", time.ctime(stats[2][0]), stats[2][1], " nsecs"
            print "Confirmed: ", stats[3][0]
            print "Current Delegations: ", stats[3][1]
            print "Current Recalls: ", stats[3][2]
            print "Current Failed Recalls: ", stats[3][3]
            print "Current Number of Revokes: ", stats[3][4]
            print
    # print list of exports
    def print_export(self, exports):
        print "Timestamp: ", time.ctime(exports[0][0]), exports[0][1], " nsecs"
        print "Exports:"
        print
        for export in exports[1]:
            print "export id: ", export[0]
            print "path: ", export[1]
            print "NFSv3 stats available: ", export[2]
            print "NFSv4.0 stats available: ", export[6]
            print "NFSv4.1 stats available: ", export[7]
            print "NFSv4.2 stats available: ", export[8]
            print "MNT stats available: ", export[3]
            print "NLMv4 stats available: ", export[4]
            print "RQUOTA stats available: ", export[5]
            print "9p stats available: ", export[9]
            print
    # total operations done globally (over all exports)
    def print_global(self, stats):
        if stats[1] != "OK":
            print "No NFS activity, GANESHA RESPONSE STATUS: ", stats[1]
        else:
            print "Timestamp: ", time.ctime(stats[2][0]), stats[2][1], " nsecs"
            print "Global Total:"
            print
            print "%s: %d" % (stats[3][0], stats[3][1]) # NFSv3
            print "%s: %d" % (stats[3][2], stats[3][3]) # NFSv4.0
            print "%s: %d" % (stats[3][4], stats[3][5]) # NFSv4.1
            print "%s: %d" % (stats[3][6], stats[3][7]) # NFSv4.2
            print
    def print_inode(self, stats):
        if stats[1] != "OK":
            print "No NFS activity, GANESHA RESPONSE STATUS: ", stats[1]
        else:
            print "Timestamp: ", time.ctime(stats[2][0]), stats[2][1], " nsecs"
            print "Inode cache:"
            print
            print "%s: %d" % (stats[3][0], stats[3][1]) # cache requests
            print "%s: %d" % (stats[3][2], stats[3][3]) # cache hits
            print "%s: %d" % (stats[3][4], stats[3][5]) # cache misses
            print "%s: %d" % (stats[3][6], stats[3][7]) # cache conf
            print "%s: %d" % (stats[3][8], stats[3][9]) # cache added
            print "%s: %d" % (stats[3][10], stats[3][11]) # cache mapping
            print
    def print_fast(self, stats):
        if stats[1] != "OK":
            print "No NFS activity, GANESHA RESPONSE STATUS: ", stats[1]
        else:
            print "Timestamp: ", time.ctime(stats[2][0]), stats[2][1], " nsecs"
            print "Global ops:"
            print
            # NFSv3, NFSv4, NLM, MNT, QUOTA stats
            for i in range(0,len(stats[3])-1):
                if ":" in str(stats[3][i]):
                    print stats[3][i]
                elif str(stats[3][i]).isdigit():
                    print "\t%s" % (str(stats[3][i]).rjust(8))
                else:
                    print "%s: " % (stats[3][i].ljust(20)),
            print
    def print_iov3(self, stats):
        for key in stats:
            if stats[key][1] != "OK":
                print "EXPORT %s: %s" % (key, stats[key][1])
                continue
            print "stats for EXPORT %s" % (key)
            print "\t\trequested\ttransferred\t     total\t    errors\t   latency\tqueue wait"
            print "READv3: ",
            for stat in stats[key][3]:
                print "\t", str(stat).rjust(8),
            print "\nWRITEv3: ",
            for stat in stats[key][4]:
                print "\t", str(stat).rjust(8),
            print
    def print_iov4(self, stats):
        for key in stats:
            if stats[key][1] != "OK":
                print "EXPORT %s: %s" % (key, stats[key][1])
                continue
            print "stats for EXPORT %s" % (key)
            print "\t\trequested\ttransferred\t     total\t    errors\t   latency\tqueue wait"
            print "READv4: ",
            for stat in stats[key][3]:
                print "\t", str(stat).rjust(8),
            print "\nWRITEv4: ",
            for stat in stats[key][4]:
                print "\t", str(stat).rjust(8),
            print
    def print_total(self, stats):
        for key in stats:
            if stats[key][1] != "OK":
                print "No NFS activity, GANESHA RESPONSE STATUS: ", stats[key][1]
            else:
                print "Total stats for export id", key
                print "Timestamp: ", time.ctime(stats[key][2][0]), stats[key][2][1], " nsecs"
                for i in range(0,len(stats[key][3])-1, 2):
                    print "%s: %s" % (stats[key][3][i], stats[key][3][i+1])
                print
    def print_pnfs(self, stats):
        for key in stats:
            if stats[key][1] != "OK":
                print "No NFS activity, GANESHA RESPONSE STATUS: ", stats[key][1]
            else:
                print "Total stats for export id", key
                print "Timestamp: ", time.ctime(stats[key][2][0]), stats[key][2][1], " nsecs"
                print "Statistics for:",exports[1],"\n\t\ttotal\terrors\tdelays"
                print 'getdevinfo ',
                for stat in stats[key][3]:
                    print '\t', stat,
                print
                print 'layout_get ',
                for stat in stats[key][4]:
                    print '\t', stat,
                print
                print 'layout_commit ',
                for stat in stats[key][5]:
                    print '\t', stat,
                print
                print 'layout_return ',
                for stat in stats[key][6]:
                    print '\t', stat,
                print
                print 'recall ',
                for stat in stats[key][7]:
                    print '\t', stat,
                print

