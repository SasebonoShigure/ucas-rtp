#!/usr/bin/python

from mininet.net import Mininet
from mininet.node import OVSBridge
from mininet.cli import CLI
from mininet.topo import Topo
from mininet.nodelib import NAT

class DNSTopo(Topo):
   def build(self, **_opts):
        s1 = self.addSwitch('s1')

        h1 = self.addHost('h1', ip='10.0.0.1/24', defaultRoute=None)
        h2 = self.addHost('h2', ip='10.0.0.2/24', defaultRoute=None)

        gateway_ip = ''
        nat0 = self.addNode('nat0', cls=NAT, ip='10.0.0.254/24', inNamespace=False)

        self.addLink(s1, h1)
        self.addLink(s1, h2)
        self.addLink(s1, nat0)


def dns_topology():
    net = Mininet(topo=DNSTopo(), switch=OVSBridge, controller=None)

    net.start()

    h1 = net.get('h1')
    h2 = net.get('h2')

    h1.cmd('ip route add default via 10.0.0.254')
    h2.cmd('ip route add default via 10.0.0.254')

    CLI(net)

    net.stop()

if __name__ == '__main__':
    dns_topology()
