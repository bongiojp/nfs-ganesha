#!/usr/bin/python

# You must initialize the gobject/dbus support for threading
# before doing anything.
import gobject
gobject.threads_init()

from dbus import glib
glib.init_threads()

# Create a session bus.
import dbus
bus = dbus.SystemBus()

def notifications(bus, message):
    if message.get_member() == "heartbeat":
        print [arg for arg in message.get_args_list()]

bus.add_match_string_non_blocking("interface='org.ganesha.nfsd.heartbeat'")
bus.add_message_filter(notifications)

loop = gobject.MainLoop()
loop.run()
