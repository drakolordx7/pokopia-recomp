// SPDX-License-Identifier: MIT
// fp32 denormals for the forms without .FTZ. Ampere keeps fp32 denormals in FADD/FMUL/FFMA/FMNMX/FSETP/FSET unless .FTZ is set;
// the host driver flushes them, and the RTX 3080 cannot be told otherwise (VkPhysicalDeviceFloatControlsProperties,
// driver 617.42: shaderDenormPreserveFloat32 = false, so the SPIR-V DenormPreserve execution mode is not allowed). These forms
// therefore take a slow path when an input is a denormal or the host result is zero or tiny: the exact operation in fp64
// (fp32 denormals are normal there), its rounding error as a sticky bit, and the rounding to fp32 done by hand where the
// result is a denormal. The .FTZ forms keep the host's flushing (unchanged).
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;
using Op = Nx.Shaders.Ampere.Operand;
using IrOperand = Ryujinx.Graphics.Shader.IntermediateRepresentation.Operand;
using Nx.Shaders.Ampere;

namespace Ryujinx.Graphics.Shader.Instructions
{
    static partial class InstEmit
    {
        const Instruction D = Instruction.FP64;

        /// fp32 source bits with |x| / -x applied as bit operations (no float instruction that could flush).
        static IrOperand FBits(EmitterContext ctx, Op o)
        {
            if (o.Kind == OperandKind.Imm) throw Unsup("integer immediate in a float operand");
            IrOperand v = Raw(ctx, o);
            if (o.Kind == OperandKind.FImm) return v;
            if (o.Abs) v = ctx.BitwiseAnd(v, Const(0x7fffffff));
            if (o.Neg) v = ctx.BitwiseExclusiveOr(v, Const(unchecked((int)0x80000000)));
            return v;
        }

        static IrOperand IsDenormBits(EmitterContext ctx, IrOperand bits) =>
            ctx.BitwiseAnd(ctx.ICompareEqual(ctx.BitwiseAnd(bits, Const(0x7f800000)), Const(0)), ctx.ICompareNotEqual(ctx.BitwiseAnd(bits, Const(0x7fffff)), Const(0)));

        /// true when the bits are a zero or a denormal (exponent field 0)
        static IrOperand IsTinyBits(EmitterContext ctx, IrOperand bits) => ctx.ICompareEqual(ctx.BitwiseAnd(bits, Const(0x7f800000)), Const(0));

        static IrOperand SelD(EmitterContext ctx, IrOperand c, IrOperand a, IrOperand b) =>
            ctx.PackDouble2x32(ctx.ConditionalSelect(c, ctx.UnpackDouble2x32Low(a), ctx.UnpackDouble2x32Low(b)),
                               ctx.ConditionalSelect(c, ctx.UnpackDouble2x32High(a), ctx.UnpackDouble2x32High(b)));

        /// The fp32 value of `bits` as fp64, exact for denormals too (the conversion instruction may flush them).
        static IrOperand ExactF64(EmitterContext ctx, IrOperand bits)
        {
            IrOperand tiny = IsTinyBits(ctx, bits);
            IrOperand m = ctx.FPMultiply(ctx.IConvertU32ToFP64(ctx.BitwiseAnd(bits, Const(0x7fffff))), ctx.PackDouble2x32(Math.ScaleB(1.0, -149)), D);
            IrOperand dv = SelD(ctx, ctx.ICompareLess(bits, Const(0)), ctx.FPNegate(m, D), m);
            return SelD(ctx, tiny, dv, ctx.FP32ConvertToFP64(bits));
        }

        /// fp32 bits of the exact value s + err (s = the fp64 rounding of it, err its exact error, 0 when s is exact) rounded with
        /// the instruction's mode; denormal results are built by hand.
        static IrOperand RoundToF32(EmitterContext ctx, SassInst i, IrOperand s, IrOperand err, IrOperand inputsFinite)
        {
            IrOperand zeroD = ctx.PackDouble2x32(0.0);
            // round to odd at 53 bits: an inexact s with an even last bit moves one ulp toward the exact value. Odd rounding keeps
            // "between two fp32 values" and "on one" apart, so the second rounding gives the correctly rounded fp32 result.
            IrOperand lo = ctx.UnpackDouble2x32Low(s), hi = ctx.UnpackDouble2x32High(s);
            IrOperand inexact = ctx.Add(Instruction.LogicalNot, Local(), ctx.FPCompareEqual(err, zeroD, D));
            IrOperand even = ctx.ICompareEqual(ctx.BitwiseAnd(lo, Const(1)), Const(0));
            IrOperand bigger = ctx.ICompareEqual(Bool(ctx, ctx.FPCompareLess(zeroD, err, D)), Bool(ctx, ctx.FPCompareLess(zeroD, s, D)));
            IrOperand nlo = ctx.ConditionalSelect(bigger, ctx.IAdd(lo, Const(1)), ctx.ISubtract(lo, Const(1)));
            IrOperand nhi = ctx.ConditionalSelect(ctx.BitwiseAnd(ctx.Add(Instruction.LogicalNot, Local(), bigger), ctx.ICompareEqual(lo, Const(0))), ctx.ISubtract(hi, Const(1)), hi);
            IrOperand sFinite = ctx.ICompareNotEqual(ctx.BitwiseAnd(hi, Const(0x7ff00000)), Const(0x7ff00000));
            IrOperand nudge = ctx.BitwiseAnd(ctx.BitwiseAnd(inexact, even), sFinite);
            lo = ctx.ConditionalSelect(nudge, nlo, lo);
            hi = ctx.ConditionalSelect(nudge, nhi, hi);
            IrOperand s2 = ctx.PackDouble2x32(lo, hi);
            IrOperand sign = ctx.BitwiseAnd(hi, Const(unchecked((int)0x80000000)));
            IrOperand neg = ctx.ICompareNotEqual(sign, Const(0));

            // normal range (and inf/NaN): the host conversion rounds to nearest; a directed mode moves it one ulp as in DirectedRound
            IrOperand rn = ctx.FP64ConvertToFP32(s2);
            if (Directed(i))
            {
                IrOperand back = ctx.FP32ConvertToFP64(rn);
                rn = DirectedRound(ctx, i, rn, ctx.FPCompareLess(s2, back, D), ctx.FPCompareLess(back, s2, D), inputsFinite);
            }

            // denormal range: |s2| * 2^149 is the result in units of the smallest denormal; round it to an integer
            IrOperand t = ctx.FPMultiply(ctx.FPAbsolute(s2, D), ctx.PackDouble2x32(Math.ScaleB(1.0, 149)), D);
            IrOperand tr;
            if (Has(i, "RZ")) tr = ctx.FPTruncate(t, D);
            else if (Has(i, "RP")) tr = SelD(ctx, neg, ctx.FPTruncate(t, D), ctx.FPCeiling(t, D));
            else if (Has(i, "RM")) tr = SelD(ctx, neg, ctx.FPCeiling(t, D), ctx.FPTruncate(t, D));
            else tr = ctx.FPRound(t, D);
            IrOperand den = ctx.BitwiseOr(sign, ctx.FP64ConvertToU32(tr));

            IrOperand small = ctx.FPCompareLess(ctx.FPAbsolute(s2, D), ctx.PackDouble2x32(Math.ScaleB(1.0, -126)), D);
            return ctx.ConditionalSelect(small, den, rn);
        }

        /// .SAT on fp32 bits without a float instruction: NaN and negative values (-0 included) give +0, values above 1 give 1.
        static IrOperand SatBits(EmitterContext ctx, IrOperand r)
        {
            IrOperand nan = ctx.ICompareGreaterUnsigned(ctx.BitwiseAnd(r, Const(0x7fffffff)), Const(0x7f800000));
            IrOperand low = ctx.BitwiseOr(nan, ctx.ICompareLess(r, Const(0)));
            return ctx.ConditionalSelect(low, Const(0), ctx.IMinimumU32(r, Const(0x3f800000)));
        }

        /// Writes `fast` to `dst`, or (when `slowWhen` holds) the result of `slow`, emitted behind a branch.
        static void DstOrSlow(EmitterContext ctx, Op dst, IrOperand fast, IrOperand slowWhen, Func<IrOperand> slow)
        {
            IrOperand lSlow = Label(), lEnd = Label();
            ctx.BranchIfTrue(lSlow, slowWhen);
            Dst(ctx, dst, fast);
            ctx.Branch(lEnd);
            ctx.MarkLabel(lSlow);
            Dst(ctx, dst, slow());
            ctx.MarkLabel(lEnd);
        }

        /// Exact sum s = a + b in fp64 and its error (Fast2Sum: |big| >= |small|, round to nearest).
        static (IrOperand s, IrOperand err) TwoSumD(EmitterContext ctx, IrOperand a, IrOperand b)
        {
            IrOperand s = ctx.FPAdd(a, b, D);
            IrOperand aBig = ctx.BitwiseOr(ctx.FPCompareLess(ctx.FPAbsolute(b, D), ctx.FPAbsolute(a, D), D), ctx.FPCompareEqual(ctx.FPAbsolute(b, D), ctx.FPAbsolute(a, D), D));
            IrOperand big = SelD(ctx, aBig, a, b), sm = SelD(ctx, aBig, b, a);
            IrOperand err = ctx.FPSubtract(sm, ctx.FPSubtract(s, big, D), D);
            return (s, err);
        }

        /// Slow path of FADD/FMUL/FFMA without .FTZ: `bits` = the source bits (modifiers applied), `kind` 0 add, 1 mul, 2 fma.
        static IrOperand DenormArith(EmitterContext ctx, SassInst i, IrOperand[] bits, int kind, double scale = 1)
        {
            IrOperand a = ExactF64(ctx, bits[0]), b = ExactF64(ctx, bits[1]);
            IrOperand s, err, zeroD = ctx.PackDouble2x32(0.0);
            IrOperand plusZeros;
            if (kind == 0)
            {
                (s, err) = TwoSumD(ctx, a, b);
                plusZeros = ctx.BitwiseAnd(ctx.ICompareLessOrEqual(Const(0), bits[0]), ctx.ICompareLessOrEqual(Const(0), bits[1]));
            }
            else
            {
                IrOperand p = ctx.FPMultiply(a, b, D);                               // exact: 24 x 24 bits
                if (scale != 1) p = ctx.FPMultiply(p, ctx.PackDouble2x32(scale), D);  // exact power of two
                if (kind == 1) { s = p; err = zeroD; plusZeros = null; }
                else
                {
                    IrOperand c = ExactF64(ctx, bits[2]);
                    (s, err) = TwoSumD(ctx, p, c);
                    IrOperand prodPlus = ctx.Add(Instruction.LogicalNot, Local(), ctx.ICompareLess(ctx.BitwiseExclusiveOr(bits[0], bits[1]), Const(0)));
                    plusZeros = ctx.BitwiseAnd(prodPlus, ctx.ICompareLessOrEqual(Const(0), bits[2]));
                }
            }
            IrOperand finite = AllFinite(ctx, bits);
            IrOperand r = RoundToF32(ctx, i, s, err, finite);
            if (plusZeros != null) r = RmZero(ctx, i, r, ctx.FPCompareEqual(s, zeroD, D), plusZeros);
            return Has(i, "SAT") ? SatBits(ctx, r) : r;
        }

        /// Slow path needed: an input is a denormal or the host result is zero or tiny.
        static IrOperand NeedsDenormPath(EmitterContext ctx, IrOperand r, params IrOperand[] bits)
        {
            IrOperand c = IsTinyBits(ctx, r);
            foreach (IrOperand b in bits) c = ctx.BitwiseOr(c, IsDenormBits(ctx, b));
            return c;
        }

        /// FMNMX without .FTZ with a denormal input: the order of the bit patterns (sign-magnitude to two's complement), NaN
        /// inputs give the other operand.
        static IrOperand DenormMinMax(EmitterContext ctx, IrOperand a, IrOperand b, IrOperand isMin)
        {
            IrOperand Key(IrOperand x) => ctx.BitwiseExclusiveOr(x, ctx.BitwiseAnd(ctx.ShiftRightS32(x, Const(31)), Const(0x7fffffff)));
            IrOperand Nan(IrOperand x) => ctx.ICompareGreaterUnsigned(ctx.BitwiseAnd(x, Const(0x7fffffff)), Const(0x7f800000));
            IrOperand aLess = ctx.ICompareLess(Key(a), Key(b));
            IrOperand pickA = ctx.ICompareEqual(Bool(ctx, aLess), Bool(ctx, isMin));
            IrOperand r = ctx.ConditionalSelect(pickA, a, b);
            return ctx.ConditionalSelect(Nan(a), b, ctx.ConditionalSelect(Nan(b), a, r));
        }
    }
}
