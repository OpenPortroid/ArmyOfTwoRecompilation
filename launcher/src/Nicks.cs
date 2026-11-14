using System;
using System.IO;
using System.Linq;
using System.Net;
using System.Security.Cryptography;
using System.Text;

namespace AO2Launcher
{
    // The player's name on the OpenPortroid server. A name belongs to the first player key that claims it
    // (no password: the key is the account); unused for 180 days it is free again. The launcher keeps the
    // key (op_key), passes it to the game, checks the name as it is typed and claims it on Play.
    static class Nicks
    {
        public const int MaxLen = 15;

        // The game's ao2_openportroid_server and the path of its lobby tunnel (aot/src/openportroid.h).
        public const string Host = "aot.bhop.pw";
        const string Path = "/aot1/";

        public static bool Allowed(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        }

        public static string Sanitize(string s)
        {
            string t = new string((s ?? "").Where(Allowed).ToArray());
            return t.Length > MaxLen ? t.Substring(0, MaxLen) : t;
        }

        public static bool ValidKey(string k)
        {
            return k != null && k.Length == 32 && k.All(Uri.IsHexDigit);
        }

        // This launcher's key (made on first use).
        public static string Key(Settings s)
        {
            string k = s["op_key"].Trim().ToLowerInvariant();
            if (!ValidKey(k))
            {
                var b = new byte[16];
                using (var rng = RandomNumberGenerator.Create())
                {
                    rng.GetBytes(b);
                }
                k = string.Concat(b.Select(x => x.ToString("x2")));
                s["op_key"] = k;
                s.Save();
            }
            return k;
        }

        // The name the game signs in with: the player name, or the computer's name made valid.
        public static string Name(Settings s)
        {
            string n = Sanitize(s["gamertag"].Trim());
            if (n.Length == 0)
            {
                n = Sanitize(Environment.MachineName);
            }
            return n.Length > 0 ? n : "Player";
        }

        // "free", "yours", "taken", "invalid"; null when the server does not answer.
        public static string Ask(string what, string name, string key)
        {
            try
            {
                var rq = (HttpWebRequest)WebRequest.Create("http://" + Host + Path + what + "?name=" +
                                                           Uri.EscapeDataString(name));
                rq.Proxy = null;
                rq.Timeout = 5000;
                rq.ReadWriteTimeout = 5000;
                rq.Headers["X-AO2-Key"] = key;
                using (var rs = (HttpWebResponse)rq.GetResponse())
                using (var rd = new StreamReader(rs.GetResponseStream(), Encoding.ASCII))
                {
                    string t = rd.ReadToEnd().Trim();
                    return t == "free" || t == "yours" || t == "taken" || t == "invalid" ? t : null;
                }
            }
            catch
            {
                return null;
            }
        }

        // The name's state for the settings page, checked in the background (once per name and key).
        static readonly object gate = new object();
        static string checkedFor, state;
        static bool busy;

        public static string State(string name, string key, Action changed)
        {
            string id = name + "\u0001" + key;
            lock (gate)
            {
                if (checkedFor == id)
                {
                    return busy ? "checking" : state;
                }
                checkedFor = id;
                busy = true;
                state = null;
            }
            var th = new System.Threading.Thread(() =>
            {
                string r = Ask("nick", name, key) ?? "error";
                lock (gate)
                {
                    if (checkedFor != id)
                    {
                        return;
                    }
                    busy = false;
                    state = r;
                }
                changed();
            });
            th.IsBackground = true;
            th.Start();
            return "checking";
        }

        // Forget the last check (after a claim or a new key).
        public static void Recheck()
        {
            lock (gate)
            {
                checkedFor = null;
            }
        }
    }
}
