using System;
using System.IO;
using System.Text;

namespace AO2Launcher
{
    // The launcher's log: what the installation (and the other file work) did and
    // why it failed, for a bug report. <launcher folder>\logs\launcher.log, or
    // %TEMP%\ArmyOfTwo\launcher.log when that folder cannot be written. Every line
    // has a time and the thread; a new run starts with a header (version, system,
    // folders). The file is rotated to launcher.old.log past 1 MB. Logging never
    // throws.
    static class Log
    {
        const long MaxBytes = 1L << 20;
        static readonly object sync = new object();
        static string file;
        static bool tried;

        // Where the log is (null if neither folder can be written).
        public static string FilePath
        {
            get
            {
                lock (sync)
                {
                    Open();
                    return file;
                }
            }
        }

        static void Open()
        {
            if (tried)
            {
                return;
            }
            tried = true;
            string[] dirs =
            {
                Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "logs"),
                Path.Combine(Path.GetTempPath(), "ArmyOfTwo"),
            };
            foreach (string dir in dirs)
            {
                try
                {
                    Directory.CreateDirectory(dir);
                    string path = Path.Combine(dir, "launcher.log");
                    if (File.Exists(path) && new FileInfo(path).Length > MaxBytes)
                    {
                        string old = Path.Combine(dir, "launcher.old.log");
                        if (File.Exists(old))
                        {
                            File.Delete(old);
                        }
                        File.Move(path, old);
                    }
                    File.AppendAllText(path, Header(), Encoding.UTF8);
                    file = path;
                    return;
                }
                catch
                {
                }
            }
        }

        static string Header()
        {
            var sb = new StringBuilder();
            sb.AppendLine();
            sb.AppendLine("==== " + Stamp() + " launcher " + Updates.Current + " ====");
            sb.AppendLine("os: " + Environment.OSVersion + (Environment.Is64BitOperatingSystem ? " x64" : " x86") +
                          ", .NET " + Environment.Version + ", " + (Environment.Is64BitProcess ? "64" : "32") + "-bit process");
            sb.AppendLine("culture: " + System.Globalization.CultureInfo.CurrentUICulture.Name);
            sb.AppendLine("launcher: " + System.Reflection.Assembly.GetExecutingAssembly().Location);
            sb.AppendLine("args: " + Environment.CommandLine);
            return sb.ToString();
        }

        static string Stamp()
        {
            return DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff");
        }

        public static void Write(string message)
        {
            try
            {
                lock (sync)
                {
                    Open();
                    if (file != null)
                    {
                        File.AppendAllText(file, Stamp() + " [" + System.Threading.Thread.CurrentThread.ManagedThreadId + "] " +
                                                 message + "\r\n", Encoding.UTF8);
                    }
                }
            }
            catch
            {
            }
        }

        public static void Write(string format, params object[] args)
        {
            string text;
            try
            {
                text = string.Format(format, args);
            }
            catch (FormatException)
            {
                text = format;
            }
            Write(text);
        }

        // The whole exception: type, message, stack trace, inner exceptions, HResult.
        public static void Error(string what, Exception ex)
        {
            Write("ERROR " + what + ": " + Describe(ex));
        }

        static string Describe(Exception ex)
        {
            var sb = new StringBuilder();
            for (Exception e = ex; e != null; e = e.InnerException)
            {
                sb.AppendLine();
                sb.Append("    " + e.GetType().FullName + ": " + e.Message + " (HResult 0x" + e.HResult.ToString("X8") + ")");
                if (e.StackTrace != null)
                {
                    sb.AppendLine();
                    sb.Append(e.StackTrace);
                }
            }
            return sb.ToString();
        }
    }
}
