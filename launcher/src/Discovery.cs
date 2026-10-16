using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;

namespace AO2Launcher
{
    // Looks for a co-op host on the local network: UDP query to port 18490,
    // a host answers with the same nonce. Packet: four big-endian uint32 -
    // magic "AO2D", version 1, type (1 query, 2 reply), nonce.
    static class Discovery
    {
        const int Port = 18490;
        const uint Magic = 0x414F3244;
        const uint Version = 1;

        static void Put(byte[] b, int off, uint v)
        {
            b[off] = (byte)(v >> 24);
            b[off + 1] = (byte)(v >> 16);
            b[off + 2] = (byte)(v >> 8);
            b[off + 3] = (byte)v;
        }

        static uint Get(byte[] b, int off)
        {
            return ((uint)b[off] << 24) | ((uint)b[off + 1] << 16) | ((uint)b[off + 2] << 8) | b[off + 3];
        }

        static void SendTo(Socket s, byte[] pkt, IPAddress to)
        {
            try
            {
                s.SendTo(pkt, new IPEndPoint(to, Port));
            }
            catch
            {
                // This adapter can't reach that address: fine, others may.
            }
        }

        // Returns the IP of the first host that answered, or null.
        public static string FindHost(string manualIp, int timeoutMs)
        {
            var sockets = new List<Socket>();
            var targets = new List<KeyValuePair<Socket, IPAddress[]>>();
            try
            {
                var rnd = new Random(Guid.NewGuid().GetHashCode());
                uint nonce = (uint)rnd.Next(1, int.MaxValue) ^ ((uint)rnd.Next() << 1);
                var pkt = new byte[16];
                Put(pkt, 0, Magic);
                Put(pkt, 4, Version);
                Put(pkt, 8, 1);
                Put(pkt, 12, nonce);

                IPAddress manual;
                if (manualIp == null || !IPAddress.TryParse(manualIp.Trim(), out manual) ||
                    manual.AddressFamily != AddressFamily.InterNetwork)
                {
                    manual = null;
                }

                foreach (var sn in Game.LocalSubnets())
                {
                    try
                    {
                        var s = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
                        sockets.Add(s);
                        s.EnableBroadcast = true;
                        s.Bind(new IPEndPoint(sn.Key, 0));
                        byte[] ip = sn.Key.GetAddressBytes();
                        byte[] mask = sn.Value == null ? new byte[] { 255, 255, 255, 0 } : sn.Value.GetAddressBytes();
                        var bc = new byte[4];
                        for (int i = 0; i < 4; i++)
                        {
                            bc[i] = (byte)(ip[i] | ~mask[i]);
                        }
                        var list = new List<IPAddress> { new IPAddress(bc), IPAddress.Broadcast };
                        if (manual != null)
                        {
                            list.Add(manual);
                        }
                        targets.Add(new KeyValuePair<Socket, IPAddress[]>(s, list.ToArray()));
                    }
                    catch
                    {
                    }
                }
                if (manual != null)
                {
                    // Reachable even when no local adapter matches (VPN etc.).
                    try
                    {
                        var s = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
                        sockets.Add(s);
                        s.Bind(new IPEndPoint(IPAddress.Any, 0));
                        targets.Add(new KeyValuePair<Socket, IPAddress[]>(s, new[] { manual }));
                    }
                    catch
                    {
                    }
                }
                if (sockets.Count == 0)
                {
                    return null;
                }

                DateTime start = DateTime.UtcNow;
                int sent = 0;
                int[] resendAt = { 0, 250, 500 };
                var buf = new byte[64];
                while (true)
                {
                    int elapsed = (int)(DateTime.UtcNow - start).TotalMilliseconds;
                    if (elapsed >= timeoutMs)
                    {
                        return null;
                    }
                    while (sent < resendAt.Length && elapsed >= resendAt[sent])
                    {
                        foreach (var t in targets)
                        {
                            foreach (IPAddress to in t.Value)
                            {
                                SendTo(t.Key, pkt, to);
                            }
                        }
                        sent++;
                    }
                    var readable = new List<Socket>(sockets);
                    try
                    {
                        Socket.Select(readable, null, null, 20 * 1000);
                    }
                    catch
                    {
                        return null;
                    }
                    foreach (Socket s in readable)
                    {
                        try
                        {
                            while (s.Available > 0)
                            {
                                EndPoint from = new IPEndPoint(IPAddress.Any, 0);
                                int n = s.ReceiveFrom(buf, ref from);
                                if (n == 16 && Get(buf, 0) == Magic && Get(buf, 4) == Version &&
                                    Get(buf, 8) == 2 && Get(buf, 12) == nonce)
                                {
                                    return ((IPEndPoint)from).Address.ToString();
                                }
                            }
                        }
                        catch
                        {
                            // e.g. an ICMP port-unreachable surfacing as a reset: ignore.
                        }
                    }
                }
            }
            finally
            {
                foreach (Socket s in sockets)
                {
                    try
                    {
                        s.Close();
                    }
                    catch
                    {
                    }
                }
            }
        }
    }
}
