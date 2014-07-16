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
bus = dbus.SystemBus()


if len(sys.argv) != 2:
	print "Command requires one and only one argument, the ip address of the client."
	sys.exit()

# Create an object that will proxy for a particular remote object.
try:
	admin = bus.get_object("org.ganesha.nfsd",
                       "/org/ganesha/nfsd/ClientMgr")
except: # catch *all* exceptions
      print "Error: Can't talk to ganesha service on d-bus. Looks like Ganesha is down"
      sys.exit()

# call method
ganesha_delegstats_ops = admin.get_dbus_method('GetDelegations',
                               'org.ganesha.nfsd.clientstats')

total_ops=ganesha_delegstats_ops(str(sys.argv[1]))
if total_ops[1] != "OK":
	print "GANESHA RESPONSE STATUS: ", total_ops[1]
else:
	print "GANESHA RESPONSE STATUS: ", total_ops[1]
	print total_ops
	print "Timestamp: ", time.ctime(total_ops[2][0]), total_ops[2][1], " nsecs"
	print "Confirmed: ", total_ops[3][0]
	print "Current Delegations: ", total_ops[4][0]
	print "Current Recalls: ", total_ops[5][0]
	print "Current Failed Recalls: ", total_ops[6][0]
	print "Current Number of Revokes: ", total_ops[7][0]
sys.exit()

