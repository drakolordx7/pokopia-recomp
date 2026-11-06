// SPDX-License-Identifier: MIT
// NVIDIA's nvdisasm (CUDA 13.3, -b SM86) as the text authority: disassemble a list of 128-bit words, bisecting on rejection so that one illegal word
// does not hide the rest. Word i sits at address i*16 (--base-address), so branch targets print the same for a word and its flipped twin.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace Nx.Shaders.XCheck
{
    static class Nvdis
    {
        public const string Illegal = "<illegal>";
        static readonly string Exe = Environment.GetEnvironmentVariable("NVDISASM") ?? @"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\bin\nvdisasm.exe";
        static readonly Regex Line = new(@"^\s*/\*([0-9a-f]{4,})\*/\s*(.*?)\s*;\s*/\*\s*(0x[0-9a-f]{16})\s*\*/\s*$", RegexOptions.Compiled);
        static readonly Regex Ws = new(@"\s+", RegexOptions.Compiled);
        const int Batch = 64;

        public static string Norm(string s) => Ws.Replace(s, " ").Trim();

        /// text of each word as nvdisasm prints it ("... ;" without address), or Illegal.
        public static string[] Run(IReadOnlyList<(ulong lo, ulong hi)> words, int jobs)
        {
            var res = new string[words.Count];
            var chunks = new List<(int s, int n)>();
            for (int s = 0; s < words.Count; s += Batch) chunks.Add((s, Math.Min(Batch, words.Count - s)));
            Parallel.ForEach(chunks, new ParallelOptions { MaxDegreeOfParallelism = jobs }, c => Solve(words, c.s, c.n, res));
            return res;
        }

        static void Solve(IReadOnlyList<(ulong lo, ulong hi)> words, int s, int n, string[] res)
        {
            var got = Try(words, s, n);
            if (got != null) { for (int i = 0; i < n; i++) res[s + i] = got[i]; return; }
            if (n == 1) { res[s] = Illegal; return; }
            int h = n / 2;
            Solve(words, s, h, res);
            Solve(words, s + h, n - h, res);
        }

        static string[] Try(IReadOnlyList<(ulong lo, ulong hi)> words, int s, int n)
        {
            string tmp = Path.Combine(Path.GetTempPath(), $"xcheck_{Environment.ProcessId}_{Environment.CurrentManagedThreadId}.bin");
            var buf = new byte[n * 16];
            for (int i = 0; i < n; i++)
            {
                BitConverter.TryWriteBytes(buf.AsSpan(i * 16, 8), words[s + i].lo);
                BitConverter.TryWriteBytes(buf.AsSpan(i * 16 + 8, 8), words[s + i].hi);
            }
            File.WriteAllBytes(tmp, buf);
            var psi = new ProcessStartInfo(Exe) { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false, CreateNoWindow = true };
            psi.ArgumentList.Add("-b"); psi.ArgumentList.Add("SM86"); psi.ArgumentList.Add("-hex");
            if (s != 0) { psi.ArgumentList.Add("--base-address"); psi.ArgumentList.Add("0x" + (s * 16).ToString("x")); }
            psi.ArgumentList.Add(tmp);
            using var p = Process.Start(psi)!;
            var err = p.StandardError.ReadToEndAsync();
            string o = p.StandardOutput.ReadToEnd();
            p.WaitForExit();
            string e = err.Result;
            try { File.Delete(tmp); } catch { }
            if (p.ExitCode != 0 || e.Contains("error")) return null;
            var list = new List<string>();
            foreach (var ln in o.Split('\n'))
            {
                var m = Line.Match(ln.TrimEnd('\r'));
                if (m.Success) list.Add(Norm(m.Groups[2].Value + " ;"));
            }
            return list.Count == n ? list.ToArray() : null;
        }
    }
}
