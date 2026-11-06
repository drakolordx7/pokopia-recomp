// SPDX-License-Identifier: MIT
// --ops 804,803,... | all: re-check chosen 12-bit opcodes (all: every opcode, 250 words each) against nvdisasm without the full run. Reads every block of the three corpora (no decoding), keeps per
// opcode up to 1500 words with the smallest hash plus a greedy cover of the bits that are ever set, and compares our text with nvdisasm's.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Nx.Shaders.Ampere;

namespace Nx.Shaders.XCheck
{
    static class OpsCheck
    {
        sealed class Keep
        {
            public readonly SortedSet<(ulong h, ulong lo, ulong hi)> Min = new();   // hi here is the full word (scheduling bits included: nvdisasm rejects some words without them)
            public readonly List<(ulong lo, ulong hi)> Cover = new();
            public ulong CovLo, CovHi;
        }

        public static int Run(string[] dirs, string[] names, string opsArg, int jobs)
        {
            bool all = opsArg == "all";
            var ops = all ? new HashSet<int>() : opsArg.Split(',').Select(x => Convert.ToInt32(x, 16)).ToHashSet();
            int cap = all ? 250 : 1500;
            var global = new Dictionary<int, Keep>();
            var gl = new object();
            foreach (int c in Enumerable.Range(0, dirs.Length))
            {
                if (!Directory.Exists(dirs[c])) continue;
                Parallel.ForEach(Corpus.Rows(dirs[c]), new ParallelOptions { MaxDegreeOfParallelism = jobs },
                    () => new Dictionary<int, Keep>(),
                    (b, _, loc) =>
                    {
                        if (!Corpus.TryLoad(b, out var code, out int start, out int end)) return loc;
                        for (int o = start; o + 16 <= end; o += 16)
                        {
                            ulong lo = BitConverter.ToUInt64(code, o);
                            int op = (int)(lo & 0xfff);
                            if (!all && !ops.Contains(op)) continue;
                            ulong hiFull = BitConverter.ToUInt64(code, o + 8), hi = hiFull & Scan.HiMask;
                            if (!loc.TryGetValue(op, out var k)) loc[op] = k = new Keep();
                            ulong h = lo * 0x9E3779B97F4A7C15UL ^ (hi + 0x632BE59BD9B4E019UL) * 0xC2B2AE3D27D4EB4FUL;
                            h ^= h >> 29; h *= 0xBF58476D1CE4E5B9UL; h ^= h >> 32;
                            if (k.Min.Count < cap || h < k.Min.Max.h) { k.Min.Add((h, lo, hiFull)); if (k.Min.Count > cap) k.Min.Remove(k.Min.Max); }
                            if (((lo & ~k.CovLo) | (hi & ~k.CovHi)) != 0) { k.Cover.Add((lo, hiFull)); k.CovLo |= lo; k.CovHi |= hi; }
                        }
                        return loc;
                    },
                    loc =>
                    {
                        lock (gl)
                            foreach (var kv in loc)
                            {
                                if (!global.TryGetValue(kv.Key, out var g)) global[kv.Key] = g = new Keep();
                                foreach (var m in kv.Value.Min) { g.Min.Add(m); if (g.Min.Count > cap) g.Min.Remove(g.Min.Max); }
                                foreach (var cv in kv.Value.Cover) if (((cv.lo & ~g.CovLo) | (cv.hi & Scan.HiMask & ~g.CovHi)) != 0) { g.Cover.Add(cv); g.CovLo |= cv.lo; g.CovHi |= cv.hi & Scan.HiMask; }
                            }
                    });
                Console.WriteLine($"{names[c]} scanned");
            }
            var words = new List<(ulong lo, ulong hi)>();
            foreach (var kv in global.OrderBy(k => k.Key))
            {
                var set = new HashSet<(ulong, ulong)>(kv.Value.Min.Select(m => (m.lo, m.hi)));
                foreach (var cv in kv.Value.Cover) set.Add(cv);
                words.AddRange(set);
            }
            string tmp = Path.Combine(Path.GetTempPath(), "xcheck_ops.txt");
            File.WriteAllLines(tmp, words.Select(w => $"0x{w.lo:x16} 0x{w.hi:x16}"));
            Console.WriteLine($"{words.Count} words for {global.Count} opcodes");
            return Report.CheckWords(tmp, jobs);
        }
    }
}
