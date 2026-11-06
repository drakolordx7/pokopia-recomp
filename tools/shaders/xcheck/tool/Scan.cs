// SPDX-License-Identifier: MIT
// Level 1 (every instruction of a corpus): opcode coverage and mnemonic, predicate fields, uniform-guard flag; plus exemplar words per form for level 2.
using System;
using System.Collections.Generic;
using System.Numerics;
using System.Threading;
using System.Threading.Tasks;
using Nx.Shaders.Ampere;

namespace Nx.Shaders.XCheck
{
    sealed class Ex
    {
        public ulong Lo, Hi;
        public string Hash = "";
        public int Addr;
        public int Corpus;
    }

    sealed class FormAcc
    {
        public int Op12;
        public string Opcode = "";
        public readonly List<Ex> Exemplars = new();
        public ulong CovLo, CovHi;
        public readonly SortedSet<(ulong h, ulong lo, ulong hi)> MinHash = new();
        public long Count;
    }

    sealed class L1Acc
    {
        public readonly long[] N = new long[3];
        public readonly long[] Blocks = new long[3];
        public int LastBlock = -1;
        public Ex First;
    }

    /// Everything a scan accumulates, merged over threads and corpora.
    sealed class ScanData
    {
        public readonly Dictionary<(int op12, string ours), L1Acc> L1 = new();
        public readonly Dictionary<(int op12, string kind), L1Acc> Pred = new();
        public readonly Dictionary<ulong, FormAcc> Forms = new();
        public readonly long[][] BitCount = { new long[4096 * 128], new long[4096 * 128], new long[4096 * 128] };
        public readonly long[] Instructions = new long[3];
        public readonly long[] Blocks = new long[3];
    }

    static class Scan
    {
        public const ulong HiMask = (1UL << 41) - 1;   // bits 64..104; 105..127 are the scheduling control bits
        const int MinHashKeep = 6;

        static ulong Mix(ulong lo, ulong hi)
        {
            ulong h = lo * 0x9E3779B97F4A7C15UL ^ (hi + 0x632BE59BD9B4E019UL) * 0xC2B2AE3D27D4EB4FUL;
            h ^= h >> 29; h *= 0xBF58476D1CE4E5B9UL; h ^= h >> 32;
            return h;
        }

        static ulong FormHash(SassInst i, int op12)
        {
            ulong h = (ulong)op12 * 0x100000001B3UL;
            h = (h ^ (uint)i.Opcode.GetHashCode()) * 0x100000001B3UL;
            foreach (var m in i.Mods) h = (h ^ (uint)m.GetHashCode()) * 0x100000001B3UL;
            h = (h ^ 0x55) * 0x100000001B3UL;
            foreach (var o in i.Operands)
            {
                h = (h ^ (ulong)((int)o.Kind | (o.Neg ? 0x100 : 0) | (o.Abs ? 0x200 : 0) | (o.Not ? 0x400 : 0))) * 0x100000001B3UL;
                if (o.Suffixes != null)
                    foreach (var s in o.Suffixes) if (s != ".reuse") h = (h ^ (uint)s.GetHashCode()) * 0x100000001B3UL;
            }
            return h;
        }

        sealed class TState
        {
            public readonly ScanData D = new();
            public int Blocks;
        }

        public static void Run(ScanData g, int corpus, string dir, int jobs)
        {
            var rows = Corpus.Rows(dir);
            int blockId = 0;
            var gl = new object();
            Parallel.ForEach(rows, new ParallelOptions { MaxDegreeOfParallelism = jobs },
                () => new TState(),
                (b, _, st) =>
                {
                    if (!Corpus.TryLoad(b, out var code, out int start, out int end)) return st;
                    int id = Interlocked.Increment(ref blockId);
                    st.D.Blocks[corpus]++;
                    var d = st.D;
                    var bits = d.BitCount[corpus];
                    int n = (end - start) / 16;
                    for (int k = 0; k < n; k++)
                    {
                        int o = start + k * 16;
                        ulong lo = BitConverter.ToUInt64(code, o), hi = BitConverter.ToUInt64(code, o + 8);
                        int op12 = (int)(lo & 0xfff);
                        var inst = SassDecoder.Decode(lo, hi, k * 16);
                        d.Instructions[corpus]++;

                        ulong m = hi & HiMask;
                        for (ulong x = lo; x != 0; x &= x - 1) bits[op12 * 128 + BitOperations.TrailingZeroCount(x)]++;
                        for (ulong x = hi; x != 0; x &= x - 1) bits[op12 * 128 + 64 + BitOperations.TrailingZeroCount(x)]++;

                        bool oursUnk = inst.Opcode.StartsWith(".UNKNOWN", StringComparison.Ordinal);
                        bool nxOk = Nx.Emitter[op12] != null;
                        string name = oursUnk ? ".UNKNOWN" : inst.Opcode;
                        var key = (op12, name);
                        if (!d.L1.TryGetValue(key, out var acc)) d.L1[key] = acc = new L1Acc();
                        acc.N[corpus]++;
                        if (acc.LastBlock != id) { acc.LastBlock = id; acc.Blocks[corpus]++; }
                        if (acc.First == null || string.CompareOrdinal(b.Hash, acc.First.Hash) < 0)
                            acc.First = new Ex { Lo = lo, Hi = hi, Hash = b.Hash, Addr = k * 16, Corpus = corpus };

                        if (oursUnk || !nxOk) continue;

                        // predicate: guard register, negate, uniform datapath
                        int wantPred = (int)((lo >> 12) & 7);
                        bool wantNeg = ((lo >> 15) & 1) != 0;
                        if (inst.Pred != wantPred) AddPred(d, op12, "guard-reg", corpus, id, b.Hash, k, lo, hi);
                        if (inst.PredNeg != wantNeg) AddPred(d, op12, "guard-neg", corpus, id, b.Hash, k, lo, hi);
                        if (inst.PredIsUniform != Nx.Uniform[op12]) AddPred(d, op12, "guard-uniform", corpus, id, b.Hash, k, lo, hi);

                        // exemplars for level 2
                        ulong fh = FormHash(inst, op12);
                        if (!d.Forms.TryGetValue(fh, out var f))
                            d.Forms[fh] = f = new FormAcc { Op12 = op12, Opcode = inst.Opcode };
                        f.Count++;
                        ulong ml = lo, mh = hi & HiMask;
                        bool cover = ((ml & ~f.CovLo) | (mh & ~f.CovHi)) != 0;
                        ulong h = Mix(ml, mh);
                        bool minh = f.MinHash.Count < MinHashKeep || h < f.MinHash.Max.h;
                        if (cover || minh)
                        {
                            if (minh && f.MinHash.Add((h, ml, mh)) && f.MinHash.Count > MinHashKeep) f.MinHash.Remove(f.MinHash.Max);
                            if (cover) { f.CovLo |= ml; f.CovHi |= mh; }
                            f.Exemplars.Add(new Ex { Lo = lo, Hi = hi, Hash = b.Hash, Addr = k * 16, Corpus = corpus });
                        }
                    }
                    return st;
                },
                st => { lock (gl) Merge(g, st.D); });
        }

        static void AddPred(ScanData d, int op12, string kind, int corpus, int id, string hash, int k, ulong lo, ulong hi)
        {
            if (!d.Pred.TryGetValue((op12, kind), out var a)) d.Pred[(op12, kind)] = a = new L1Acc();
            a.N[corpus]++;
            if (a.LastBlock != id) { a.LastBlock = id; a.Blocks[corpus]++; }
            a.First ??= new Ex { Lo = lo, Hi = hi, Hash = hash, Addr = k * 16, Corpus = corpus };
        }

        static void MergeAcc(Dictionary<(int, string), L1Acc> dst, Dictionary<(int, string), L1Acc> src)
        {
            foreach (var kv in src)
            {
                if (!dst.TryGetValue(kv.Key, out var a)) dst[kv.Key] = a = new L1Acc();
                for (int c = 0; c < 3; c++) { a.N[c] += kv.Value.N[c]; a.Blocks[c] += kv.Value.Blocks[c]; }
                var f = kv.Value.First;
                if (f != null && (a.First == null || string.CompareOrdinal(f.Hash, a.First.Hash) < 0)) a.First = f;
            }
        }

        static void Merge(ScanData g, ScanData s)
        {
            MergeAcc(g.L1, s.L1);
            MergeAcc(g.Pred, s.Pred);
            for (int c = 0; c < 3; c++)
            {
                g.Instructions[c] += s.Instructions[c];
                g.Blocks[c] += s.Blocks[c];
                var a = g.BitCount[c]; var b = s.BitCount[c];
                for (int i = 0; i < a.Length; i++) a[i] += b[i];
            }
            foreach (var kv in s.Forms)
            {
                if (!g.Forms.TryGetValue(kv.Key, out var f)) { g.Forms[kv.Key] = kv.Value; continue; }
                f.Count += kv.Value.Count;
                var seen = new HashSet<(ulong, ulong)>();
                foreach (var e in f.Exemplars) seen.Add((e.Lo, e.Hi & HiMask));
                foreach (var e in kv.Value.Exemplars) if (seen.Add((e.Lo, e.Hi & HiMask))) f.Exemplars.Add(e);
            }
        }
    }
}
