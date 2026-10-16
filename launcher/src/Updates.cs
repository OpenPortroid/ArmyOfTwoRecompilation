using System;
using System.IO;
using System.Net;
using System.Text;
using System.Text.RegularExpressions;

namespace AO2Launcher
{
    // Update check: the latest release on GitHub against the version of this
    // package. Current is bumped with every release (the tag is "v" + Current).
    static class Updates
    {
        public const string Current = "1.1";
        public const string Repo = "OpenPortroid/ArmyOfTwoRecompilation";
        public static readonly string ReleasesPage = "https://github.com/" + Repo + "/releases/latest";

        // Debug: --latest=<tag> takes this as the latest release instead of asking GitHub.
        public static string Override;

        public sealed class Release
        {
            public string Version;  // "1.1" (the tag without the leading v)
            public string Url;      // the .zip asset, else the release page
        }

        // The latest release, or null when GitHub could not be reached. Blocks: call off the UI thread.
        public static Release Latest()
        {
            if (Override != null)
            {
                return new Release { Version = Override.TrimStart('v', 'V'), Url = ReleasesPage };
            }
            try
            {
                // .NET Framework 4.x does not offer TLS 1.2 by default, GitHub needs it.
                ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
                var req = (HttpWebRequest)WebRequest.Create("https://api.github.com/repos/" + Repo + "/releases/latest");
                req.UserAgent = "ArmyOfTwoLauncher/" + Current;
                req.Accept = "application/vnd.github+json";
                req.Timeout = 6000;
                req.ReadWriteTimeout = 6000;
                string json;
                using (var resp = (HttpWebResponse)req.GetResponse())
                using (var rd = new StreamReader(resp.GetResponseStream(), Encoding.UTF8))
                {
                    json = rd.ReadToEnd();
                }
                Match tag = Regex.Match(json, "\"tag_name\"\\s*:\\s*\"([^\"]+)\"");
                if (!tag.Success)
                {
                    return null;
                }
                Match zip = Regex.Match(json, "\"browser_download_url\"\\s*:\\s*\"([^\"]+\\.zip)\"");
                return new Release
                {
                    Version = tag.Groups[1].Value.TrimStart('v', 'V'),
                    Url = zip.Success ? zip.Groups[1].Value : "https://github.com/" + Repo + "/releases/tag/" + tag.Groups[1].Value,
                };
            }
            catch
            {
                return null;
            }
        }

        // Whether version a is newer than b ("1.10" > "1.9"); tags that are not numbers count as newer when they differ.
        public static bool Newer(string a, string b)
        {
            Version va, vb;
            if (System.Version.TryParse(Pad(a), out va) && System.Version.TryParse(Pad(b), out vb))
            {
                return va > vb;
            }
            return !string.Equals(a, b, StringComparison.OrdinalIgnoreCase);
        }

        static string Pad(string v)
        {
            return v.IndexOf('.') < 0 ? v + ".0" : v;
        }
    }
}
