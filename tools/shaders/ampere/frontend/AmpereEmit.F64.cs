// SPDX-License-Identifier: MIT
// fp64 arithmetic and compares (DADD, DMUL, DFMA, DSETP) and the 64-bit conversions (F2F/I2F/F2I with F64, S64 or U64).
// Register pairs: the even register holds the low word. Directed rounding (.RZ/.RM/.RP) of DADD and DMUL moves the
// round-to-nearest result one ulp by the sign of its exact error (TwoSum, FMA); hwcheck, RTX 3080.
using Nx.Shaders.Ampere;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Linq;
using Ryujinx.Graphics.Shader.Decoders;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitAluHelper;
using Op = Nx.Shaders.Ampere.Operand;
using IrOperand = Ryujinx.Graphics.Shader.IntermediateRepresentation.Operand;

namespace Ryujinx.Graphics.Shader.Instructions
{
    static partial class InstEmit
    {
        /// (low, high) words of an fp64 source with |x| / -x applied to the sign bit.
        static (IrOperand lo, IrOperand hi) DWords(EmitterContext ctx, Op o)
        {
            IrOperand lo, hi;
            if (o.Kind == OperandKind.FImm) { lo = Const((int)(uint)o.ImmBits); hi = Const((int)(uint)(o.ImmBits >> 32)); }
            else if (o.Kind == OperandKind.Imm) throw Unsup("integer immediate in an fp64 operand");
            else { lo = Raw(ctx, o, 0); hi = Raw(ctx, o, 1); }
            if (o.Kind != OperandKind.FImm)
            {
                if (o.Abs) hi = ctx.BitwiseAnd(hi, Const(0x7fffffff));
                if (o.Neg) hi = ctx.BitwiseExclusiveOr(hi, Const(unchecked((int)0x80000000)));
            }
            return (lo, hi);
        }

        static IrOperand DSrc(EmitterContext ctx, Op o) { var (lo, hi) = DWords(ctx, o); return ctx.PackDouble2x32(lo, hi); }

        static void DDst(EmitterContext ctx, Op o, IrOperand d)
        {
            DstWord(ctx, o, 0, ctx.UnpackDouble2x32Low(d));
            DstWord(ctx, o, 1, ctx.UnpackDouble2x32High(d));
        }

        static IrOperand FiniteHi(EmitterContext ctx, IrOperand hi) => ctx.ICompareNotEqual(ctx.BitwiseAnd(hi, Const(0x7ff00000)), Const(0x7ff00000));

        /// The round-to-nearest fp64 result r moved one ulp when the exact value lies on the wrong side for the mode
        /// (`below` / `above`: exact < r / exact > r). Finite overflow to infinity becomes the largest finite value where the mode
        /// says so; an exact zero is -0 under .RM unless `plusZeros`.
        static IrOperand DirectedRoundD(EmitterContext ctx, SassInst i, IrOperand r, IrOperand below, IrOperand above, IrOperand inputsFinite)
        {
            IrOperand Not(IrOperand b) => ctx.Add(Instruction.LogicalNot, Local(), b);
            IrOperand lo = ctx.UnpackDouble2x32Low(r), hi = ctx.UnpackDouble2x32High(r);
            IrOperand neg = ctx.ICompareLess(hi, Const(0));
            IrOperand magHi = ctx.BitwiseAnd(hi, Const(0x7fffffff));
            IrOperand nonzero = ctx.BitwiseOr(ctx.ICompareNotEqual(magHi, Const(0)), ctx.ICompareNotEqual(lo, Const(0)));
            IrOperand rFinite = FiniteHi(ctx, hi);
            below = ctx.BitwiseAnd(below, rFinite);
            above = ctx.BitwiseAnd(above, rFinite);
            IrOperand overflow = ctx.BitwiseAnd(ctx.BitwiseAnd(ctx.ICompareEqual(magHi, Const(0x7ff00000)), ctx.ICompareEqual(lo, Const(0))), inputsFinite);
            IrOperand towardZero = ctx.BitwiseOr(ctx.BitwiseAnd(Not(neg), below), ctx.BitwiseAnd(neg, above));
            IrOperand dec, inc;
            if (Has(i, "RZ")) { dec = ctx.BitwiseOr(ctx.BitwiseAnd(towardZero, nonzero), overflow); inc = Const(IrConsts.False); }
            else if (Has(i, "RM"))
            {
                dec = ctx.BitwiseOr(ctx.BitwiseAnd(ctx.BitwiseAnd(Not(neg), below), nonzero), ctx.BitwiseAnd(overflow, Not(neg)));
                inc = ctx.BitwiseAnd(neg, below);
            }
            else
            {
                dec = ctx.BitwiseOr(ctx.BitwiseAnd(ctx.BitwiseAnd(neg, above), nonzero), ctx.BitwiseAnd(overflow, neg));
                inc = ctx.BitwiseAnd(Not(neg), above);
            }
            // magnitude +1 / -1 ulp on the 64-bit pattern
            IrOperand incLo = ctx.IAdd(lo, Const(1)), incHi = ctx.IAdd(hi, Bool(ctx, ctx.ICompareEqual(incLo, Const(0))));
            IrOperand decLo = ctx.ISubtract(lo, Const(1)), decHi = ctx.ISubtract(hi, Bool(ctx, ctx.ICompareEqual(lo, Const(0))));
            // from a zero, one ulp up is the smallest denormal of the result's sign
            lo = ctx.ConditionalSelect(dec, decLo, ctx.ConditionalSelect(inc, incLo, lo));
            hi = ctx.ConditionalSelect(dec, decHi, ctx.ConditionalSelect(inc, incHi, hi));
            return ctx.PackDouble2x32(lo, hi);
        }

        static void EmitDArithAmpere(EmitterContext ctx, SassInst i)
        {
            IReadOnlyList<Op> o = i.Operands;
            IrOperand a = DSrc(ctx, o[1]), b = DSrc(ctx, o[2]);
            IrOperand r, zeroD = ctx.PackDouble2x32(0.0);
            IrOperand finite = ctx.BitwiseAnd(FiniteHi(ctx, DWords(ctx, o[1]).hi), FiniteHi(ctx, DWords(ctx, o[2]).hi));
            switch (i.Opcode)
            {
                case "DADD":
                    {
                        r = ctx.FPAdd(a, b, D);
                        if (Directed(i))
                        {
                            (_, IrOperand err) = TwoSumD(ctx, a, b);
                            r = DirectedRoundD(ctx, i, r, ctx.FPCompareLess(err, zeroD, D), ctx.FPCompareLess(zeroD, err, D), finite);
                            if (Has(i, "RM"))
                            {
                                // exact zero sum: -0 unless both terms are +0
                                IrOperand plus = ctx.BitwiseAnd(ctx.ICompareLessOrEqual(Const(0), DWords(ctx, o[1]).hi), ctx.ICompareLessOrEqual(Const(0), DWords(ctx, o[2]).hi));
                                IrOperand z = ctx.FPCompareEqual(r, zeroD, D);
                                r = ctx.PackDouble2x32(ctx.UnpackDouble2x32Low(r), ctx.ConditionalSelect(z, ctx.ConditionalSelect(plus, Const(0), Const(unchecked((int)0x80000000))), ctx.UnpackDouble2x32High(r)));
                            }
                        }
                        break;
                    }
                case "DMUL":
                    {
                        r = ctx.FPMultiply(a, b, D);
                        if (Directed(i))
                        {
                            // err = a*b - r exactly (fused)
                            IrOperand err = ctx.FPFusedMultiplyAdd(a, b, ctx.FPNegate(r, D), D);
                            IrOperand rn = r;
                            r = DirectedRoundD(ctx, i, r, ctx.FPCompareLess(err, zeroD, D), ctx.FPCompareLess(zeroD, err, D), finite);
                            // Denormal results: the FMA error is not exact there. Scale the smaller operand by 2^1074 (exact, no
                            // overflow when the product is that small), so the product is in units of the smallest denormal; round
                            // that to an integer with the mode (t = RN of it, e = its exact error) and scale back (exact).
                            IrOperand aSmall = ctx.FPCompareLess(ctx.FPAbsolute(a, D), ctx.FPAbsolute(b, D), D);
                            IrOperand sm = SelD(ctx, aSmall, a, b), bg = SelD(ctx, aSmall, b, a);
                            IrOperand sc = ctx.FPMultiply(ctx.FPMultiply(sm, ctx.PackDouble2x32(Math.ScaleB(1.0, 537)), D), ctx.PackDouble2x32(Math.ScaleB(1.0, 537)), D);
                            IrOperand t = ctx.FPMultiply(sc, bg, D);
                            IrOperand e = ctx.FPFusedMultiplyAdd(sc, bg, ctx.FPNegate(t, D), D);
                            IrOperand ti = ctx.FPTruncate(t, D);
                            IrOperand isInt = ctx.FPCompareEqual(ti, t, D);
                            IrOperand one = ctx.PackDouble2x32(1.0), q;
                            if (Has(i, "RZ"))
                            {
                                // toward zero: drop the fraction; an integer t whose exact value is nearer zero steps one toward zero
                                IrOperand tPos = ctx.FPCompareLess(zeroD, t, D);
                                IrOperand inward = ctx.BitwiseAnd(isInt, ctx.BitwiseOr(ctx.BitwiseAnd(tPos, ctx.FPCompareLess(e, zeroD, D)), ctx.BitwiseAnd(ctx.FPCompareLess(t, zeroD, D), ctx.FPCompareLess(zeroD, e, D))));
                                q = SelD(ctx, inward, SelD(ctx, tPos, ctx.FPSubtract(ti, one, D), ctx.FPAdd(ti, one, D)), ti);
                            }
                            else if (Has(i, "RM"))
                            {
                                IrOperand f = ctx.FPFloor(t, D);
                                q = SelD(ctx, ctx.BitwiseAnd(isInt, ctx.FPCompareLess(e, zeroD, D)), ctx.FPSubtract(f, one, D), f);
                            }
                            else
                            {
                                IrOperand f = ctx.FPCeiling(t, D);
                                q = SelD(ctx, ctx.BitwiseAnd(isInt, ctx.FPCompareLess(zeroD, e, D)), ctx.FPAdd(f, one, D), f);
                            }
                            IrOperand den = ctx.FPMultiply(ctx.FPMultiply(q, ctx.PackDouble2x32(Math.ScaleB(1.0, -537)), D), ctx.PackDouble2x32(Math.ScaleB(1.0, -537)), D);
                            // keep the sign of a zero result
                            IrOperand sgn = ctx.BitwiseAnd(ctx.BitwiseExclusiveOr(DWords(ctx, o[1]).hi, DWords(ctx, o[2]).hi), Const(unchecked((int)0x80000000)));
                            den = ctx.PackDouble2x32(ctx.UnpackDouble2x32Low(den), ctx.BitwiseOr(ctx.BitwiseAnd(ctx.UnpackDouble2x32High(den), Const(0x7fffffff)), sgn));
                            IrOperand tiny = ctx.FPCompareLess(ctx.FPAbsolute(rn, D), ctx.PackDouble2x32(Math.ScaleB(1.0, -1021)), D);
                            // A denormal operand with a normal result: the FMA error is below the denormal grid too. Scale that
                            // operand by 2^128 (normal then), round there and scale back (exact for a normal result).
                            IrOperand sc2 = ctx.FPMultiply(sm, ctx.PackDouble2x32(Math.ScaleB(1.0, 128)), D);
                            IrOperand t2 = ctx.FPMultiply(sc2, bg, D);
                            IrOperand e2 = ctx.FPFusedMultiplyAdd(sc2, bg, ctx.FPNegate(t2, D), D);
                            IrOperand r2 = DirectedRoundD(ctx, i, t2, ctx.FPCompareLess(e2, zeroD, D), ctx.FPCompareLess(zeroD, e2, D), finite);
                            r2 = ctx.FPMultiply(r2, ctx.PackDouble2x32(Math.ScaleB(1.0, -128)), D);
                            IrOperand smDen = ctx.FPCompareLess(ctx.FPAbsolute(sm, D), ctx.PackDouble2x32(Math.ScaleB(1.0, -1022)), D);
                            r = SelD(ctx, ctx.BitwiseAnd(smDen, finite), r2, r);
                            r = SelD(ctx, ctx.BitwiseAnd(tiny, finite), den, r);
                        }
                        break;
                    }
                default:
                    {
                        if (Directed(i)) throw Unsup("DFMA rounding mode");
                        r = ctx.FPFusedMultiplyAdd(a, b, DSrc(ctx, o[3]), D);
                        break;
                    }
            }
            DDst(ctx, o[0], r);
        }

        static void EmitDsetpAmpere(EmitterContext ctx, SassInst i)
        {
            // DSETP.cmp.bop Pd, Pd2, a, b, Pc. MIN / MAX (min.f64 / max.f64): Pd = "a is the result" (b NaN, or a < b / a > b, or
            // equal with a = -0 / +0; false when both are NaN), Pd2 = both NaN (the caller then quiets the selected NaN).
            IReadOnlyList<Op> o = i.Operands;
            BoolOp bop = BopOf(i);
            IrOperand a = DSrc(ctx, o[2]), b = DSrc(ctx, o[3]), pc = Src(ctx, o[4]);
            IrOperand Not(IrOperand x) => ctx.Add(Instruction.LogicalNot, Local(), x);
            if (Has(i, "MIN") || Has(i, "MAX"))
            {
                bool min = Has(i, "MIN");
                IrOperand an = ctx.IsNan(a, D), bn = ctx.IsNan(b, D);
                IrOperand strict = min ? ctx.FPCompareLess(a, b, D) : ctx.FPCompareLess(b, a, D);
                IrOperand aNeg = ctx.ICompareLess(DWords(ctx, o[2]).hi, Const(0));
                IrOperand tie = ctx.BitwiseAnd(ctx.FPCompareEqual(a, b, D), min ? aNeg : Not(aNeg));
                IrOperand pick = ctx.BitwiseAnd(Not(an), ctx.BitwiseOr(bn, ctx.BitwiseOr(strict, tie)));   // both NaN: b (hwcheck)
                Dst(ctx, o[0], GetPredLogicalOp(ctx, bop, pick, pc));
                if (!(IsPredKind(o[1]) && o[1].Reg == 7)) Dst(ctx, o[1], GetPredLogicalOp(ctx, bop, ctx.BitwiseAnd(an, bn), pc));
                return;
            }
            int c = Array.FindIndex(FCompNames, n => i.Mods.Contains(n));
            if (c < 0) throw Unsup("no comparison");
            IrOperand cmp = GetFPComparison(ctx, (FComp)c, a, b, D);
            Dst(ctx, o[0], GetPredLogicalOp(ctx, bop, cmp, pc));
            if (!(IsPredKind(o[1]) && o[1].Reg == 7)) Dst(ctx, o[1], GetPredLogicalOp(ctx, bop, Not(cmp), pc));
        }

        /// F2F with an F64 side: F64 <- F32 (exact) or F32 <- F64 (rounded with the mode, denormal results by hand).
        static void EmitF2f64Ampere(EmitterContext ctx, SassInst i, bool toF64)
        {
            IReadOnlyList<Op> o = i.Operands;
            if (toF64)
            {
                IrOperand bits = FBits(ctx, o[1]);
                if (Has(i, "FTZ")) bits = ctx.ConditionalSelect(IsTinyBits(ctx, bits), ctx.BitwiseAnd(bits, Const(unchecked((int)0x80000000))), bits);
                DDst(ctx, o[0], ExactF64(ctx, bits));
                return;
            }
            var (lo, hi) = DWords(ctx, o[1]);
            IrOperand r = RoundToF32(ctx, i, ctx.PackDouble2x32(lo, hi), ctx.PackDouble2x32(0.0), FiniteHi(ctx, hi));
            if (Has(i, "FTZ")) r = ctx.ConditionalSelect(IsTinyBits(ctx, r), ctx.BitwiseAnd(r, Const(unchecked((int)0x80000000))), r);
            Dst(ctx, o[0], r);
        }

        /// I2F with a 64-bit side: F64 <- S32/U32 (exact), F64 <- S64/U64 and F32 <- S64/U64 (rounded with the mode).
        static void EmitI2f64Ampere(EmitterContext ctx, SassInst i)
        {
            IReadOnlyList<Op> o = i.Operands;
            bool dst64 = Has(i, "F64"), src64 = Has(i, "S64") || Has(i, "U64");
            bool unsigned = Has(i, "U64") || Has(i, "U32") || Has(i, "U16") || Has(i, "U8");
            if (!src64)
            {
                if (Has(i, "U8") || Has(i, "S8") || Has(i, "U16") || Has(i, "S16")) throw Unsup("sub-word source");
                IrOperand v = Raw(ctx, o[1]);
                DDst(ctx, o[0], unsigned ? ctx.IConvertU32ToFP64(v) : ctx.IConvertS32ToFP64(v));
                return;
            }
            // value = hi * 2^32 + lo (hi signed for S64): both parts exact in fp64, one rounding in the sum, TwoSum for its error
            IrOperand lo = Raw(ctx, o[1], 0), hi = Raw(ctx, o[1], 1);
            IrOperand dh = ctx.FPMultiply(unsigned ? ctx.IConvertU32ToFP64(hi) : ctx.IConvertS32ToFP64(hi), ctx.PackDouble2x32(4294967296.0), D);
            (IrOperand s, IrOperand err) = TwoSumD(ctx, dh, ctx.IConvertU32ToFP64(lo));
            IrOperand zeroD = ctx.PackDouble2x32(0.0);
            if (dst64)
            {
                IrOperand r = Directed(i) ? DirectedRoundD(ctx, i, s, ctx.FPCompareLess(err, zeroD, D), ctx.FPCompareLess(zeroD, err, D), Const(IrConsts.True)) : s;
                DDst(ctx, o[0], r);
            }
            else Dst(ctx, o[0], RoundToF32(ctx, i, s, err, Const(IrConsts.True)));
        }

        /// F2I with a 64-bit side: S32/U32 <- F64, S64/U64 <- F32/F64. Saturating; NaN gives the most negative value (S32, S64) or
        /// 0x8000000000000000 (U64); U32 from NaN gives 0 (not checked on hardware).
        static void EmitF2i64Ampere(EmitterContext ctx, SassInst i)
        {
            IReadOnlyList<Op> o = i.Operands;
            IrOperand v, nan;
            if (Has(i, "F64"))
            {
                // NaN from the source bits (the rounding keeps it a NaN)
                var (slo, shi) = DWords(ctx, o[1]);
                IrOperand mh = ctx.BitwiseAnd(shi, Const(0x7fffffff));
                nan = ctx.BitwiseOr(ctx.ICompareGreaterUnsigned(mh, Const(0x7ff00000)), ctx.BitwiseAnd(ctx.ICompareEqual(mh, Const(0x7ff00000)), ctx.ICompareNotEqual(slo, Const(0))));
                v = ctx.PackDouble2x32(slo, shi);
            }
            else
            {
                IrOperand bits = FBits(ctx, o[1]);
                if (Has(i, "FTZ")) bits = ctx.ConditionalSelect(IsTinyBits(ctx, bits), ctx.BitwiseAnd(bits, Const(unchecked((int)0x80000000))), bits);
                v = ExactF64(ctx, bits);
                nan = ctx.ICompareGreaterUnsigned(ctx.BitwiseAnd(bits, Const(0x7fffffff)), Const(0x7f800000));
            }
            v = Has(i, "TRUNC") ? ctx.FPTruncate(v, D) : Has(i, "FLOOR") ? ctx.FPFloor(v, D) : Has(i, "CEIL") ? ctx.FPCeiling(v, D) : ctx.FPRound(v, D);
            bool dst64 = Has(i, "S64") || Has(i, "U64");
            bool unsigned = Has(i, "U64") || Has(i, "U32") || Has(i, "U16") || Has(i, "U8");
            if (!dst64)
            {
                if (Has(i, "U16") || Has(i, "U8") || Has(i, "S16") || Has(i, "S8")) throw Unsup("sub-word destination");
                IrOperand lim = unsigned ? ctx.FPMaximum(ctx.FPMinimum(v, ctx.PackDouble2x32(4294967295.0), D), ctx.PackDouble2x32(0.0), D)
                                         : ctx.FPMaximum(ctx.FPMinimum(v, ctx.PackDouble2x32(2147483647.0), D), ctx.PackDouble2x32(-2147483648.0), D);
                IrOperand r32 = unsigned ? ctx.FP64ConvertToU32(lim) : ctx.FP64ConvertToS32(lim);
                Dst(ctx, o[0], ctx.ConditionalSelect(nan, Const(unsigned ? 0 : int.MinValue), r32));
                return;
            }
            IrOperand Not(IrOperand x) => ctx.Add(Instruction.LogicalNot, Local(), x);
            IrOperand neg = ctx.FPCompareLess(v, ctx.PackDouble2x32(0.0), D);
            IrOperand m = ctx.FPAbsolute(v, D);
            IrOperand big = unsigned ? ctx.FPCompareLess(ctx.PackDouble2x32(18446744073709549568.0), m, D)       // >= 2^64
                                     : ctx.BitwiseOr(ctx.BitwiseAnd(Not(neg), ctx.FPCompareLess(ctx.PackDouble2x32(9223372036854774784.0), m, D)),
                                                     ctx.BitwiseAnd(neg, ctx.FPCompareLess(ctx.PackDouble2x32(9223372036854775808.0), m, D)));
            IrOperand mc = ctx.FPMinimum(m, ctx.PackDouble2x32(18446744073709549568.0), D);   // 2^64 - 2048: hi fits 32 bits
            IrOperand hiD = ctx.FPFloor(ctx.FPMultiply(mc, ctx.PackDouble2x32(1.0 / 4294967296.0), D), D);
            IrOperand loD = ctx.FPSubtract(mc, ctx.FPMultiply(hiD, ctx.PackDouble2x32(4294967296.0), D), D);
            IrOperand hw = ctx.FP64ConvertToU32(hiD), lw = ctx.FP64ConvertToU32(loD);
            if (unsigned)
            {
                // negative values (large ones too) give 0
                hw = ctx.ConditionalSelect(big, Const(-1), hw); lw = ctx.ConditionalSelect(big, Const(-1), lw);
                hw = ctx.ConditionalSelect(neg, Const(0), hw); lw = ctx.ConditionalSelect(neg, Const(0), lw);
            }
            else
            {
                // two's complement of the magnitude
                IrOperand nlw = ctx.INegate(lw), nhw = ctx.IAdd(ctx.BitwiseNot(hw), Bool(ctx, ctx.ICompareEqual(lw, Const(0))));
                lw = ctx.ConditionalSelect(neg, nlw, lw); hw = ctx.ConditionalSelect(neg, nhw, hw);
                lw = ctx.ConditionalSelect(big, ctx.ConditionalSelect(neg, Const(0), Const(-1)), lw);
                hw = ctx.ConditionalSelect(big, ctx.ConditionalSelect(neg, Const(int.MinValue), Const(int.MaxValue)), hw);
            }
            lw = ctx.ConditionalSelect(nan, Const(0), lw);
            hw = ctx.ConditionalSelect(nan, Const(int.MinValue), hw);   // NaN: 0x8000000000000000 for U64 too (hwcheck)
            DstWord(ctx, o[0], 0, lw);
            DstWord(ctx, o[0], 1, hw);
        }
    }
}
