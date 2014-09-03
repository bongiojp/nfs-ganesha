#!/usr/bin/python

import gobject
import sys
import time
import re
import glib_dbus_stats

def menu():
    print "Command requires one specific option from this list:"
    print "%s [list_clients | deleg <ip address> | global | inode | iov3 [export id] | iov4 [export id] | export | total | fast | pnfs [export id] ]" % (sys.argv[0])
    sys.exit()

if len(sys.argv) < 2:
    menu()

command = sys.argv[1]

# check arguments
matches = re.search('help|list_clients|deleg|global|inode|iov3|iov4|export|total|fast|pnfs', command)
if not matches or not matches.group(0):
    print "Option \"%s\" is not correct." % (command)
    menu()
elif command == "help":
    menu()
# requires an IP address
elif command == "deleg":
    if not len(sys.argv) == 3:
        print "Option \"%s\" must be followed by an ip address." % (command)
        menu()
    command_arg = sys.argv[2]
# optionally accepts an export id
elif "iov" in command or command == "total" or command == "pnfs":
    if (not len(sys.argv) < 3) and sys.argv[2].isdigit():
        command_arg = sys.argv[2]
    else:
        command_arg = -1

# retrieve and print stats
exp_interface = glib_dbus_stats.RetrieveExportStats()
cl_interface = glib_dbus_stats.RetrieveClientStats()
if command == "global":
    print exp_interface.global_stats()
elif command == "export":
    print exp_interface.export_stats()
elif command == "inode":
    print exp_interface.inode_stats()
elif command == "fast":
    print exp_interface.fast_stats()
elif command == "list_clients":
    print cl_interface.list_clients()
elif command == "deleg":
    print cl_interface.deleg_stats(command_arg)
elif command == "iov3":
    print exp_interface.v3io_stats(command_arg)
elif command == "iov4":
    print exp_interface.v4io_stats(command_arg)
elif command == "total":
    print exp_interface.total_stats(command_arg)
elif command == "pnfs":
    print exp_interface.pnfs_stats(command_arg)
