// SPDX-License-Identifier: MIT
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace Nx.Shaders.XCheck
{
    static class Names
    {
        // nxnative emitter name (leading 'A' dropped, upper case) -> the mnemonics our decoder prints for the same opcode. Anything not listed must match
        // after dropping a leading 'U' (uniform datapath forms share the vector emitter).
        static readonly Dictionary<string, string[]> Alias = new()
        {
            ["IMADWIDE"] = new[] { "IMAD" },
            ["IMADHI"] = new[] { "IMAD" },
            ["IABS"] = new[] { "IABS" },
            ["FSEL"] = new[] { "FSEL" },
            ["VOTE"] = new[] { "VOTE", "VOTEU" },
            ["ST LOCALSHARED"] = Array.Empty<string>(),
            ["STLOCALSHARED"] = new[] { "STL", "STS" },
            ["ATOMSCAS"] = new[] { "ATOMS" },
            ["LDSM"] = new[] { "LDSM" },
            ["UP2UR"] = new[] { "UP2UR" },
            ["UMOV"] = new[] { "UMOV" },
            ["PLOP3"] = new[] { "PLOP3", "UPLOP3" },
            ["F2FP"] = new[] { "F2FP" },
            ["I2FP"] = new[] { "I2FP" },
            ["F2I64"] = new[] { "F2I" },
            ["I2F64"] = new[] { "I2F" },
            ["HMNMX2"] = new[] { "HMNMX2" },
            ["MOVM"] = new[] { "MOVM" },
            ["HMMA"] = new[] { "HMMA" },
            ["R2UR"] = new[] { "R2UR" },
        };

        public static bool Match(string ours, string emitter)
        {
            string n = emitter.StartsWith("A", StringComparison.Ordinal) ? emitter[1..] : emitter;
            n = n.ToUpperInvariant();
            if (ours == n) return true;
            if (Alias.TryGetValue(n, out var al) && al.Contains(ours)) return true;
            string o = ours.StartsWith("U", StringComparison.Ordinal) ? ours[1..] : ours;
            string nn = n.StartsWith("U", StringComparison.Ordinal) ? n[1..] : n;
            return o == nn || o == n || ours == nn;
        }
    }
}
