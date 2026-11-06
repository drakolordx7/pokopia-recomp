// SPDX-License-Identifier: MIT
// Memory tests: LDS/STS/ATOMS (shared), LDL/STL (local), LDG/STG/ATOMG/RED (global, through the register-file buffer).
//
// Every invocation owns a 32-byte memory window M (words 20..27 of its record, random at the start). The test instruction's
// address register is overwritten in the inputs so that the access lands inside M's copy:
//   shared  M is copied to shared byte index * 32 before the test and back after it. Each invocation only touches its own
//           32 bytes, so the result does not depend on the order of threads (and the GPU side's one 1024-thread block and the
//           Vulkan side's 32-invocation workgroups see the same thing).
//   local   M is copied to local byte W = 0x400 of the frame before and back after. The GPU side's frame starts at
//           c[0x0][0x28] - 0x800 (ptxas output for a 2 KB frame), so it adds that to the address register just before the test
//           and subtracts it again after (unless the instruction wrote it); the Vulkan side's local array starts at 0.
//   global  the address pair is M's byte offset in the record (minus the immediate); each side adds its own buffer base just
//           before the test (GPU: IMAD.WIDE Ra, Ra, 0x1, R18; Vulkan: AmpereHwcheck.cs) and zeroes the pair after it, unless the
//           instruction wrote it.
// Forms refused here: an absolute address (shared/global: every thread would hit one word; local: the GPU side's frame is not
// at local address 0), a uniform-register address part, a scaled local address, a global address that is not a register pair,
// and a global or local address register that is also a data source.
using Nx.Shaders.Ampere;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.RegularExpressions;

namespace Hwcheck
{
    sealed class MemSpec
    {
        public const int Word = 20, Words = 8, Bytes = Words * 4, LocalBytes = 2048;

        public string Kind;          // shared, local, global
        public int AddrReg = -1;     // RZ (255) for an absolute local address
        public int Scale;            // .X4/.X8/.X16 shift
        public int Size;             // access bytes
        public long Imm;
        public int LocalBase;        // local: byte address of the window
        public List<int> Zero = new();   // global: registers zeroed after the test; local: the address register, if not written
        public (int pred, bool neg)? ForceGuard;
        int[] _offsets;              // byte offsets in M the access may start at

        public static readonly HashSet<string> Opcodes = new() { "LDS", "STS", "ATOMS", "LDL", "STL", "LDG", "STG", "ATOMG", "RED", "LDSM" };

        public string Tsv => Kind switch
        {
            "local" => $"local:{LocalBase}",
            "global" => $"global:{AddrReg}:{string.Join(',', Zero)}",
            _ => Kind,
        };

        static int SizeOf(SassInst i)
        {
            var m = i.Mods;
            if (i.Opcode == "LDSM") return 16;   // one matrix row (8 x 16-bit); each lane gives one row address
            if (m.Contains("U8") || m.Contains("S8")) return 1;
            if (m.Contains("U16") || m.Contains("S16")) return 2;
            if (m.Contains("128") || m.Contains("U.128")) return 16;
            if (m.Contains("64")) return 8;
            return 4;
        }

        /// The memory test for `i`, or null with the reason.
        public static MemSpec Of(SassInst i, out string why)
        {
            why = null;
            string op = i.Opcode;
            Operand mem = i.Operands.FirstOrDefault(o => o.Kind == OperandKind.Mem);
            if (mem == null) { why = "no memory operand"; return null; }
            if (mem.IndexReg >= 0 && !(mem.IndexIsUReg && mem.IndexReg == 63)) { why = "uniform-register address part"; return null; }
            var s = new MemSpec
            {
                Kind = op is "LDS" or "STS" or "ATOMS" or "LDSM" ? "shared" : op is "LDL" or "STL" ? "local" : "global",
                AddrReg = mem.Reg, Imm = mem.Offset, Size = SizeOf(i),
            };
            string t = mem.Text ?? "";
            s.Scale = t.Contains(".X16") ? 4 : t.Contains(".X8") ? 3 : t.Contains(".X4") ? 2 : 0;
            if (s.Imm % s.Size != 0) { why = "unaligned immediate"; return null; }
            if (s.AddrReg == 255) { why = $"absolute {s.Kind} address"; return null; }
            if (s.Kind != "shared")
            {
                // the GPU side moves the address register by its own base (global: the buffer; local: the frame, which does not
                // start at local address 0 on the GPU side, see Cuda.cs), so the register must not also be data
                int span = s.Kind == "global" ? 2 : 1;
                if (s.Kind == "global")
                {
                    bool wide = (i.Mods.Contains("E") && !t.Contains(".U32")) || t.Contains(".64");
                    if (!wide || s.Scale != 0) { why = "global address is not a register pair"; return null; }
                }
                else if (s.Scale != 0) { why = "scaled local address"; return null; }
                var dst = new HashSet<int>();
                for (int k = 0; k < i.DstCount; k++)
                    if (i.Operands[k].Kind == OperandKind.Reg && i.Operands[k].Reg != 255)
                        for (int n = 0; n < Math.Max(1, s.Size / 4); n++) dst.Add(i.Operands[k].Reg + n);
                foreach (Operand o in i.Operands.Skip(i.DstCount))
                    if (o.Kind == OperandKind.Reg && o.Reg != 255 && o.Reg <= s.AddrReg + span - 1 && o.Reg + Math.Max(1, s.Size / 4) > s.AddrReg)
                    { why = "address register is also data"; return null; }
                for (int r = s.AddrReg; r < s.AddrReg + span; r++) if (!dst.Contains(r)) s.Zero.Add(r);
                // a guarded instruction that writes its own address register leaves the GPU side's pointer there in the lanes it
                // skips: those lanes get the guard set so the instruction runs everywhere
                if (s.Zero.Count < span && i.Pred != 7 && !i.PredIsUniform) s.ForceGuard = (i.Pred, i.PredNeg);
                if (s.Kind == "local") s.LocalBase = 0x400;
            }
            // offsets in M the access can start at: aligned to the size and, for a scaled register, (offset - imm) a multiple of
            // the scale (every window base is a multiple of 32)
            s._offsets = Enumerable.Range(0, Bytes / s.Size).Select(k => k * s.Size).Where(o => ((o - s.Imm) & ((1L << s.Scale) - 1)) == 0).ToArray();
            if (s._offsets.Length == 0) { why = "no aligned address for the scale"; return null; }
            return s;
        }

        /// Writes the address register of every invocation (inputs from Inputs.Make).
        public void Apply(uint[] d, int threads, int seed)
        {
            if (AddrReg == 255) return;
            var r = new Random(seed ^ 0x5bd1e995);
            for (int t = 0; t < threads; t++)
            {
                int b = t * Inputs.Words;
                long off = _offsets[r.Next(_offsets.Length)];
                long target = Kind switch { "shared" => t * Bytes + off, "local" => LocalBase + off, _ => Word * 4 + off };
                d[b + AddrReg] = (uint)((target - Imm) >> Scale);
                if (Kind == "global") d[b + AddrReg + 1] = 0;
                if (ForceGuard is (int p, bool neg)) d[b + Inputs.PredWord] = neg ? d[b + Inputs.PredWord] & ~(1u << p) : d[b + Inputs.PredWord] | 1u << p;
            }
        }
    }
}
