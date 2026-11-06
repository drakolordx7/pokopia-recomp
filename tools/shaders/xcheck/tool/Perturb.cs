// SPDX-License-Identifier: MIT
// Level 2: which instruction bits does each front end depend on? For every exemplar word of every form, flip each bit 16..127 in turn and ask both
// front ends whether the result changed (ours: the nvdisasm-style text; nxnative: the Ryujinx IR its emitter produces). Bits 0..15 are the opcode and
// the guard (level 1). A bit one side depends on and the other ignores is a disagreement.
using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Nx.Shaders.Ampere;

namespace Nx.Shaders.XCheck
{
    sealed class Case
    {
        public Ex E = new();
        public string OursBase = "", OursFlip = "", NxBase = "", NxFlip = "";
    }

    sealed class Cell
    {
        public long Exemplars, OursSens, NxSens, OursOnly, NxOnly;
        public readonly List<Case> OursOnlyEx = new(), NxOnlyEx = new();
    }

    static class Perturb
    {
        public static string OursSig(ulong lo, ulong hi)
        {
            var i = SassDecoder.Decode(lo, hi, 0);
            return i.ToSassText().Replace(".reuse", "") + "|" + i.DstCount;
        }

        public static List<(FormAcc f, Ex e)> Exemplars(ScanData d)
        {
            var exs = new List<(FormAcc f, Ex e)>();
            foreach (var f in d.Forms.Values)
            {
                var seen = new HashSet<(ulong, ulong)>();
                foreach (var e in f.Exemplars) if (seen.Add((e.Lo, e.Hi & Scan.HiMask))) exs.Add((f, e));
            }
            return exs;
        }

        public static Dictionary<(int op12, int bit), Cell> Run(List<(FormAcc f, Ex e)> exs, int jobs, int keepExamples = 3)
        {
            var global = new Dictionary<(int, int), Cell>();
            var gl = new object();
            Parallel.ForEach(exs, new ParallelOptions { MaxDegreeOfParallelism = jobs },
                () => new Dictionary<(int, int), Cell>(),
                (x, _, local) =>
                {
                    var (f, e) = x;
                    string ob = OursSig(e.Lo, e.Hi), nb = Nx.Signature(e.Lo, e.Hi);
                    for (int bit = 16; bit < 128; bit++)
                    {
                        ulong lo = e.Lo, hi = e.Hi;
                        if (bit < 64) lo ^= 1UL << bit; else hi ^= 1UL << (bit - 64);
                        string of = OursSig(lo, hi), nf = Nx.Signature(lo, hi);
                        bool os = of != ob, ns = nf != nb;
                        var key = (f.Op12, bit);
                        if (!local.TryGetValue(key, out var c)) local[key] = c = new Cell();
                        c.Exemplars++;
                        if (os) c.OursSens++;
                        if (ns) c.NxSens++;
                        if (os != ns)
                        {
                            var cs = new Case { E = e, OursBase = ob, OursFlip = of, NxBase = nb, NxFlip = nf };
                            if (os) { c.OursOnly++; if (c.OursOnlyEx.Count < keepExamples) c.OursOnlyEx.Add(cs); }
                            else { c.NxOnly++; if (c.NxOnlyEx.Count < keepExamples) c.NxOnlyEx.Add(cs); }
                        }
                    }
                    return local;
                },
                local =>
                {
                    lock (gl)
                        foreach (var kv in local)
                        {
                            if (!global.TryGetValue(kv.Key, out var c)) { global[kv.Key] = kv.Value; continue; }
                            c.Exemplars += kv.Value.Exemplars; c.OursSens += kv.Value.OursSens; c.NxSens += kv.Value.NxSens;
                            c.OursOnly += kv.Value.OursOnly; c.NxOnly += kv.Value.NxOnly;
                            foreach (var cs in kv.Value.OursOnlyEx) if (c.OursOnlyEx.Count < keepExamples) c.OursOnlyEx.Add(cs);
                            foreach (var cs in kv.Value.NxOnlyEx) if (c.NxOnlyEx.Count < keepExamples) c.NxOnlyEx.Add(cs);
                        }
                });
            Console.WriteLine($"level 2: {exs.Count} exemplars, {global.Count} (opcode, bit) cells");
            return global;
        }
    }
}
