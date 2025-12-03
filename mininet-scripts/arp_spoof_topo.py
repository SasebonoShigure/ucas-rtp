#!/usr/bin/python

from mininet.net import Mininet
from mininet.node import OVSBridge
from mininet.cli import CLI

def arp_spoof_topology():
    net = Mininet(switch=OVSBridge, controller=None)

    h1 = net.addHost('h1', ip='10.0.0.1/24')
    h2 = net.addHost('h2', ip='10.0.0.2/24')
    h3 = net.addHost('h3', ip='10.0.0.3/24')
    s1 = net.addSwitch('s1')

    net.addLink(h1, s1)
    net.addLink(h2, s1)
    net.addLink(h3, s1)

    for idx, hostname in enumerate(('h1', 'h2', 'h3')):
        host = net.get(hostname)
        host.setMAC('00:00:00:00:00:0%d' % (idx+1), '%s-eth0' % (hostname))

    net.start()

    CLI(net)

    net.stop()

if __name__ == '__main__':
    arp_spoof_topology()
