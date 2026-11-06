// SPDX-License-Identifier: MIT
// GPU side of the oracle (the truth): a cubin whose whole .text is our own SASS, run with the CUDA driver API.
//
// The ELF shell comes from template.ptx (ptxas -arch=sm_86, one .u64 parameter = the register-file buffer). BuildKernel
// writes, per test, this program into .text.hw and pads the rest of the section with NOPs:
//
//   S2R R16, SR_TID.X                       ; thread index (one block of up to 1024 threads)
//   IMAD.MOV.U32 R17, RZ, RZ, 0x70          ; 28 words (112 bytes) per thread
//   ULDC.64 UR4, c[0x0][0x118]              ; global memory descriptor (LDG/STG .E use UR4)
//   IMAD.WIDE.U32 R18, R16, R17, c[0x0][0x160]
//   LDG.E.128 R0/R4/R8/R12, [R18.64+0x0/0x10/0x20/0x30]
//   LDG.E R20, [R18.64+0x40]                ; predicate word
//   R2P PR, R20, 0x7f
//   [memory test set-up, Mem.cs]            ; shared/local: M -> R20..R27 -> STS/STL.128 at R28; global: IMAD.WIDE Ra, Ra, 0x1, R18
//   <test instructions>                     ; stall 15, own scoreboards, reuse bits cleared
//   NOP                                     ; waits on every scoreboard
//   [memory test read-back]                 ; shared/local: LDS/LDL.128 -> STG.E.128 to M; global: zero the address pair; NOP
//   P2R R20, PR, RZ, 0x7f
//   STG.E.128 ... / STG.E [R18.64+0x40], R20
//   EXIT ; BRA self
//
// The memory-test words (LDS/STS/LDL/STL.128 on R28, IMAD.SHL/MOV/WIDE) are ptxas 13.3 encodings of the same instructions with
// register and immediate fields changed. template.ptx declares 32 KB of shared memory (at shared address 0) and a 2 KB local
// frame, so absolute local addresses below 2 KB are valid.
// The register count (section sh_info bits 24..31 and EIATTR_REGCOUNT in .nv.info) is set to 32 and the EXIT offset list
// (EIATTR_EXIT_INSTR_OFFSETS in .nv.info.hw) to our EXIT. The fixed words below are ptxas 13.3 output for template.ptx with
// register fields changed; hwcheck checks the assembled kernel text with nvdisasm before the first run (Program.VerifyKernel).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace Hwcheck
{
    static class Sass
    {
        /// Control bits (instruction bits 105..125, i.e. hi bits 41..61): stall, yield, write/read scoreboard (7 = none), wait mask, reuse.
        public static ulong Ctrl(ulong hi, int stall, int yield, int wbar, int rbar, int wait, int reuse = 0)
        {
            hi &= (1UL << 41) - 1;
            hi |= (ulong)(stall & 0xf) << 41 | (ulong)(yield & 1) << 45 | (ulong)(wbar & 7) << 46 | (ulong)(rbar & 7) << 49
                | (ulong)(wait & 0x3f) << 52 | (ulong)(reuse & 0xf) << 58;
            return hi;
        }

        // yield bit: nvdisasm refuses stall 15 with it set, and BRA with it set and stall 0 (ptxas: 'BRA self' uses stall 0, yield 0)
        static (ulong, ulong) W(ulong lo, ulong hi, int stall, int wbar = 7, int rbar = 7, int wait = 0) => (lo, Ctrl(hi, stall, stall is 0 or 15 ? 0 : 1, wbar, rbar, wait));

        public const int ScratchFirst = 16, Regs = 32;

        /// Opcodes on variable-latency units (the compiler gives them a write scoreboard): MUFU/conversions/bit counts, warp ops, FP64.
        static readonly System.Collections.Generic.HashSet<string> VariableLatency = new()
        {
            "MUFU", "I2F", "F2I", "F2F", "FRND", "POPC", "FLO", "BREV", "SHFL", "REDUX", "MATCH", "DADD", "DMUL", "DFMA", "DSETP",
        };

        /// The whole kernel text for `test` (each element lo, hi as found; control bits are rewritten here).
        public static List<(ulong lo, ulong hi)> Kernel(IReadOnlyList<(ulong lo, ulong hi)> test, MemSpec mem, out int exitOffset)
        {
            var k = new List<(ulong, ulong)>
            {
                W(0x0000000000107919, 0x0000000000002100, 2, wbar: 0),                 // S2R R16, SR_TID.X
                W(0x00000070ff117424, 0x00000000078e00ff, 2),                          // IMAD.MOV.U32 R17, RZ, RZ, 0x70
                W(0x0000460000047ab9, 0x0000000000000a00, 6),                          // ULDC.64 UR4, c[0x0][0x118]
                W(0x0000580010127625, 0x00000000078e0011, 15, wait: 1),               // IMAD.WIDE.U32 R18, R16, R17, c[0x0][0x160]
                W(0x0000000412007981, 0x000000000c1e1d00, 2, wbar: 2),                 // LDG.E.128 R0, [R18.64]
                W(0x0000100412047981, 0x000000000c1e1d00, 2, wbar: 2),                 // LDG.E.128 R4, [R18.64+0x10]
                W(0x0000200412087981, 0x000000000c1e1d00, 2, wbar: 2),                 // LDG.E.128 R8, [R18.64+0x20]
                W(0x00003004120c7981, 0x000000000c1e1d00, 2, wbar: 2),                 // LDG.E.128 R12, [R18.64+0x30]
                W(0x0000400412147981, 0x000000000c1e1900, 2, wbar: 2),                 // LDG.E R20, [R18.64+0x40]
                W(0x0000007f14007804, 0x0000000000000000, 15, wait: 1 << 2),          // R2P PR, R20, 0x7f
            };
            bool window = mem != null && mem.Kind is "shared" or "local";
            bool shared = mem?.Kind == "shared";
            if (window)
            {
                k.Add(W(0x0000500412147981, 0x000000000c1e1d00, 2, wbar: 2));             // LDG.E.128 R20, [R18.64+0x50]
                k.Add(W(0x0000600412187981, 0x000000000c1e1d00, 2, wbar: 2));             // LDG.E.128 R24, [R18.64+0x60]
                if (shared) k.Add(W(0x00000020101c7824, 0x00000000078e00ff, 15));         // IMAD.SHL.U32 R28, R16, 0x20, RZ
                else
                {
                    k.Add(W((ulong)(uint)mem.LocalBase << 32 | 0xff1c7424, 0x00000000078e00ff, 15)); // IMAD.MOV.U32 R28, RZ, RZ, W
                    k.AddRange(LocalBase(28, +1));
                }
                k.Add(W(shared ? 0x000000141c007388UL : 0x000000141c007387UL, shared ? 0xc00UL : 0x100c00UL, 2, rbar: 1, wait: 1 << 2)); // ST.128 [R28], R20
                k.Add(W(shared ? 0x000010181c007388UL : 0x000010181c007387UL, shared ? 0xc00UL : 0x100c00UL, 2, rbar: 1));              // ST.128 [R28+0x10], R24
            }
            if (mem?.Kind == "local") k.AddRange(LocalBase(mem.AddrReg, +1));
            else if (mem?.Kind == "global")
            {
                ulong a = (ulong)mem.AddrReg;
                k.Add(W(0x0000000100007825UL | a << 16 | a << 24, 0x00000000078e0212, 15)); // IMAD.WIDE Ra, Ra, 0x1, R18
            }
            int pending = window ? 1 << 1 : 0;
            foreach (var (lo, hi) in test)
            {
                int wbar = (int)(hi >> 46) & 7, rbar = (int)(hi >> 49) & 7;
                // variable-latency units always get a scoreboard: a corpus word can lack one when the compiler covered the wait
                // elsewhere (e.g. SHFL with wbar 7), and without it the stores below could read the old register
                string op = Nx.Shaders.Ampere.SassDecoder.Decode(lo, hi, 0).Opcode;
                if (VariableLatency.Contains(op)) wbar = 0;
                // memory instructions read their address and data registers late: without a read scoreboard the address fix-up
                // after the test can change them first (a corpus STG with rbar 7 then stored to address 0: ILLEGAL_ADDRESS)
                if (MemSpec.Opcodes.Contains(op)) { rbar = 0; if (op is not ("STS" or "STL" or "STG" or "RED")) wbar = 0; }
                // keep the instruction's own scoreboard use (variable-latency ops must set one), on scoreboards 3 and 4;
                // a later test instruction waits for both
                k.Add((lo, Ctrl(hi, 15, 0, wbar == 7 ? 7 : 3, rbar == 7 ? 7 : 4, pending)));
                pending = (1 << 3) | (1 << 4);
            }
            k.Add(W(0x0000000000007918, 0, 15, wait: 0x3f));                              // NOP, waits on all
            if (window)
            {
                k.Add(W(shared ? 0x000000001c147984UL : 0x000000001c147983UL, shared ? 0xc00UL : 0x100c00UL, 2, wbar: 2));   // LD.128 R20, [R28]
                k.Add(W(shared ? 0x000010001c187984UL : 0x000010001c187983UL, shared ? 0xc00UL : 0x100c00UL, 2, wbar: 2));   // LD.128 R24, [R28+0x10]
                k.Add(W(0x0000501412007986, 0x000000000c101d04, 2, rbar: 1, wait: 1 << 2)); // STG.E.128 [R18.64+0x50], R20
                k.Add(W(0x0000601812007986, 0x000000000c101d04, 2, rbar: 1));              // STG.E.128 [R18.64+0x60], R24
                k.Add(W(0x0000000000007918, 0, 15, wait: 0x3f));                          // NOP, waits on all (R20 is reused below)
            }
            if (mem?.Kind == "local") foreach (int z in mem.Zero) k.AddRange(LocalBase(z, -1));
            else if (mem?.Kind == "global")
                foreach (int z in mem.Zero) k.Add(W(0x00000000ff007424UL | (ulong)z << 16, 0x00000000078e00ff, 15)); // IMAD.MOV.U32 Rz, RZ, RZ, 0x0
            k.Add(W(0x0000007fff147803, 0, 15));                                          // P2R R20, PR, RZ, 0x7f
            k.Add(W(0x0000000012007986, 0x000000000c101d04, 2, rbar: 1));                  // STG.E.128 [R18.64], R0
            k.Add(W(0x0000100412007986, 0x000000000c101d04, 2, rbar: 1));                  // STG.E.128 [R18.64+0x10], R4
            k.Add(W(0x0000200812007986, 0x000000000c101d04, 2, rbar: 1));                  // STG.E.128 [R18.64+0x20], R8
            k.Add(W(0x0000300c12007986, 0x000000000c101d04, 2, rbar: 1));                  // STG.E.128 [R18.64+0x30], R12
            k.Add(W(0x0000401412007986, 0x000000000c101904, 2, rbar: 1));                  // STG.E [R18.64+0x40], R20
            exitOffset = k.Count * 16;
            k.Add(W(0x000000000000794d, 0x0000000003800000, 5));                          // EXIT
            k.Add(W(0xfffffff000007947, 0x000000000383ffff, 0));                          // BRA self
            return k;
        }

        /// Rr += (sign > 0) or -= the GPU side's local frame base, c[0x0][0x28] - 0x800: IADD3 Rr, Rr, [-]c[0x0][0x28], RZ and
        /// IADD3 Rr, Rr, -/+0x800, RZ.
        static IEnumerable<(ulong, ulong)> LocalBase(int reg, int sign)
        {
            ulong r = (ulong)reg << 16 | (ulong)reg << 24;
            yield return W((sign > 0 ? 0x00000a0000007a10UL : 0x80000a0000007a10UL) | r, 0x0000000007ffe0ff, 15);
            yield return W((sign > 0 ? 0xfffff80000007810UL : 0x0000080000007810UL) | r, 0x0000000007ffe0ff, 15);
        }

        public static readonly (ulong lo, ulong hi) Nop = (0x0000000000007918, 0x000fc00000000000);
    }

    /// The template cubin and its patch points.
    sealed class Cubin
    {
        readonly byte[] _elf;
        readonly int _textOff, _textSize, _textHdr;
        int _regAttr = -1, _exitAttr = -1, _exitCount;

        public Cubin(byte[] elf)
        {
            _elf = elf;
            long shoff = BitConverter.ToInt64(elf, 0x28);
            int shentsize = BitConverter.ToUInt16(elf, 0x3a), shnum = BitConverter.ToUInt16(elf, 0x3c), shstrndx = BitConverter.ToUInt16(elf, 0x3e);
            int strOff = (int)BitConverter.ToInt64(elf, (int)(shoff + shstrndx * shentsize + 24));
            for (int s = 0; s < shnum; s++)
            {
                int h = (int)(shoff + s * shentsize);
                int nameOff = BitConverter.ToInt32(elf, h);
                int end = Array.IndexOf(elf, (byte)0, strOff + nameOff);
                string name = Encoding.ASCII.GetString(elf, strOff + nameOff, end - strOff - nameOff);
                int off = (int)BitConverter.ToInt64(elf, h + 24), size = (int)BitConverter.ToInt64(elf, h + 32);
                if (name == ".text.hw") { _textOff = off; _textSize = size; _textHdr = h; }
                else if (name == ".nv.info" || name == ".nv.info.hw") ScanInfo(off, size);
            }
            if (_textSize == 0 || _regAttr < 0 || _exitAttr < 0) throw new InvalidDataException("template cubin: .text.hw / EIATTR_REGCOUNT / EIATTR_EXIT_INSTR_OFFSETS not found");

            void ScanInfo(int off, int size)
            {
                for (int p = off; p + 4 <= off + size;)
                {
                    int fmt = _elf[p], attr = _elf[p + 1];
                    if (fmt == 0x04)      // EIFMT_SVAL: u16 size + data
                    {
                        int n = BitConverter.ToUInt16(_elf, p + 2);
                        if (attr == 0x2f) _regAttr = p + 4;                         // EIATTR_REGCOUNT {u32 symbol, u32 count}
                        if (attr == 0x1c) { _exitAttr = p + 4; _exitCount = n / 4; } // EIATTR_EXIT_INSTR_OFFSETS
                        p += 4 + n;
                    }
                    else if (fmt == 0x03) p += 4;   // EIFMT_HVAL (u16 value)
                    else if (fmt == 0x01) p += 4;   // EIFMT_NVAL
                    else break;
                }
            }
        }

        public int Slots => _textSize / 16;

        public byte[] Build(IReadOnlyList<(ulong lo, ulong hi)> code, int exitOffset)
        {
            if (code.Count > Slots) throw new InvalidOperationException($"kernel needs {code.Count} slots, template has {Slots}");
            byte[] e = (byte[])_elf.Clone();
            for (int n = 0; n < Slots; n++)
            {
                var (lo, hi) = n < code.Count ? code[n] : Sass.Nop;
                BitConverter.TryWriteBytes(e.AsSpan(_textOff + n * 16), lo);
                BitConverter.TryWriteBytes(e.AsSpan(_textOff + n * 16 + 8), hi);
            }
            uint info = BitConverter.ToUInt32(e, _textHdr + 44);
            BitConverter.TryWriteBytes(e.AsSpan(_textHdr + 44), (info & 0x00ffffffu) | ((uint)Sass.Regs << 24));
            BitConverter.TryWriteBytes(e.AsSpan(_regAttr + 4), Sass.Regs);
            for (int n = 0; n < _exitCount; n++) BitConverter.TryWriteBytes(e.AsSpan(_exitAttr + n * 4), exitOffset);
            return e;
        }

        public static byte[] FromPtx(string ptx, string cacheDir)
        {
            Directory.CreateDirectory(cacheDir);
            string outPath = Path.Combine(cacheDir, "template.cubin");
            if (!File.Exists(outPath) || File.GetLastWriteTimeUtc(outPath) < File.GetLastWriteTimeUtc(ptx))
                Tools.Run(Tools.Cuda("ptxas"), $"-arch=sm_86 -o \"{outPath}\" \"{ptx}\"");
            return File.ReadAllBytes(outPath);
        }
    }

    /// Minimal CUDA driver API (nvcuda.dll), primary context of device 0.
    sealed unsafe class CudaRunner : IDisposable
    {
        const string Dll = "nvcuda.dll";
        [DllImport(Dll)] static extern int cuInit(uint flags);
        [DllImport(Dll)] static extern int cuDeviceGet(out int dev, int ordinal);
        [DllImport(Dll)] static extern int cuDeviceGetName(byte* name, int len, int dev);
        [DllImport(Dll)] static extern int cuDevicePrimaryCtxRetain(out IntPtr ctx, int dev);
        [DllImport(Dll)] static extern int cuDevicePrimaryCtxRelease_v2(int dev);
        [DllImport(Dll)] static extern int cuCtxSetCurrent(IntPtr ctx);
        [DllImport(Dll)] static extern int cuCtxSynchronize();
        [DllImport(Dll)] static extern int cuModuleLoadData(out IntPtr mod, byte* image);
        [DllImport(Dll)] static extern int cuModuleUnload(IntPtr mod);
        [DllImport(Dll)] static extern int cuModuleGetFunction(out IntPtr fn, IntPtr mod, string name);
        [DllImport(Dll)] static extern int cuMemAlloc_v2(out ulong ptr, ulong size);
        [DllImport(Dll)] static extern int cuMemFree_v2(ulong ptr);
        [DllImport(Dll)] static extern int cuMemcpyHtoD_v2(ulong dst, void* src, ulong size);
        [DllImport(Dll)] static extern int cuMemcpyDtoH_v2(void* dst, ulong src, ulong size);
        [DllImport(Dll)] static extern int cuLaunchKernel(IntPtr f, uint gx, uint gy, uint gz, uint bx, uint by, uint bz, uint shmem, IntPtr stream, void** args, void** extra);
        [DllImport(Dll)] static extern int cuGetErrorName(int err, out IntPtr str);

        int _dev;
        IntPtr _ctx;
        ulong _buf;
        readonly int _bytes;
        public string DeviceName { get; }

        public CudaRunner(int bytes)
        {
            _bytes = bytes;
            Check(cuInit(0), "cuInit");
            Check(cuDeviceGet(out _dev, 0), "cuDeviceGet");
            byte* name = stackalloc byte[256];
            cuDeviceGetName(name, 256, _dev);
            DeviceName = Marshal.PtrToStringAnsi((IntPtr)name);
            Open();
        }

        void Open()
        {
            Check(cuDevicePrimaryCtxRetain(out _ctx, _dev), "cuDevicePrimaryCtxRetain");
            Check(cuCtxSetCurrent(_ctx), "cuCtxSetCurrent");
            Check(cuMemAlloc_v2(out _buf, (ulong)_bytes), "cuMemAlloc");
        }

        static string Name(int e) { cuGetErrorName(e, out IntPtr s); return s == IntPtr.Zero ? e.ToString() : Marshal.PtrToStringAnsi(s); }
        static void Check(int e, string what) { if (e != 0) throw new Exception($"{what}: {Name(e)}"); }

        /// Runs one kernel image over `data` (in place). Returns null or an error text. A launch error is sticky (the context
        /// is unusable afterwards), which is why this runs in a worker process (Program.CudaWorker).
        public string Run(byte[] cubin, uint[] data, int threads)
        {
            IntPtr mod = IntPtr.Zero;
            try
            {
                int e;
                fixed (byte* p = cubin) e = cuModuleLoadData(out mod, p);
                if (e != 0) return "cuModuleLoadData: " + Name(e);
                Check(cuModuleGetFunction(out IntPtr fn, mod, "hw"), "cuModuleGetFunction");
                fixed (uint* d = data) Check(cuMemcpyHtoD_v2(_buf, d, (ulong)_bytes), "cuMemcpyHtoD");
                ulong arg = _buf;
                void* argp = &arg;
                e = cuLaunchKernel(fn, 1, 1, 1, (uint)threads, 1, 1, 0, IntPtr.Zero, &argp, null);
                if (e == 0) e = cuCtxSynchronize();
                if (e != 0) { mod = IntPtr.Zero; return "launch: " + Name(e); }
                fixed (uint* d = data) Check(cuMemcpyDtoH_v2(d, _buf, (ulong)_bytes), "cuMemcpyDtoH");
                return null;
            }
            finally
            {
                if (mod != IntPtr.Zero) cuModuleUnload(mod);
            }
        }

        public void Dispose()
        {
            if (_buf != 0) cuMemFree_v2(_buf);
            cuDevicePrimaryCtxRelease_v2(_dev);
        }
    }

    static class Tools
    {
        public static string CudaBin
        {
            get
            {
                string p = Environment.GetEnvironmentVariable("CUDA_PATH");
                if (!string.IsNullOrEmpty(p) && Directory.Exists(Path.Combine(p, "bin"))) return Path.Combine(p, "bin");
                return @"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\bin";
            }
        }

        public static string Cuda(string exe) => Path.Combine(CudaBin, exe + ".exe");

        public static string Run(string exe, string args, bool check = true)
        {
            var psi = new ProcessStartInfo(exe, args) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
            using var pr = Process.Start(psi);
            try { pr.PriorityClass = ProcessPriorityClass.BelowNormal; } catch { }
            var err = pr.StandardError.ReadToEndAsync();
            string o = pr.StandardOutput.ReadToEnd();
            pr.WaitForExit();
            if (check && pr.ExitCode != 0) throw new Exception($"{Path.GetFileName(exe)} {args} failed ({pr.ExitCode}): {err.Result}{o}");
            return o + err.Result;
        }
    }
}
