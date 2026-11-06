import sys
ops = [
 ("f32","add.f32 %d, fa, fb;"),("f32","add.ftz.f32 %d, fa, fb;"),("f32","add.rz.f32 %d, fa, fb;"),("f32","add.rm.ftz.f32 %d, fa, fb;"),
 ("f32","sub.rp.f32 %d, fa, fb;"),("f32","add.sat.f32 %d, fa, fb;"),
 ("f32","mul.f32 %d, fa, fb;"),("f32","mul.ftz.f32 %d, fa, fb;"),("f32","mul.rz.ftz.f32 %d, fa, fb;"),("f32","mul.sat.f32 %d, fa, fb;"),
 ("f32","fma.rn.f32 %d, fa, fb, fc;"),("f32","fma.rn.ftz.f32 %d, fa, fb, fc;"),("f32","fma.rz.f32 %d, fa, fb, fc;"),("f32","fma.rn.sat.f32 %d, fa, fb, fc;"),
 ("f32","fma.rn.f32 %d, fa, 0f3F000000, fc;"),("f32","fma.rm.f32 %d, fa, fb, fc;"),("f32","fma.rp.f32 %d, fa, fb, fc;"),
 ("f32","min.f32 %d, fa, fb;"),("f32","max.ftz.f32 %d, fa, fb;"),("f32","min.NaN.f32 %d, fa, fb;"),("f32","max.xorsign.abs.f32 %d, fa, fb;"),
 ("f32","abs.f32 %d, fa;"),("f32","neg.ftz.f32 %d, fa;"),
 ("pred","setp.lt.f32 %d, fa, fb;"),("pred","setp.gtu.ftz.f32 %d, fa, fb;"),("pred","setp.num.f32 %d, fa, fb;"),("pred","setp.nan.f32 %d, fa, fb;"),("pred","setp.equ.f32 %d, fa, fb;"),
 ("u32","set.ge.u32.f32 %d, fa, fb;"),("f32","set.neu.f32.f32 %d, fa, fb;"),
 ("f32","cvt.rni.f32.f32 %d, fa;"),("f32","cvt.rmi.f32.f32 %d, fa;"),("f32","cvt.rpi.ftz.f32.f32 %d, fa;"),("f32","cvt.rzi.f32.f32 %d, fa;"),
 ("f32","sin.approx.f32 %d, fa;"),("f32","cos.approx.ftz.f32 %d, fa;"),("f32","ex2.approx.f32 %d, fa;"),("f32","ex2.approx.ftz.f32 %d, fa;"),("f32","lg2.approx.f32 %d, fa;"),
 ("f32","rsqrt.approx.f32 %d, fa;"),("f32","rsqrt.approx.ftz.f32 %d, fa;"),("f32","sqrt.approx.f32 %d, fa;"),("f32","rcp.approx.f32 %d, fa;"),("f32","rcp.approx.ftz.f32 %d, fa;"),("f32","tanh.approx.f32 %d, fa;"),
 ("b16","ex2.approx.f16 %d, hl;"),("b16","tanh.approx.f16 %d, hl;"),
 ("s32","cvt.rzi.s32.f32 %d, fa;"),("u32","cvt.rni.u32.f32 %d, fa;"),("s32","cvt.rmi.ftz.s32.f32 %d, fa;"),("u32","cvt.rpi.u32.f32 %d, fa;"),
 ("u16","cvt.rzi.u16.f32 %d, fa;"),("s16","cvt.rni.s16.f32 %d, fa;"),("s64","cvt.rzi.s64.f32 %d, fa;"),("u64","cvt.rni.u64.f64 %d, da;"),("s32","cvt.rzi.s32.f64 %d, da;"),
 ("f32","cvt.rn.f32.s32 %d, a;"),("f32","cvt.rz.f32.u32 %d, a;"),("f32","cvt.rm.f32.s16 %d, sl;"),("f32","cvt.rn.f32.u32 %d, ul8;"),("f32","cvt.rn.f32.s64 %d, la;"),("f64","cvt.rn.f64.s32 %d, a;"),
 ("b16","cvt.rn.f16.f32 %d, fa;"),("b16","cvt.rz.f16.f32 %d, fa;"),("f32","cvt.f32.f16 %d, hl;"),("f32","cvt.rn.f32.f64 %d, da;"),("f64","cvt.f64.f32 %d, fa;"),("f32","cvt.ftz.sat.f32.f32 %d, fa;"),
 ("b32","cvt.rn.f16x2.f32 %d, fa, fb;"),("b32","cvt.rn.relu.f16x2.f32 %d, fa, fb;"),("b32","cvt.rn.bf16x2.f32 %d, fa, fb;"),
 ("s32","cvt.s8.s32 %d, a;"),("u32","cvt.sat.u8.s32 %d, a;"),("s32","cvt.s16.u32 %d, a;"),
 ("f64","add.f64 %d, da, db;"),("f64","mul.rz.f64 %d, da, db;"),("f64","fma.rn.f64 %d, da, db, dc;"),("pred","setp.lt.f64 %d, da, db;"),("f64","min.f64 %d, da, db;"),
 ("b32","add.f16x2 %d, ha, hb;"),("b32","add.ftz.sat.f16x2 %d, ha, hb;"),("b32","mul.f16x2 %d, ha, hb;"),("b32","fma.rn.f16x2 %d, ha, hb, hc;"),("b32","fma.rn.relu.f16x2 %d, ha, hb, hc;"),
 ("b32","min.f16x2 %d, ha, hb;"),("b32","max.NaN.f16x2 %d, ha, hb;"),("b16","add.f16 %d, hl, hh;"),("b32","set.lt.f16x2.f16x2 %d, ha, hb;"),("b32","set.gtu.u32.f16x2 %d, ha, hb;"),
("b32","fma.rn.bf16x2 %d, ha, hb, hc;"),
 ("u32","add.u32 %d, a, b;"),("u32","sub.u32 %d, a, b;"),("s32","add.sat.s32 %d, a, b;"),("u64","add.u64 %d, la, lb;"),("u32","add.cc.u32 tmp, a, b; addc.u32 %d, c, a;"),
 ("u32","mad.lo.u32 %d, a, b, c;"),("u32","mad.hi.u32 %d, a, b, c;"),("s32","mad.hi.s32 %d, a, b, c;"),("u64","mad.wide.u32 %d, a, b, lc;"),("s64","mad.wide.s32 %d, a, b, lc;"),
 ("u32","mul.hi.u32 %d, a, b;"),("s32","mul.hi.s32 %d, a, b;"),("u32","mul.lo.u32 %d, a, 0x12345;"),("u64","mul.lo.u64 %d, la, lb;"),
 ("u32","min.u32 %d, a, b;"),("s32","max.s32 %d, a, b;"),("s32","abs.s32 %d, a;"),("s32","neg.s32 %d, a;"),
 ("u32","lop3.b32 %d, a, b, c, 0x96;"),("u32","lop3.b32 %d, a, b, c, 0xe8;"),("u32","not.b32 %d, a;"),
 ("u32","shf.l.wrap.b32 %d, a, b, c;"),("u32","shf.r.clamp.b32 %d, a, b, c;"),("u32","shf.r.wrap.b32 %d, a, b, c;"),("u32","shf.l.clamp.b32 %d, a, b, c;"),
 ("u32","shl.b32 %d, a, c;"),("u32","shr.u32 %d, a, c;"),("s32","shr.s32 %d, a, c;"),("u64","shl.b64 %d, la, c;"),("s64","shr.s64 %d, la, c;"),("u64","shr.u64 %d, la, 5;"),
 ("u32","prmt.b32 %d, a, b, c;"),("u32","prmt.b32.f4e %d, a, b, c;"),("u32","prmt.b32.b4e %d, a, b, c;"),("u32","prmt.b32.rc8 %d, a, b, c;"),("u32","prmt.b32.ecl %d, a, b, c;"),("u32","prmt.b32.ecr %d, a, b, c;"),("u32","prmt.b32.rc16 %d, a, b, c;"),
 ("u32","bfe.u32 %d, a, b, c;"),("s32","bfe.s32 %d, a, b, c;"),("u32","bfi.b32 %d, a, b, c, ul8;"),("u32","bmsk.clamp.b32 %d, a, b;"),("u32","bmsk.wrap.b32 %d, a, b;"),("s32","szext.clamp.s32 %d, a, b;"),("u32","szext.wrap.u32 %d, a, b;"),
 ("u32","popc.b32 %d, a;"),("u32","clz.b32 %d, a;"),("u32","bfind.u32 %d, a;"),("u32","bfind.s32 %d, a;"),("u32","bfind.shiftamt.u32 %d, a;"),("u32","brev.b32 %d, a;"),
 ("u32","dp4a.u32.u32 %d, a, b, c;"),("s32","dp2a.lo.s32.s32 %d, a, b, c;"),("u32","sad.u32 %d, a, b, c;"),
 ("s32","cvt.sat.s8.s32 %d, a;"),("u32","cvt.sat.u16.s32 %d, a;"),("s32","cvt.sat.s16.s32 %d, a;"),
 ("s32","dp4a.s32.s32 %d, a, b, c;"),("s32","dp4a.u32.s32 %d, a, b, c;"),("u32","dp2a.hi.u32.u32 %d, a, b, c;"),("s32","dp2a.lo.s32.u32 %d, a, b, c;"),("s32","sad.s32 %d, a, b, c;"),
 ("pred","setp.lt.s32 %d, a, b;"),("pred","setp.hs.u32 %d, a, b;"),("pred","setp.ne.u64 %d, la, lb;"),("pred","setp.lt.s64 %d, la, lb;"),
 ("u32","selp.b32 %d, a, b, pa;"),("pred","and.pred %d, pa, pb;"),("pred","xor.pred %d, pa, pb;"),
 ("u32","shfl.sync.idx.b32 %d, a, b, 0x1f, 0xffffffff;"),("u32","shfl.sync.up.b32 %d, a, 3, 0, 0xffffffff;"),("u32","shfl.sync.down.b32 %d, a, b, 0x1f, 0xffffffff;"),("u32","shfl.sync.bfly.b32 %d, a, 5, 0x1f, 0xffffffff;"),
 ("u32","shfl.sync.idx.b32 %d|pt, a, b, c, 0xffffffff;"),
 ("pred","vote.sync.any.pred %d, pa, 0xffffffff;"),("pred","vote.sync.all.pred %d, pa, 0xffffffff;"),("pred","vote.sync.uni.pred %d, pa, 0xffffffff;"),("u32","vote.sync.ballot.b32 %d, pa, 0xffffffff;"),
 ("u32","match.any.sync.b32 %d, a, 0xffffffff;"),("u32","match.all.sync.b32 %d|pt, a, 0xffffffff;"),
 ("u32","redux.sync.add.u32 %d, a, 0xffffffff;"),("u32","redux.sync.min.u32 %d, a, 0xffffffff;"),("s32","redux.sync.max.s32 %d, a, 0xffffffff;"),("u32","redux.sync.or.b32 %d, a, 0xffffffff;"),("u32","redux.sync.xor.b32 %d, a, 0xffffffff;"),
]
regdecl = {"f32":".f32","u32":".u32","s32":".s32","b32":".b32","f64":".f64","u64":".u64","s64":".s64","u16":".u16","s16":".s16","b16":".b16","pred":".pred"}
out = ["""// Instruction source for the hardware oracle (tools/shaders/hwcheck): the ALU forms we want encodings of, written in PTX
// and compiled by ptxas -arch=sm_86. hwcheck harvests the SASS words of the compiled kernel (cuobjdump -sass), so these
// encodings are NVIDIA compiler output from our own source. Inputs come from memory so nothing folds; every result is stored.
.version 8.0
.target sm_86
.address_size 64
.visible .entry ops(.param .u64 p)
{
  .reg .b32 a, b, c, tmp, ha, hb, hc; .reg .f32 fa, fb, fc; .reg .f64 da, db, dc; .reg .b64 la, lb, lc, rd<4>;
  .reg .b16 hl, hh; .reg .s16 sl; .reg .u32 ul8; .reg .pred pa, pb, pt; .reg .u32 t;
  ld.param.u64 rd0, [p];
  cvta.to.global.u64 rd0, rd0;
  mov.u32 t, %tid.x;
  mul.wide.u32 rd1, t, 64;
  add.s64 rd2, rd0, rd1;
  ld.global.u32 a, [rd2]; ld.global.u32 b, [rd2+4]; ld.global.u32 c, [rd2+8];
  ld.global.u32 ha, [rd2+12]; ld.global.u32 hb, [rd2+16]; ld.global.u32 hc, [rd2+20];
  ld.global.f32 fa, [rd2+24]; ld.global.f32 fb, [rd2+28]; ld.global.f32 fc, [rd2+32];
  ld.global.f64 da, [rd2+40]; ld.global.f64 db, [rd2+48]; ld.global.f64 dc, [rd2+56];
  mov.b64 la, da; mov.b64 lb, db; mov.b64 lc, dc;
  mov.b32 {hl, hh}, ha; cvt.u16.u32 sl, b; and.b32 ul8, c, 255;
  setp.ne.u32 pa, a, 0; setp.lt.u32 pb, b, c;"""]
for k,(ty,ins) in enumerate(ops):
    body = ins.replace("%d","d")
    if ty=="pred":
        out.append(f"  {{ .reg .pred d; {body} selp.u32 t, 1, 0, d; st.global.u32 [rd2+{k*8}], t; }}")
    else:
        out.append(f"  {{ .reg {regdecl[ty]} d; {body} st.global{regdecl[ty]} [rd2+{k*8}], d; }}")
# tensor-core forms: whole blocks (multi-register operands; ldmatrix takes a shared address from a register)
tensor = [
 "{ .reg .b32 d<2>; mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 {d0,d1}, {ha,hb,hc,a}, {b,c}, {ha,hb}; st.global.v2.u32 [rd2+%o], {d0,d1}; }",
 "{ .reg .b32 d<2>, z; mov.b32 z, 0; mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 {d0,d1}, {ha,hb,hc,a}, {b,c}, {z,z}; st.global.v2.u32 [rd2+%o], {d0,d1}; }",
 "{ .reg .b32 d<2>; mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16 {d0,d1}, {ha,hb}, {b}, {hc,a}; st.global.v2.u32 [rd2+%o], {d0,d1}; }",
 "{ .reg .f32 d<4>; mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {d0,d1,d2,d3}, {ha,hb,hc,a}, {b,c}, {fa,fb,fc,fa}; st.global.v4.f32 [rd2+%o], {d0,d1,d2,d3}; }",
 "{ .reg .f32 d<4>; mma.sync.aligned.m16n8k8.row.col.f32.f16.f16.f32 {d0,d1,d2,d3}, {ha,hb}, {b}, {fa,fb,fc,fa}; st.global.v4.f32 [rd2+%o], {d0,d1,d2,d3}; }",
 "{ .reg .f32 d<4>; mma.sync.aligned.m16n8k16.row.col.f32.bf16.bf16.f32 {d0,d1,d2,d3}, {ha,hb,hc,a}, {b,c}, {fa,fb,fc,fa}; st.global.v4.f32 [rd2+%o], {d0,d1,d2,d3}; }",
 "{ .reg .b32 d<4>; ldmatrix.sync.aligned.m8n8.x4.shared.b16 {d0,d1,d2,d3}, [a]; st.global.v4.u32 [rd2+%o], {d0,d1,d2,d3}; }",
 "{ .reg .b32 d<4>; ldmatrix.sync.aligned.m8n8.x4.trans.shared.b16 {d0,d1,d2,d3}, [a+16]; st.global.v4.u32 [rd2+%o], {d0,d1,d2,d3}; }",
 "{ .reg .b32 d<2>; ldmatrix.sync.aligned.m8n8.x2.shared.b16 {d0,d1}, [a+32]; st.global.v2.u32 [rd2+%o], {d0,d1}; }",
 "{ .reg .b32 d<2>; ldmatrix.sync.aligned.m8n8.x2.trans.shared.b16 {d0,d1}, [a]; st.global.v2.u32 [rd2+%o], {d0,d1}; }",
 "{ .reg .b32 d; ldmatrix.sync.aligned.m8n8.x1.shared.b16 {d}, [a+64]; st.global.u32 [rd2+%o], d; }",
 "{ .reg .b32 d; ldmatrix.sync.aligned.m8n8.x1.trans.shared.b16 {d}, [a]; st.global.u32 [rd2+%o], d; }",
 "{ .reg .b32 d; movmatrix.sync.aligned.m8n8.trans.b16 d, ha; st.global.u32 [rd2+%o], d; }",
]
for k, blk in enumerate(tensor):
    out.append("  " + blk.replace("%o", str((len(ops) + k) * 16)))
out.append("  ret;\n}\n")
open(sys.argv[1],'w',newline='\n').write("\n".join(out))
print(len(ops) + len(tensor))
