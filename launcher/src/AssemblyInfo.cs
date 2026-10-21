using System.Reflection;
using System.Runtime.InteropServices;

// Metadata baked into the exe's version resource. A launcher with a real
// company/product/version looks far less suspicious to antivirus heuristics
// than a nameless managed binary, which is the main reason fresh builds get
// flagged. Keep the version in step with Updates.Version.
[assembly: AssemblyTitle("Army of Two Recomp Launcher")]
[assembly: AssemblyProduct("Army of Two Launcher")]
[assembly: AssemblyDescription("Army of Two Recomp Launcher")]
[assembly: AssemblyCompany("OpenPortroid")]
[assembly: AssemblyCopyright("OpenPortroid")]
[assembly: AssemblyVersion("1.3.0.0")]
[assembly: AssemblyFileVersion("1.3.0.0")]

[assembly: ComVisible(false)]
