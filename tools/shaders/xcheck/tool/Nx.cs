// SPDX-License-Identifier: MIT
// nxnative's Ampere front end (seedds/nxnative, MIT), driven as a black box: its opcode table (AmpereTable) and its instruction emitters, whose
// Ryujinx IR output is serialised into a signature string. Two words with equal signatures translate identically; a bit whose flip changes the
// signature is a bit the emitter depends on.
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Text;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.CodeGen;
using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;

namespace Nx.Shaders.XCheck
{
    sealed class StubAccessor : IGpuAccessor
    {
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => default;
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, index + 1);
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) => new(3, 0);
        public SetBindingPair CreateStorageBufferBinding(int index) => new(1, index);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) => new(2, 0);
        public int QuerySamplerArrayLengthFromPool() => 1;
        public int QueryTextureArrayLengthFromBuffer(int slot) => 1;
        public int QueryTextureArrayLengthFromPool() => 1;
    }

    static class Nx
    {
        public static readonly string[] Emitter = new string[4096];   // emitter method name per 12-bit opcode, null = no entry
        public static readonly string[] Name = new string[4096];      // Ryujinx InstName
        public static readonly bool[] Uniform = new bool[4096];       // uniform-datapath guard (12..15 names a UP)

        static Nx()
        {
            for (int op = 0; op < 4096; op++)
            {
                var o = AmpereTable.GetOp(0, (ulong)op, 0);
                if (o.Name == InstName.Invalid) continue;
                Emitter[op] = o.Emitter.Method.Name;
                Name[op] = o.Name.ToString();
                Uniform[op] = AmpereTable.HasUniformGuard(op);
            }
        }

        [ThreadStatic] static TranslatorContext tctx;
        [ThreadStatic] static ResourceManager rm;
        [ThreadStatic] static DecodedProgram prog;

        static void Init()
        {
            var acc = new StubAccessor();
            var defs = new ShaderDefinitions(ShaderStage.Compute, 8, 8, 1);
            var opts = new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, TranslationFlags.Ampere | TranslationFlags.Compute);
            var main = new DecodedFunction(0);
            main.SetBlocks(new Ryujinx.Graphics.Shader.Decoders.Block[] { new Ryujinx.Graphics.Shader.Decoders.Block(0) });
            prog = new DecodedProgram(main, new Dictionary<ulong, DecodedFunction> { [0] = main }, new AttributeUsage(acc), FeatureFlags.None, 0, 0);
            tctx = (TranslatorContext)Activator.CreateInstance(typeof(TranslatorContext),
                BindingFlags.NonPublic | BindingFlags.Instance | BindingFlags.Public, null,
                new object[] { 0UL, 16, 0, defs, acc, opts, prog }, null);
            rm = new ResourceManager(ShaderStage.Compute, acc);
        }

        /// IR the emitter produces for the word, as text. "EXC:..." when the emitter throws, "NONE" when the opcode has no entry.
        public static string Signature(ulong lo, ulong hi)
        {
            var op = AmpereTable.GetOp(0, lo, hi);
            if (op.Name == InstName.Invalid) return "NONE";
            if (tctx == null) Init();
            Operation[] ops;
            try
            {
                var ctx = new EmitterContext(tctx, rm, prog, false, false);
                int before = ctx.OperationsCount;
                ctx.CurrOp = op;
                op.Emitter(ctx);
                ops = ctx.GetOperations();
                return Serialise(ops, before);
            }
            catch (Exception e)
            {
                var m = e.Message;
                int p = m.LastIndexOf(" (", StringComparison.Ordinal);
                if (p > 0) m = m[..p];
                int c = m.IndexOf(':');
                if (c > 0 && m.StartsWith("Ampere ", StringComparison.Ordinal)) m = m[(c + 1)..].Trim();
                return "EXC:" + e.GetType().Name + ":" + m;
            }
        }

        static string Serialise(Operation[] ops, int before)
        {
            var sb = new StringBuilder();
            var locals = new Dictionary<Operand, int>();
            for (int i = before; i < ops.Length; i++)
            {
                var o = ops[i];
                sb.Append(o.Inst).Append(o.StorageKind != default ? "." + o.StorageKind : "");
                for (int d = 0; d < o.DestsCount; d++) sb.Append(d == 0 ? " " : ",").Append(Fmt(o.GetDest(d), locals));
                sb.Append(" <-");
                for (int s = 0; s < o.SourcesCount; s++) sb.Append(' ').Append(Fmt(o.GetSource(s), locals));
                sb.Append(';');
            }
            return sb.ToString();
        }

        static string Fmt(Operand o, Dictionary<Operand, int> locals)
        {
            if (o == null) return "null";
            switch (o.Type)
            {
                case OperandType.Constant: return "#" + o.Value.ToString("x");
                case OperandType.Register:
                    var r = o.GetRegister();
                    return r.Type + ":" + r.Index;
                case OperandType.ConstantBuffer: return $"c[{o.GetCbufSlot()}][{o.GetCbufOffset():x}]";
                case OperandType.LocalVariable:
                case OperandType.Label:
                    if (!locals.TryGetValue(o, out int id)) locals[o] = id = locals.Count;
                    return (o.Type == OperandType.Label ? "L" : "t") + id;
                default: return o.Type + ":" + o.Value.ToString("x");
            }
        }
    }
}
