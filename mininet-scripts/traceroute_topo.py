#!/usr/bin/python

from mininet.net import Mininet
from mininet.node import Node
from mininet.cli import CLI

class LinuxRouter(Node):
    def config(self, **params):
        super(LinuxRouter, self).config(**params)
        self.cmd('sysctl net.ipv4.ip_forward=1')

    def terminate(self):
        self.cmd('sysctl net.ipv4.ip_forward=0')
        super(LinuxRouter, self).terminate()

def traceroute_topology():
    net = Mininet(controller=None)
    h1 = net.addHost('h1', ip='10.0.1.2/24', defaultRoute='via 10.0.1.1')
    h2 = net.addHost('h2', ip='10.0.3.2/24', defaultRoute='via 10.0.3.1')
    r1 = net.addHost('r1', cls=LinuxRouter, ip='10.0.1.1/24')
    r2 = net.addHost('r2', cls=LinuxRouter, ip='10.0.2.2/24')

    net.addLink(h1, r1, intfName1='h1-eth0', intfName2='r1-eth0')
    net.addLink(r1, r2, intfName1='r1-eth1', params1={'ip': '10.0.2.1/24'},
                         intfName2='r2-eth0', params2={'ip': '10.0.2.2/24'})
    net.addLink(r2, h2, intfName1='r2-eth1', params1={'ip': '10.0.3.1/24'},
                         intfName2='h2-eth0')

    net.start()

    r1.cmd('ip route add 10.0.3.0/24 via 10.0.2.2 dev r1-eth1')
    r2.cmd('ip route add 10.0.1.0/24 via 10.0.2.1 dev r2-eth0')

    CLI(net)

    net.stop()

if __name__ == '__main__':
    traceroute_topology()
