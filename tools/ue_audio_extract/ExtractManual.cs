using System;
using System.IO;
using UELib;
using UELib.Core;

/// <summary>
/// Inspects a (decompressed) Army of Two package and lists every
/// USoundNodeWave export with the raw BulkData layout pointers.
/// It reads the bulk bytes directly from the file instead of calling
/// UBulkData.LoadData: UELib's load path rewinds the stream based on its
/// table offsets in ways that produce stale/duplicated buffers in this
/// build, while StorageOffset/StorageSize in the export deserialization
/// are exact.
/// </summary>
class ExtractManual {
    static void Main(string[] args) {
        // args: <assembled.pkg> <outDir> <outCsv>
        string outDir = args[1];
        Directory.CreateDirectory(outDir);
        string pkgBase = Path.GetFileNameWithoutExtension(args[0]);
        byte[] fileBytes = File.ReadAllBytes(args[0]);
        using (var pkg = UnrealLoader.LoadFullPackage(args[0], FileAccess.Read)) {
            var t = typeof(USoundNodeWave);
            var fX360 = t.GetField("CompressedXbox360Data");
            using (var csv = new StreamWriter(args[2])) {
                csv.WriteLine("wave,ElementCount,StorageOffset,StorageSize,bulk_file");
                foreach (var exp in pkg.Exports) {
                    if (exp.Class == null || exp.Class.ObjectName.ToString() != "SoundNodeWave") continue;
                    var wave = pkg.IndexToObject<USoundNodeWave>(exp.Index + 1);
                    if (wave == null) continue;
                    try { wave.Load(); } catch { }
                    var bd = (UBulkData<byte>)fX360.GetValue(wave);
                    long off = bd.StorageOffset, sz = bd.StorageSize;
                    if (off <= 0 || sz <= 0 || off + sz > fileBytes.Length) continue;
                    string nm = wave.Name.ToString().Replace('/', '_') + "__" + pkgBase + ".bulk";
                    byte[] outBuf = new byte[sz];
                    Array.Copy(fileBytes, off, outBuf, 0, sz);
                    File.WriteAllBytes(Path.Combine(outDir, nm), outBuf);
                    csv.WriteLine(wave.Name + "," + bd.ElementCount + "," + off + "," + sz + "," + nm);
                    Console.WriteLine("WAVE " + wave.Name + " elems=" + bd.ElementCount + " off=" + off + " size=" + sz);
                }
            }
        }
    }
}
