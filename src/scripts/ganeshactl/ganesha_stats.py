#!/usr/bin/python

import gobject
import sys
import time
import re
import glib_dbus_stats

def menu():
    print "Command requires a specific command or commands:"
    print "%s [DELEG <ip address> | GLOBAL | INODE | IOv3 [export id] | IOv4 [export id] | EXPORT | TOTAL | FAST | PNFS [export id] ]" % (sys.argv[0])
    sys.exit()

if len(sys.argv) < 2:
    menu()

# check arguments
commands = {}
for i in range(1, len(sys.argv)):
    matches = re.search('DELEG|GLOBAL|INODE|IOv3|IOv4|EXPORT|TOTAL|FAST|PNFS', sys.argv[i])
    if not matches or not matches.group(0):
        print "Argument %d \"%s\" is not correct." % (i, sys.argv[i])
        menu()

    if sys.argv[i] in commands:
        continue

    match = matches.group(0)

    # requires an IP address
    if match == "DELEG":
        if i == len(sys.argv) - 1:
            print "Argument %d (%s) must be followed by an ip address." % (i, sys.argv[i])
            menu()
        commands[sys.argv[i]] = sys.argv[i+1]
        i = i + 1
        continue

    # optionally accepts an export id
    if "IOv" in match or match == "TOTAL" or match == "PNFS":
        if (i != len(sys.argv) - 1) and (sys.argv[i+1].isdigit()):
            commands[sys.argv[i]] = sys.argv[i+1]
            i = i + 1
        else:
            commands[sys.argv[i]] = -1
        continue
    commands[sys.argv[i]] = 1

# retrieve and print stats
interface = glib_dbus_stats.RetrieveStats()
printstats = glib_dbus_stats.PrintStats()
for key in commands:
    print "----------------------------------------------------------------------------"
    if key == "GLOBAL":
        stats = interface.global_stats()
        printstats.print_global(stats)
    if key == "EXPORT":
        exports = interface.export_stats()
        printstats.print_export(exports)
    if key == "INODE":
        stats = interface.inode_stats()
        printstats.print_inode(stats)
    if key == "FAST":
        stats = interface.fast_stats()
        printstats.print_fast(stats)
    if key == "DELEG":
        stats = interface.deleg_stats(commands[key])
        printstats.print_deleg(stats)
    if key == "IOv3":
        stats = interface.v3io_stats(commands[key])
        printstats.print_iov3(stats)
    if key == "IOv4":
        stats = interface.v4io_stats(commands[key])
        printstats.print_iov4(stats)
    if key == "TOTAL":
        stats = interface.total_stats(commands[key])
        printstats.print_total(stats)
    if key == "PNFS":
        stats = interface.pnfs_stats(commands[key])
        printstats.print_pnfs(stats)

print "----------------------------------------------------------------------------"

